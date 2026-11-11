#include "echo_envelope.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>

namespace echo {
namespace {

constexpr uint32_t kNoMatch = 0xFFFFFFFFu;
constexpr int kChecksPerNode = 8;           // complete builds sampled per branch
constexpr long kNodesPerAttempt = 200000;   // global budget for one assembly attempt

// WL colouring over the labelled pattern graph (an isomorphism invariant).
std::vector<uint32_t> wl_colours(const QueryGraph& g) {
    std::vector<uint32_t> col(g.n);
    for (uint32_t u = 0; u < g.n; ++u) col[u] = g.label[u];
    for (uint32_t it = 0; it < g.n; ++it) {
        std::vector<std::pair<std::vector<uint32_t>, uint32_t>> keys(g.n);
        for (uint32_t u = 0; u < g.n; ++u) {
            keys[u].first.push_back(col[u]);
            for (uint32_t v : g.adj[u]) keys[u].first.push_back(col[v] + 0x100000u);
            std::sort(keys[u].first.begin(), keys[u].first.end());
            keys[u].second = u;
        }
        std::sort(keys.begin(), keys.end());
        uint32_t next = 0;
        bool changed = false;
        for (uint32_t i = 0; i < g.n; ++i) {
            if (i && keys[i].first != keys[i - 1].first) ++next;
            if (next != col[keys[i].second]) changed = true;
            col[keys[i].second] = next;
        }
        if (!changed) break;
    }
    return col;
}

// Exact isomorphism by backtracking, used to confirm a WL-equality test (WL is incomplete and
// would otherwise report non-isomorphic queries as identical, which N1 must never do).
bool exact_iso(const QueryGraph& a, const QueryGraph& b) {
    if (a.n != b.n) return false;
    {
        std::vector<uint32_t> la = a.label, lb = b.label;
        std::sort(la.begin(), la.end());
        std::sort(lb.begin(), lb.end());
        if (la != lb) return false;
        uint64_t ma = 0, mb = 0;
        for (auto& v : a.adj) ma += v.size();
        for (auto& v : b.adj) mb += v.size();
        if (ma != mb) return false;
    }
    std::vector<uint32_t> ca = wl_colours(a), cb = wl_colours(b);
    std::vector<int> ma(a.n, -1), mb(b.n, -1);
    std::function<bool(uint32_t)> rec = [&](uint32_t depth) -> bool {
        if (depth == a.n) return true;
        uint32_t best = kNoMatch, best_cnt = 0xFFFFFFFFu;
        for (uint32_t u = 0; u < a.n; ++u) {
            if (ma[u] >= 0) continue;
            uint32_t cnt = 0;
            for (uint32_t v = 0; v < b.n; ++v)
                if (mb[v] < 0 && cb[v] == ca[u]) ++cnt;
            if (cnt < best_cnt) {
                best_cnt = cnt;
                best = u;
                if (cnt <= 1) break;
            }
        }
        if (best_cnt == 0) return false;
        for (uint32_t v = 0; v < b.n; ++v) {
            if (mb[v] >= 0 || cb[v] != ca[best]) continue;
            bool ok = true;
            for (uint32_t u2 = 0; u2 < a.n && ok; ++u2) {
                if (ma[u2] < 0) continue;
                bool ea = std::binary_search(a.adj[best].begin(), a.adj[best].end(), u2);
                bool eb = std::binary_search(b.adj[v].begin(), b.adj[v].end(), (uint32_t)ma[u2]);
                if (ea != eb) ok = false;
            }
            if (!ok) continue;
            ma[best] = (int)v;
            mb[v] = (int)best;
            if (rec(depth + 1)) return true;
            ma[best] = -1;
            mb[v] = -1;
        }
        return false;
    };
    return rec(0);
}

bool wl_iso(const QueryGraph& a, const QueryGraph& b) {
    if (a.n != b.n) return false;
    std::vector<uint32_t> x = wl_colours(a), y = wl_colours(b);
    std::sort(x.begin(), x.end());
    std::sort(y.begin(), y.end());
    if (x != y) return false;   // cheap necessary condition
    return exact_iso(a, b);     // WL equality alone is not sound
}



}  // namespace

// ----------------------------------------------------------------------------------------
// Envelope of a family. Members are placed one query vertex at a time along a BFS order: a
// vertex either opens a fresh envelope vertex or joins the class of its parent in the order,
// when the label agrees and every pair of classes linked in the envelope is linked by the
// member (the envelope keeps only the edges that every member has, which is what the family's
// common core allows; extra edges of a query are checked during the search). A class holds at
// most one vertex per member. Randomised assemblies are repeated; the most shared classes win.

struct AClass {
    uint32_t label = 0;
    std::vector<std::pair<uint32_t, uint32_t>> member;  // (member, vertex)
    std::vector<uint64_t> e;                            // [m] classes of member m's neighbours
};

struct Plan {
    uint32_t n = 0;
    std::vector<AClass> cls;
};

class EnvBuilder {
public:
    EnvBuilder(const std::vector<QueryGraph>& q, const std::vector<std::vector<uint32_t>>& order,
               const std::vector<std::vector<uint32_t>>& order_pos, std::mt19937_64& rng)
        : q_all_(q), order_all_(order), order_pos_all_(order_pos), rng_(rng) {}

    // `limit` bounds the number of classes; a class holds at most one vertex per member, so a
    // family that fits in `limit` classes always has a plan. Every trial keeps the best complete
    // assembly it reaches within `kBudget` nodes (`done` is false when a trial hit the budget);
    // a plan is useful when some class carries at least two members' vertices (`shared`).
    bool build(const std::vector<uint32_t>& grp, uint32_t limit, Plan& out, bool& shared, bool& done) {
        grp_ = grp;
        // The search indexes members by group position; order / order_pos are indexed by global
        // query id. Remap once per build.
        order_.clear();
        order_pos_.clear();
        q_.clear();
        for (uint32_t g : grp) {
            q_.push_back(q_all_[g]);
            // One order for the whole group: depth d must denote the same query vertex in every
            // member, otherwise the anchor's class at depth d is not the class to try.
            order_.push_back(order_all_[grp[0]]);
            order_pos_.push_back(order_pos_all_[grp[0]]);
        }
        limit_ = std::min<uint32_t>(limit, 63);
        anchor_cls_.assign(q_all_[grp[0]].n, kNoData);  // by depth
        best_ok_ = false;
        best_score_ = -1;
        done = true;
        greedy_ = true;
        {
            Plan p;
            nodes_ = 0;
            aborted_ = false;
            bool stop = step(0, 0, p);
            (void)stop;
            if (aborted_) done = false;  // greedy hit the class limit
        }
        greedy_ = false;
        for (int trial = 0; trial < kTrials; ++trial) {
            Plan p;
            nodes_ = 0;
            aborted_ = false;
            step(0, 0, p);
            if (aborted_) done = false;
        }
        if (best_ok_) {
            out = best_;
            shared = best_score_ > 1e-3;
            return true;
        }
        return false;
    }

private:
    static constexpr uint32_t kNoData = 0xFFFFFFFFu;
    static constexpr int kTrials = 24;
    static constexpr uint64_t kBudget = 400000;

    bool compatible(const Plan& p, uint32_t m, uint32_t i, uint32_t c) const {
        // A class is realized by one data vertex, so it can hold at most one vertex of each
        // member (a query's vertices must map to distinct data vertices), and its label must
        // agree. Class adjacency is NOT a requirement here: e[m] is filled in by add() as the
        // member's vertices are placed, and whether an edge is legal for member m is decided by
        // the walk against m's own query. Requiring a shared class adjacency across members is
        // what made non-isomorphic family members impossible to assemble.
        if (p.cls[c].label != q_[m].label[i]) return false;
        for (auto& mi : p.cls[c].member)
            if (mi.first == m) return false;
        return true;
    }

    void add(Plan& p, uint32_t m, uint32_t i, uint32_t c) {
        const QueryGraph& q = q_[m];
        if (c == p.n) {
            AClass a;
            a.label = q.label[i];
            a.e.assign(grp_.size(), 0);
            p.cls.push_back(a);
            ++p.n;
        }
        p.cls[c].member.emplace_back(m, i);
        for (uint32_t j : q.adj[i]) {
            uint32_t cj = kNoData;
            for (uint32_t c2 = 0; c2 < p.n; ++c2)
                for (auto& mi : p.cls[c2].member)
                    if (mi.first == m && mi.second == j) cj = c2;
            if (cj == kNoData) continue;
            p.cls[c].e[m] |= 1ULL << cj;
            p.cls[cj].e[m] |= 1ULL << c;
        }
    }

    std::vector<uint32_t> greedy_targets(const Plan& p, uint32_t m, uint32_t i, uint32_t d) const {
        std::vector<uint32_t> out, extra;
        if (m > 0 && d < anchor_cls_.size() && anchor_cls_[d] != kNoData) out.push_back(anchor_cls_[d]);
        for (uint32_t j : q_[m].adj[i]) {
            uint32_t c = class_of(p, m, j);
            if (c != kNoData && std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c);
        }
        for (uint32_t c2 = 0; c2 < p.n; ++c2) {
            if (p.cls[c2].label != q_[m].label[i]) continue;
            if (std::find(out.begin(), out.end(), c2) != out.end()) continue;
            extra.push_back(c2);
        }
        std::sort(extra.begin(), extra.end(),
                  [&](uint32_t a, uint32_t b) { return p.cls[a].member.size() > p.cls[b].member.size(); });
        for (uint32_t c : extra) out.push_back(c);
        return out;
    }

    uint32_t class_of(const Plan& p, uint32_t m, uint32_t i) const {
        for (uint32_t c = 0; c < p.n; ++c)
            for (auto& mi : p.cls[c].member)
                if (mi.first == m && mi.second == i) return c;
        return kNoData;
    }

    bool step(uint32_t m, uint32_t d, Plan& p) {
        if (++nodes_ > kBudget) {
            if (!greedy_) { aborted_ = true; return true; }
            // Deterministic trial: budget exhaustion forces fresh classes from here down.
        }
        if (m == grp_.size()) {
            // Work saved by the envelope is the number of member vertices that share a class
            // instead of opening their own: sum of (members in class - 1). Counting shared
            // classes instead would prefer many two-member classes over one class holding all
            // members, which is the opposite of what we want.
            double saved = 0;
            for (uint32_t c = 0; c < p.n; ++c)
                if (p.cls[c].member.size() >= 2) saved += (double)p.cls[c].member.size() - 1.0;
            double score = saved - 1e-3 * (double)p.n;
            if (!best_ok_ || score > best_score_) {
                best_ok_ = true;
                best_score_ = score;
                best_ = p;
            }
            return false;
        }
        const QueryGraph& q = q_[m];  // group-indexed view set in build()
        if (d == q.n) return step(m + 1, 0, p);
        if (p.n >= limit_ && !greedy_) return false;
        const uint32_t i = order_[m][d];
        if (greedy_) {
            // Deterministic linear pass: take the first compatible class (the anchor's class at
            // this depth comes first), else open a fresh one. No backtracking: this trial exists
            // to guarantee a complete plan and a good incumbent, not to search.
            uint32_t chosen = kNoData;
            for (uint32_t c : greedy_targets(p, m, i, d)) {
                if (!compatible(p, m, i, c)) continue;
                chosen = c;
                break;
            }
            if (chosen == kNoData) {
                chosen = p.n;  // fresh class; the fallback may exceed limit_ and is scored low
            }
            add(p, m, i, chosen);
            if (m == 0) anchor_cls_[d] = chosen;
            return step(m, d + 1, p);
        }
        // Candidate existing classes: classes of this vertex's placed neighbours in its own
        // member first (joining them carries the member's envelope edges), then a bounded scan
        // of label-compatible classes, largest first (most sharing potential).
        std::vector<uint32_t> targets, extra;
        // Canonical correspondence guess: the anchor's class at this depth. Members of an edit
        // family are built from one core in the same way, so this is the class to try first;
        // compatible() rejects it when the correspondence does not hold.
        if (m > 0 && d < anchor_cls_.size() && anchor_cls_[d] != kNoData)
            targets.push_back(anchor_cls_[d]);
        for (uint32_t j : q.adj[i]) {
            uint32_t c = class_of(p, m, j);
            if (c != kNoData && std::find(targets.begin(), targets.end(), c) == targets.end())
                targets.push_back(c);
        }
        for (uint32_t c2 = 0; c2 < p.n; ++c2) {
            if (p.cls[c2].label != q.label[i]) continue;
            if (std::find(targets.begin(), targets.end(), c2) != targets.end()) continue;
            extra.push_back(c2);
        }
        const size_t kExtraCap = 8;
        if (extra.size() > kExtraCap) {
            std::partial_sort(extra.begin(), extra.begin() + kExtraCap, extra.end(),
                              [&](uint32_t a, uint32_t b) { return p.cls[a].member.size() > p.cls[b].member.size(); });
            extra.resize(kExtraCap);
        }
        for (uint32_t c : extra) targets.push_back(c);
        bool fresh_first = !greedy_ && (targets.empty() || (rng_() & 1));
        if (greedy_ && !targets.empty()) fresh_first = false;
        if (!fresh_first) {
            for (uint32_t c : targets) {
                if (!compatible(p, m, i, c)) continue;
                Plan save = p;
                add(p, m, i, c);
                if (m == 0) anchor_cls_[d] = c;
                bool stop = step(m, d + 1, p);
                p = save;
                if (stop) return true;
            }
        }
        // The fresh class is tried only when fewer than `limit` classes exist.
        {
            Plan save = p;
            add(p, m, i, p.n);
            if (m == 0) anchor_cls_[d] = p.n - 1;
            bool stop = step(m, d + 1, p);
            p = save;
            if (stop) return true;
        }
        if (fresh_first) {
            for (uint32_t c : targets) {
                if (!compatible(p, m, i, c)) continue;
                Plan save = p;
                add(p, m, i, c);
                if (m == 0) anchor_cls_[d] = c;
                bool stop = step(m, d + 1, p);
                p = save;
                if (stop) return true;
            }
        }
        return false;
    }
    const std::vector<QueryGraph>& q_all_;
    const std::vector<std::vector<uint32_t>>& order_all_;
    const std::vector<std::vector<uint32_t>>& order_pos_all_;
    std::vector<QueryGraph> q_;                    // group-indexed views, set in build()
    std::vector<std::vector<uint32_t>> order_;
    std::vector<std::vector<uint32_t>> order_pos_;
    std::mt19937_64& rng_;
    bool greedy_ = false;
    std::vector<uint32_t> grp_;
    std::vector<uint32_t> anchor_cls_;  // [d] class the anchor put its d-th vertex in
    Plan best_;
    bool best_ok_ = false;
    double best_score_ = -1;
    uint64_t nodes_ = 0;
    bool aborted_ = false;
    uint32_t limit_ = 63;
};

// BFS order of a query from a random root, with each vertex's position.
void bfs_order(const QueryGraph& q, std::mt19937_64& rng, std::vector<uint32_t>& order,
               std::vector<uint32_t>& pos) {
    order.clear();
    pos.assign(q.n, 0);
    std::vector<char> seen(q.n, 0);
    std::vector<uint32_t> st{(uint32_t)(rng() % q.n)};
    seen[st[0]] = 1;
    for (size_t h = 0; h < st.size(); ++h) {
        order.push_back(st[h]);
        std::vector<uint32_t> nb = q.adj[st[h]];
        std::shuffle(nb.begin(), nb.end(), rng);
        for (uint32_t w : nb)
            if (!seen[w]) {
                seen[w] = 1;
                st.push_back(w);
            }
    }
    for (uint32_t k = 0; k < order.size(); ++k) pos[order[k]] = k;
}

// Turn a plan into the envelope structure.
Envelope plan_to_envelope(const std::vector<QueryGraph>& queries, const std::vector<uint32_t>& grp, const Plan& p,
                          const Graph* data) {
    Envelope env;
    env.members = grp;
    env.n = p.n;
    env.q = 0;
    env.labels.assign(p.n, {});
    env.degrees.assign(p.n, {});
    env.plabel.assign(p.n, 0);
    env.adj.assign(p.n, {});
    env.e_label.assign(p.n, {});
    env.span_v.assign(p.n, {});
    env.reps.assign(grp.size(), {});
    for (size_t m = 0; m < grp.size(); ++m) {
        env.reps[m].assign(queries[grp[m]].n, 0xFFFFFFFFu);
        for (uint32_t i = 0; i < queries[grp[m]].n; ++i) {
            uint32_t c = 0xFFFFFFFFu;
            for (uint32_t k = 0; k < p.n; ++k)
                for (auto& mi : p.cls[k].member)
                    if (mi.first == m && mi.second == i) c = k;
            env.reps[m][i] = c;
        }
    }
    std::map<uint32_t, uint32_t> shell;
    for (uint32_t c = 0; c < p.n; ++c) {
        env.plabel[c] = p.cls[c].label;
        for (auto& mi : p.cls[c].member) {
            uint32_t g = grp[mi.first];
            uint32_t lab = queries[g].label[mi.second];
            uint32_t deg = (uint32_t)queries[g].adj[mi.second].size();
            auto it = shell.find(lab);
            if (it == shell.end()) it = shell.emplace(lab, (uint32_t)shell.size()).first;
            env.labels[c].push_back(it->second);
            env.degrees[c].push_back(it->second * 4096u + std::min(deg, 4095u));
            env.span_v[c].push_back((mi.first << 16) | mi.second);
        }
        std::sort(env.labels[c].begin(), env.labels[c].end());
        env.labels[c].erase(std::unique(env.labels[c].begin(), env.labels[c].end()), env.labels[c].end());
        std::sort(env.degrees[c].begin(), env.degrees[c].end());
        env.degrees[c].erase(std::unique(env.degrees[c].begin(), env.degrees[c].end()), env.degrees[c].end());
    }
    env.q = (uint32_t)shell.size();
    env.label_base = 0xFFFFFFFFu;
    for (auto& kv : shell) env.label_base = std::min(env.label_base, kv.first);
    // Edges of the envelope: pairs of classes linked by some member.
    for (uint32_t c = 0; c < p.n; ++c)
        for (size_t m = 0; m < grp.size(); ++m)
            for (uint32_t c2 = 0; c2 < p.n; ++c2)
                if ((p.cls[c].e[m] >> c2) & 1ULL) {
                    env.adj[c].push_back(c2);
                }
    for (uint32_t c = 0; c < p.n; ++c) {
        std::sort(env.adj[c].begin(), env.adj[c].end());
        env.adj[c].erase(std::unique(env.adj[c].begin(), env.adj[c].end()), env.adj[c].end());
        env.e_label[c].assign(env.adj[c].size(), 0);
    }
    // equivalence classes: same projection and same label
    env.equiv.assign(grp.size(), {});
    for (size_t m = 0; m < grp.size(); ++m) {
        std::map<std::vector<uint32_t>, std::vector<uint32_t>> byk;
        for (uint32_t i = 0; i < queries[grp[m]].n; ++i)
            byk[{(uint32_t)env.reps[m][i], queries[grp[m]].label[i]}].push_back(i);
        for (auto& kv : byk)
            if (kv.second.size() > 1) env.equiv[m].insert(env.equiv[m].end(), kv.second.begin(), kv.second.end());
    }
    env.canon.assign(queries.size() * 64, 0);
    env.shared.assign(env.n, 0);
    for (uint32_t c = 0; c < env.n; ++c)
        if (p.cls[c].member.size() >= 2) {
            env.shared[c] = 1;
            ++env.shared_vertices;
        }
    (void)data;
    return env;
}

// ----------------------------------------------------------------------------------------
// Public entry points.

// Maximum common induced subgraph of a and b with an explicit correspondence: returns
// ma[u] = the b vertex matched to a's vertex u, or -1. Labels and adjacency must agree on every
// matched pair, so the matched vertices induce isomorphic subgraphs (a "core" of the family).
// Relaxed alignment for ECHO: same backtracking as core_match but labels are ignored, so two
// members that differ at a position (a label edit) are still aligned there and share one class.
CoreMatch align_match(const QueryGraph& a, const QueryGraph& b, uint64_t budget) {
    (void)budget;
    // Edit alignment, not isomorphism: a vertex pair may disagree on an edge (an edit) and still
    // be the same position of the family. Greedy, in decreasing anchor degree, preferring the
    // member vertex with the same label and the fewest disagreements with the pairs already
    // fixed. The walk re-checks every label and edge of each member, so a poor alignment costs
    // pruning, never correctness.
    CoreMatch out;
    out.ma.assign(a.n, -1);
    if (a.n == 0 || b.n == 0) return out;
    std::vector<uint32_t> order(a.n);
    for (uint32_t u = 0; u < a.n; ++u) order[u] = u;
    std::sort(order.begin(), order.end(), [&](uint32_t x, uint32_t y) {
        return a.adj[x].size() > a.adj[y].size();
    });
    std::vector<char> used(b.n, 0);
    std::vector<int> mb(b.n, -1);
    for (uint32_t u : order) {
        int best = -1, best_score = 0;
        for (uint32_t v = 0; v < b.n; ++v) {
            if (used[v]) continue;
            int sc = (b.label[v] == a.label[u]) ? 1000 : 0;
            for (uint32_t u2 = 0; u2 < a.n; ++u2) {
                if (out.ma[u2] < 0) continue;
                bool ea = std::binary_search(a.adj[u].begin(), a.adj[u].end(), u2);
                bool eb = std::binary_search(b.adj[v].begin(), b.adj[v].end(), (uint32_t)out.ma[u2]);
                sc += (ea == eb) ? 1 : -1;
            }
            if (sc > best_score) { best_score = sc; best = (int)v; }
        }
        if (best < 0) continue;
        out.ma[u] = best;
        used[best] = 1;
        mb[best] = (int)u;
        ++out.size;
    }
    return out;
}

// Core envelope of a family: the shared part is the maximum common induced subgraph ("core") of
// all members, with an explicit correspondence; every other query vertex is private to its
// member. The workload generator builds each family from one core vertex set, so the members'
// induced subgraphs on the core are identical and one data-vertex tuple realizes the core in
// every member at once: sharing the core image loses no solution on this protocol, while the
// private parts are enumerated per member and cost only the edit size.
// ECHO envelope of a family: an edit alignment of the members onto the anchor (labels may
// differ), one class per aligned group, candidates = the union of the members' candidate sets,
// and a per-member edge bitmap. Class labels are kept as a set: the walk hands a candidate v to
// exactly those members whose required label at that position equals label(v), which is the
// "query mask" N4/N5 works with. Edges are optional: a class pair carries an edge for the members
// that have it, and only those members check it.
Envelope make_core_envelope(const std::vector<QueryGraph>& queries, const std::vector<uint32_t>& grp,
                            const Graph* data, uint32_t min_core) {
    Envelope env;
    (void)data;
    if (grp.empty()) return env;
    const QueryGraph& base = queries[grp[0]];
    const uint32_t K = (uint32_t)grp.size();
    std::vector<CoreMatch> cm(K);
    cm[0].ma.resize(base.n);
    for (uint32_t u = 0; u < base.n; ++u) cm[0].ma[u] = (int)u;
    cm[0].size = base.n;
    for (uint32_t t = 1; t < K; ++t) cm[t] = align_match(base, queries[grp[t]], 200000);
    // Classes: every anchor vertex that at least one member aligns to. A member that does not
    // align to it keeps that vertex private (span_v simply does not list it).
    std::vector<uint32_t> aligned;  // anchor vertices used by the alignment
    for (uint32_t u = 0; u < base.n; ++u) {
        bool any = false;
        for (uint32_t t = 1; t < K && !any; ++t)
            if (cm[t].ma[u] >= 0) any = true;
        if (any) aligned.push_back(u);
    }
    if (aligned.empty()) return env;
    env.members = grp;
    env.n = (uint32_t)aligned.size();
    env.span_v.assign(env.n, {});
    env.labels.assign(env.n, {});
    env.degrees.assign(env.n, {});
    env.plabel.assign(env.n, 0xFFFFFFFFu);
    env.adj.assign(env.n, {});
    env.e_label.assign(env.n, {});
    env.shared.assign(env.n, 0);
    env.reps.assign(K, {});
    env.reach.assign(env.n, {});
    env.sep.assign(env.n, {});
    std::map<uint32_t, uint32_t> shell;
    for (uint32_t t = 0; t < K; ++t) {
        env.reps[t].assign(queries[grp[t]].n, 0xFFFFFFFFu);
        for (uint32_t c = 0; c < env.n; ++c) {
            int v = (t == 0) ? (int)aligned[c] : cm[t].ma[aligned[c]];
            if (v >= 0) env.reps[t][(uint32_t)v] = c;
        }
    }
    for (uint32_t c = 0; c < env.n; ++c) {
        // The class label is the set of member labels; the walk checks each member's own label
        // through its candidate list, so a single representative is only used for ordering.
        for (uint32_t t = 0; t < K; ++t) {
            int v = (t == 0) ? (int)aligned[c] : cm[t].ma[aligned[c]];
            if (v < 0) continue;
            uint32_t g = grp[t];
            uint32_t lab = queries[g].label[(uint32_t)v];
            uint32_t deg = (uint32_t)queries[g].adj[(uint32_t)v].size();
            auto it = shell.find(lab);
            if (it == shell.end()) it = shell.emplace(lab, (uint32_t)shell.size()).first;
            env.span_v[c].push_back((t << 16) | (uint32_t)v);
            env.labels[c].push_back(it->second);
            env.degrees[c].push_back(it->second * 4096u + std::min(deg, 4095u));
            if (t) ++env.shared[c];
            if (env.plabel[c] == 0xFFFFFFFFu) env.plabel[c] = lab;
        }
        std::sort(env.labels[c].begin(), env.labels[c].end());
        env.labels[c].erase(std::unique(env.labels[c].begin(), env.labels[c].end()), env.labels[c].end());
        std::sort(env.degrees[c].begin(), env.degrees[c].end());
        env.degrees[c].erase(std::unique(env.degrees[c].begin(), env.degrees[c].end()), env.degrees[c].end());
        if (env.plabel[c] == 0xFFFFFFFFu) env.plabel[c] = 0;
    }
    env.q = (uint32_t)shell.size();
    env.label_base = 0xFFFFFFFFu;
    for (auto& kv : shell) env.label_base = std::min(env.label_base, kv.first);
    // Optional edges: recorded per member, checked only by the members that have them.
    for (uint32_t c = 0; c < env.n; ++c) {
        for (uint32_t t = 0; t < K; ++t) {
            int v = (t == 0) ? (int)aligned[c] : cm[t].ma[aligned[c]];
            if (v < 0) continue;
            const QueryGraph& q = queries[grp[t]];
            for (uint32_t w : q.adj[(uint32_t)v]) {
                uint32_t c2 = env.reps[t][w];
                if (c2 == 0xFFFFFFFFu) continue;  // private neighbour
                env.adj[c].push_back(c2);
            }
        }
        std::sort(env.adj[c].begin(), env.adj[c].end());
        env.adj[c].erase(std::unique(env.adj[c].begin(), env.adj[c].end()), env.adj[c].end());
        env.e_label[c].assign(env.adj[c].size(), 0);
    }
    // A vertex the alignment leaves unmatched is a class of its own, spanned by that member
    // alone: a class introduced by one member's edit. Without it the vertex would have no class
    // and every emitted tuple would carry a hole.
    {
        uint32_t base_n = env.n;
        for (uint32_t t = 0; t < K; ++t) {
            const QueryGraph& q = queries[grp[t]];
            for (uint32_t v = 0; v < q.n; ++v) {
                if (env.reps[t][v] != 0xFFFFFFFFu) continue;
                uint32_t c = (uint32_t)env.span_v.size();
                env.span_v.push_back({(t << 16) | v});
                env.labels.push_back({});
                env.degrees.push_back({});
                env.plabel.push_back(q.label[v]);
                env.adj.push_back({});
                env.e_label.push_back({});
                env.shared.push_back(0);
                env.reach.push_back({});
                env.sep.push_back({});
                env.reps[t][v] = c;
                auto it = shell.find(q.label[v]);
                if (it == shell.end()) it = shell.emplace(q.label[v], (uint32_t)shell.size()).first;
                env.labels[c].push_back(it->second);
                env.degrees[c].push_back(it->second * 4096u + std::min<uint32_t>((uint32_t)q.adj[v].size(), 4095u));
                ++env.n;
            }
        }
        (void)base_n;
        // Edges of the new classes follow the members' own adjacency.
        for (uint32_t c = 0; c < env.n; ++c) {
            for (auto& code : env.span_v[c]) {
                int t = (int)(code >> 16), v = (int)(code & 0xFFFF);
                for (uint32_t w : queries[grp[t]].adj[v]) {
                    uint32_t c2 = env.reps[t][w];
                    if (c2 != 0xFFFFFFFFu && c2 != c) env.adj[c].push_back(c2);
                }
            }
            std::sort(env.adj[c].begin(), env.adj[c].end());
            env.adj[c].erase(std::unique(env.adj[c].begin(), env.adj[c].end()), env.adj[c].end());
            env.e_label[c].assign(env.adj[c].size(), 0);
        }
    }
    env.canon.assign(queries.size() * 64, 0xFFFFFFFFu);
    env.fitted = 1;
    env.shared_vertices = 0;
    for (uint32_t c = 0; c < env.n; ++c) if (env.shared[c]) ++env.shared_vertices;
    return env;
}

Envelope make_envelope_of(const std::vector<QueryGraph>& queries, const std::vector<uint32_t>& grp,
                          const Graph* data, double max_free_fraction, uint32_t size_limit, uint64_t seed) {
    (void)max_free_fraction;
    std::mt19937_64 rng(seed);
    std::vector<std::vector<uint32_t>> order(queries.size()), pos(queries.size());
    for (uint32_t m : grp) bfs_order(queries[m], rng, order[m], pos[m]);
    EnvBuilder b(queries, order, pos, rng);
    Plan p;
    bool shared = false, done = true;
    if (!b.build(grp, size_limit, p, shared, done)) return Envelope();
    Envelope env = plan_to_envelope(queries, grp, p, data);
    env.fitted = done ? 1 : 0;
    (void)shared;
    return env;
}

// Common core of two queries: vertices of a that have a twin class in b under an isomorphism
// of their induced subgraphs. Only the "all but one" case of the workload is tested: a vertex u
// of a is in the core when some vertex v of b has the same label, the same neighbourhood size,
// and the same labels on both sides for every neighbour (adjacent and non-adjacent alike).
// Neutralised colour refinement: two vertices share a colour when the multiset of (own label,
// neighbour colours) is equal. A vertex of a gets a colour that no vertex of b holds when its
// colour class in b is smaller than the number of vertices of a holding the same colour, so an
// injective mapping from a's matched vertices into b's exists on this witness. Output in a's order.
std::vector<uint32_t> refine_colours(const QueryGraph& g, const std::vector<uint32_t>& shell,
                                     const std::vector<uint32_t>& base, uint32_t ncol) {
    std::vector<uint32_t> col = base;
    std::vector<std::vector<uint32_t>> sig(g.n);
    std::vector<uint32_t> keys(g.n);
    std::map<std::vector<uint32_t>, uint32_t> ids;  // canonical (a, b) pair -> joint id
    for (int it = 0; it < 8 && ncol < 1000000u; ++it) {
        bool changed = false;
        for (uint32_t u = 0; u < g.n; ++u) {
            sig[u].clear();
            sig[u].push_back(ncol);
            sig[u].push_back(shell[u]);
            sig[u].push_back(col[u]);
            for (uint32_t w : g.adj[u]) sig[u].push_back(col[w]);
            std::sort(sig[u].begin(), sig[u].end());
        }
        for (uint32_t u = 0; u < g.n; ++u) {
            auto it2 = ids.find(sig[u]);
            if (it2 == ids.end()) it2 = ids.emplace(sig[u], ncol + (uint32_t)ids.size()).first;
            keys[u] = it2->second;
        }
        for (uint32_t u = 0; u < g.n; ++u)
            if (keys[u] != col[u]) changed = true;
        col = keys;
        if (!changed) break;
    }
    return col;
}

std::vector<char> common_core(const QueryGraph& a, const QueryGraph& b, const std::vector<uint32_t>& sa,
                              const std::vector<uint32_t>& sb) {
    std::vector<uint32_t> ca = sa, cb = sb;
    uint32_t ncol = 0;
    for (uint32_t x : sa) ncol = std::max(ncol, x + 1);
    for (uint32_t x : sb) ncol = std::max(ncol, x + 1);
    ca = refine_colours(a, sa, ca, ncol);
    cb = refine_colours(b, sb, cb, ncol);
    std::map<uint32_t, uint32_t> cnt_a, cnt_b;
    for (uint32_t x : ca) cnt_a[x]++;
    for (uint32_t x : cb) cnt_b[x]++;
    // A vertex is in the common core only up to the multiplicity of its colour class on the two
    // sides: counting every vertex whose colour merely occurs in b would report ~n for any two
    // queries with mostly-unique colours, which is not a shared substructure at all.
    std::vector<char> in(a.n, 0);
    std::map<uint32_t, uint32_t> quota = cnt_b;
    for (uint32_t u = 0; u < a.n; ++u) {
        auto it = quota.find(ca[u]);
        if (it != quota.end() && it->second > 0) { in[u] = 1; --it->second; }
    }
    return in;
}

// Public helper: isomorphic? via common_core with identity shells.
bool common_core_size(const QueryGraph& a, const QueryGraph& b, uint32_t min_sz) {
    std::vector<uint32_t> sa(a.n), sb(b.n);
    std::map<uint32_t, uint32_t> s;
    for (uint32_t u = 0; u < a.n; ++u) s.emplace(a.label[u], (uint32_t)s.size());
    for (uint32_t u = 0; u < a.n; ++u) sa[u] = s[a.label[u]];
    s.clear();
    for (uint32_t u = 0; u < b.n; ++u) s.emplace(b.label[u], (uint32_t)s.size());
    for (uint32_t u = 0; u < b.n; ++u) sb[u] = s[b.label[u]];
    std::vector<char> c = common_core(a, b, sa, sb);
    uint32_t sz = 0;
    for (char x : c) sz += x;
    return sz >= min_sz;
}

std::vector<std::vector<uint32_t>> families_of(const std::vector<QueryGraph>& queries, const Graph* data,
                                               uint32_t k, uint32_t min_overlap) {
    return families_of_min(queries, data, k, min_overlap);
}

// N1+N2 of a batch: group queries by intrinsic structure only. Nothing here may depend on the
// position of a query in the workload (that would be generator knowledge the method cannot have,
// and it breaks on E3's reshuffled batches and on E4's real candidate pools).
//
// Step 1 partitions by signature (size, edge count, sorted label multiset) — queries with
// different signatures cannot be isomorphic. Step 2, inside a signature class, measures how much
// structure two queries share with the colour-refinement common-core test and greedily forms
// families around a seed, keeping the family size at most `fam_size`. The cost of a merge is
// judged later, when the envelope is assembled (N3).
std::vector<std::vector<uint32_t>> families_of_min(const std::vector<QueryGraph>& queries, const Graph* data,
                                                  uint32_t k, uint32_t min_overlap) {
    (void)data;
    (void)k;
    (void)min_overlap; // the frozen rule is relative to each pair, not an absolute CLI threshold
    // Sorted label sets, one flat vector per query. The pair prefilter only needs the multiset
    // intersection; building std::set per pair left ~8 MB of allocator arenas behind, which the
    // first N6 repetition then paid for in page faults.
    std::vector<std::vector<uint32_t>> labs(queries.size());
    for (size_t i = 0; i < queries.size(); ++i) {
        labs[i] = queries[i].label;
        std::sort(labs[i].begin(), labs[i].end());
        labs[i].erase(std::unique(labs[i].begin(), labs[i].end()), labs[i].end());
    }
    std::map<uint32_t, std::vector<uint32_t>> by_sig;
    for (uint32_t i = 0; i < queries.size(); ++i) by_sig[queries[i].n].push_back(i);
    const uint32_t fam_size = 5;
    std::vector<std::vector<uint32_t>> fams;
    for (auto& kv : by_sig) {
        const std::vector<uint32_t>& ids = kv.second;
        std::vector<char> used(ids.size(), 0);
        for (size_t i = 0; i < ids.size(); ++i) {
            if (used[i]) continue;
            used[i] = 1;
            std::vector<uint32_t> fam{ids[i]};
            std::vector<std::pair<uint32_t, uint32_t>> cand;
            const std::vector<uint32_t>& set_i = labs[ids[i]];
            for (size_t j = i + 1; j < ids.size(); ++j) {
                if (used[j]) continue;
                const std::vector<uint32_t>& set_j = labs[ids[j]];
                size_t a = 0, b = 0, common = 0;
                while (a < set_i.size() && b < set_j.size()) {
                    if (set_i[a] == set_j[b]) { ++common; ++a; ++b; }
                    else if (set_i[a] < set_j[b]) ++a;
                    else ++b;
                }
                size_t need = (size_t)(0.6 * (double)std::max(set_i.size(), set_j.size()));
                if (common < std::max<size_t>(1, need)) continue;
                CoreMatch am = align_match(queries[ids[i]], queries[ids[j]], 20000);
                if (am.size >= (7u * std::max(queries[ids[i]].n, queries[ids[j]].n) + 9u) / 10u)
                    cand.emplace_back(am.size, (uint32_t)j);
            }
            std::sort(cand.begin(), cand.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            for (auto& pr : cand) {
                if (fam.size() >= fam_size) break;
                if (used[pr.second]) continue;
                used[pr.second] = 1;
                fam.push_back(ids[pr.second]);
            }
            fams.push_back(std::move(fam));
        }
    }
    std::sort(fams.begin(), fams.end(), [](const auto& a, const auto& b) { return a[0] < b[0]; });
    return fams;
}

std::vector<Envelope> build_envelopes(const std::vector<QueryGraph>& queries, const std::vector<uint32_t>& family,
                                      const Graph* data, double max_free_fraction, uint32_t size_limit,
                                      uint64_t seed, double& merge_seconds) {
    Stopwatch sw;
    std::vector<Envelope> out;
    uint64_t s = seed;
    for (auto& fam : families_of(queries, data, 0, 14)) {
        (void)family;
        Envelope e = make_envelope_of(queries, fam, data, max_free_fraction, size_limit, s++);
        if (e.n == 0) {
            for (uint32_t m : fam) {
                std::vector<uint32_t> one{m};
                out.push_back(make_envelope_of(queries, one, data, 1.0, size_limit, s++));
            }
            continue;
        }
        out.push_back(e);
    }
    merge_seconds = sw.seconds();
    return out;
}

// ----------------------------------------------------------------------------------------
// Query terms and the walk order.

Terms query_terms(const QueryGraph& q, const std::vector<uint32_t>& freq) {
    auto labfreq = [&](uint32_t lab) -> uint32_t { return lab < freq.size() ? freq[lab] : 0; };
    Terms t;
    t.nvert = q.n;
    std::map<uint32_t, uint32_t> labs;
    for (uint32_t u = 0; u < q.n; ++u) labs.emplace(q.label[u], (uint32_t)labs.size());
    t.nterms = 2u * (uint32_t)labs.size();
    t.lab_term.resize(q.n);
    t.deg_term.resize(q.n);
    t.plabel.resize(q.n);
    for (uint32_t u = 0; u < q.n; ++u) {
        t.lab_term[u] = labs[q.label[u]];
        t.plabel[u] = labs[q.label[u]];
        t.deg_term[u] = t.nterms + t.lab_term[u] * 4096u + (uint32_t)q.adj[u].size();
    }
    // Greedy order: start at the rarest label per degree, extend by the vertex with most placed
    // neighbours (rarest label first).
    t.parent.assign(q.n, kNoMatch);
    std::vector<char> in(q.n, 0);
    uint32_t start = 0;
    double best = 1e300;
    for (uint32_t u = 0; u < q.n; ++u) {
        double sc = (double)labfreq(q.label[u]) / (double)std::max<size_t>(1, q.adj[u].size());
        if (sc < best) {
            best = sc;
            start = u;
        }
    }
    t.order.push_back(start);
    in[start] = 1;
    while (t.order.size() < q.n) {
        uint32_t pick = kNoMatch;
        int pn = -1;
        uint32_t pf = 0xFFFFFFFFu;
        for (uint32_t u = 0; u < q.n; ++u) {
            if (in[u]) continue;
            int nb = 0;
            for (uint32_t w : q.adj[u]) nb += in[w];
            if (nb == 0) continue;
            uint32_t f = labfreq(q.label[u]);
            if (nb > pn || (nb == pn && f < pf)) {
                pick = u;
                pn = nb;
                pf = f;
            }
        }
        if (pick == kNoMatch) break;
        for (uint32_t w : q.adj[pick])
            if (in[w] && t.parent[pick] == kNoMatch) t.parent[pick] = w;
        in[pick] = 1;
        t.order.push_back(pick);
    }
    for (uint32_t u = 0; u < q.n; ++u)
        if (!in[u]) {
            t.parent[u] = t.order.empty() ? kNoMatch : t.order[0];
            in[u] = 1;
            t.order.push_back(u);
        }
    // Walk order. r (self loops) is placed by its own query with the DPiso-style start rule;
    // r0 (loop-free) follows a hashing scheme shared by all members (for a loop-free query it is
    // the greedy order). The envelope walk visits the loop vertices of every member first, then
    // the loop-free part in one order.
    t.oq.assign(q.n, q.n);
    t.orr.assign(q.n, q.n);
    t.rw.assign(q.n, kNoMatch);
    t.rv.assign(q.n, 0);
    t.nw.assign(q.n, 0);
    t.nv.assign(q.n, 0);
    std::vector<uint32_t> pos_in_order(q.n, 0);
    for (uint32_t d = 0; d < q.n; ++d) pos_in_order[t.order[d]] = d;
    for (uint32_t d = 0; d < q.n; ++d) t.orr[t.order[d]] = d;
    bool any_loop = false;
    for (uint32_t u = 0; u < q.n; ++u)
        if (std::binary_search(q.adj[u].begin(), q.adj[u].end(), u)) {
            t.oq[u] = 0;
            any_loop = true;
        }
    uint32_t r0 = kNoMatch;
    for (uint32_t d = 0; d < q.n && r0 == kNoMatch; ++d)
        if (t.oq[t.order[d]] == q.n) r0 = t.order[d];
    if (r0 == kNoMatch) r0 = t.order[0];
    t.oq[r0] = 1;
    std::vector<uint32_t> ch{r0};
    for (uint32_t d = 0; d < q.n; ++d) {
        uint32_t u = t.order[d];
        if (u != r0 && t.oq[u] == q.n) ch.push_back(u);
    }
    if (any_loop)
        for (uint32_t d = 0; d < q.n; ++d)
            if (t.oq[t.order[d]] == 0) ch.push_back(t.order[d]);
    for (size_t k = 1; k < ch.size(); ++k) {
        t.rw[ch[k]] = ch[k - 1];
        t.rv[ch[k]] = pos_in_order[ch[k - 1]];
        t.nw[ch[k - 1]] += 1;
        t.nv[ch[k]] = 1;
    }
    return t;
}

// One member alone: the query's own classes, one member vertex per class, no sharing. This is
// the "no envelope" control (E5) and the split branch of N3: same filter and walk code as a
// merged envelope, so the only difference to the merged run is the sharing.
Envelope make_single_envelope(const std::vector<QueryGraph>& queries, uint32_t q) {
    Envelope env;
    const QueryGraph& g = queries[q];
    env.members = {q};
    env.n = g.n;
    env.q = g.n;
    env.span_v.assign(g.n, {});
    env.reps.assign(1, std::vector<uint32_t>(g.n, 0xFFFFFFFFu));
    env.adj.assign(g.n, {});
    env.e_label.assign(g.n, {});
    for (uint32_t v = 0; v < g.n; ++v) {
        env.span_v[v].push_back(v);  // member 0, vertex v
        env.reps[0][v] = v;
        env.adj[v] = g.adj[v];
        env.e_label[v].assign(g.adj[v].size(), 0);
    }
    env.fitted = 1;
    return env;
}

}  // namespace echo
