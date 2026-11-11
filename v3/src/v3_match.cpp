// ECHO v3 masked evaluation of an envelope (M3/M4): every level of the skeleton is walked once,
// each word carrying the exact member mask, the relationship row of every skeleton edge and the
// per-member "absent" bits of the OPTIONAL branches that found no extension.
//
// Level 0 is the seed's first MATCH; each later MATCH / OPTIONAL MATCH that writes an edge is its
// own level.  A level's walk starts from the positions bound by earlier levels, extends the new
// positions, checks the edges whose two ends are already bound, applies the members' own labels,
// relationship types, directions, hop ranges and predicates, and then descends to the next level.
//
// The walk keeps one mutable word (values, relationship rows, absent bits) and restores each slot
// after its subtree, so an extension costs no allocation and no copy of the word.  K <= 64 by the
// envelope cap, so a mask is one word and the member tests are scalar bit operations.
#include "v3_match.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <set>

#include "v3_exec.h"
#include "v3_target.h"
#include "v3_executor.h"
#include "v3_mcs.h"

namespace v3 {

namespace {

struct Link {
    int32_t e = -1;     // level edge
    int32_t w = -1;     // skeleton position it binds
    bool rev = false;   // the walk goes from the edge's w to its u
};

struct Close {
    int32_t e = -1, u = -1, w = -1;
};

// One mutable word of the growth.  `mask` travels as a parameter of the recursive calls; only
// the values, relationship rows and absent bits live here, and every write is undone on return.
struct Word {
    std::vector<uint32_t> val;       // skeleton position -> vertex (kNil: unbound)
    std::vector<uint32_t> rels;      // skeleton edge -> relationship row (kNil: not walked)
    std::vector<uint64_t> absent;    // position -> members for which it is NULL
    Word(size_t np, size_t ne) : val(np, kNil), rels(ne, kNil), absent(np, 0ULL) {}
};

}  // namespace

bool MaskedMatch::run_impl(const GraphStore& g, const Schema& s, const EnvelopeSpec& E, int K,
                           int32_t /*level*/, MaskSink* sink, Counters* cnt, std::string* err,
                           TargetDistCache* tdc) {
    (void)err;
    const int M = K;
    if (M <= 0 || M > 64) {
        if (err) *err = "envelope wider than 64 members";
        return false;
    }
    const size_t NP = E.pos.size();
    const size_t NE = E.edges.size();
    if (NP == 0 || E.levels.empty()) return true;
    // One envelope has one matching semantics: the aligner refuses to merge a candidate whose
    // semantics differ from the seed's.  The masked walk therefore reads the rule once.
    const Semantics sem = (!E.q.empty() && E.q[0]) ? E.q[0]->sem : Semantics::CYPHER;
    const bool injective = (sem == Semantics::ISO || sem == Semantics::SIMPLE);
    const bool simple_path = (sem == Semantics::SIMPLE);
    const bool walk_sem = (sem == Semantics::WALK);
    // Relationship uniqueness is scoped to one MATCH clause; skeleton edges carry the seed's
    // clause, and consecutive MATCH clauses of one level keep separate scopes.
    std::vector<int32_t> se_clause(NE, -1);
    for (size_t se = 0; se < NE; ++se) se_clause[se] = E.edges[se].clause;
    const int32_t maxl = (int32_t)E.levels.size() - 1;
    // Optional per-level ledger for profiling runs.  Off by default; the hot path only tests
    // one local bool per recursion step.
    const bool prof = std::getenv("V3_LEVEL_PROFILE") != nullptr;
    const bool stage = std::getenv("V3_STAGE_PROFILE") != nullptr;
    const double stage_t0 = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    double stage_mcs = 0.0;
    auto stage_mark = [&](const char* what) {
        if (!stage) return;
        const double t = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        std::fprintf(stderr, "[stage-mark] %s %.6f\n", what, t - stage_t0);
        std::fflush(stderr);
    };
    const bool dbg_align = std::getenv("V3_DEBUG_ALIGN") != nullptr;
    const bool dbg_budget = std::getenv("V3_DEBUG_BUDGET") != nullptr;
    std::vector<double> prof_incl((size_t)maxl + 2, 0.0), prof_split((size_t)maxl + 2, 0.0);
    std::vector<uint64_t> prof_tuples((size_t)maxl + 2, 0), prof_edges((size_t)maxl + 2, 0),
        prof_init((size_t)maxl + 2, 0), prof_vl((size_t)maxl + 2, 0),
        prof_mask((size_t)maxl + 2, 0), prof_walkrows((size_t)maxl + 2, 0),
        prof_atom((size_t)maxl + 2, 0), prof_type((size_t)maxl + 2, 0),
        prof_typecmp((size_t)maxl + 2, 0), prof_close((size_t)maxl + 2, 0),
        prof_relfree((size_t)maxl + 2, 0);
    double prof_sink_sec = 0.0, prof_sink_rows = 0.0, prof_t0 = 0.0, prof_priv_sec = 0.0;
    uint64_t prof_sol = 0;
    uint64_t prof_core_rows = 0, prof_core_m[64] = {0}, prof_priv_walks[64] = {0};
    std::set<std::vector<uint32_t>> prof_core_set;
    uint64_t prof_member_sol[64] = {0};
    auto pnow = []() {
        return std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    bool aborted = false;
    uint64_t ticks = 0;
    // B_S is a work budget, not a wall-clock deadline: the merge rule estimates work units and
    // gamma is the calibrated seconds per unit.  A wall-clock gate here would make the same
    // round fall back under machine load, which changes the method under test.  Keep only a
    // generous watchdog for a runaway plan (100x B_S, at least 60 s).
    const uint64_t work0 = cnt ? cnt->work() : 0;
    double bscale = 1.0;
    if (const char* bs = std::getenv("V3_BUDGET_SCALE")) bscale = std::atof(bs);

    // `deadline == 0` means the executor did not enable the budget (v3_check correctness
    // run): B_S must not silently abort the masked path and fall back after a few rows.
    const bool budget_on = E.budget_sec > 0.0 && E.deadline > 0.0;
    uint64_t wlimit = budget_on
                           ? (uint64_t)(E.budget_sec / 1e-6 * std::max(0.0, bscale)) : 0;
    // The topological estimator is a lower bound (one row per trie branch, average fanout).
    // Keep the rho * sum_own budget, but never abort before twelve times the envelope's own
    // estimate: the DBLP beta9/12 groups measure 6-9x the model and still beat both the
    // per-query and MASC paths, while a hub-heavy WordNet group (which measured 40x) is no
    // longer planned at all by the 3 ms topology budget.  The watchdog catches a runaway plan.
    if (budget_on && E.topology && E.est_work > 0.0)
        wlimit = std::max(wlimit, (uint64_t)(E.est_work * 12.0));
    const double watchdog = budget_on ? std::max(60.0, E.budget_sec * 100.0) : 0.0;
    const double w0 = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    auto budget_hit = [&]() {
        if (wlimit && cnt && cnt->work() - work0 > wlimit) {
            aborted = true;
            if (err) *err = "budget";
            if (dbg_budget)
                std::fprintf(stderr, "budget hit work=%llu limit=%llu\n",
                             (unsigned long long)(cnt->work() - work0),
                             (unsigned long long)wlimit);
            return true;
        }
        if (watchdog > 0.0 && (++ticks & 65535ULL) == 0) {
            const double now = std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (now - w0 > watchdog) {
                aborted = true;
                if (err) *err = "budget";
                if (dbg_budget)
                    std::fprintf(stderr, "budget watchdog after %.3fs\n", now - w0);
                return true;
            }
        }
        return false;
    };

    // per member: query variable -> skeleton position, query edge -> skeleton edge
    std::vector<std::vector<int32_t>> vpos((size_t)M), epos((size_t)M);
    for (int m = 0; m < M; ++m) {
        const Query& q = *E.q[(size_t)m];
        vpos[(size_t)m].assign(q.vars.size(), -1);
        for (size_t v = 0; v < q.vars.size(); ++v)
            vpos[(size_t)m][v] = member_var_pos(E, (size_t)m, q, (int32_t)v);
        epos[(size_t)m] = E.emap[(size_t)m];
    }

    struct Binding { int m; int32_t var; };
    struct VCheck { int m; const Atom* atom; };
    std::vector<std::vector<std::vector<Binding>>> at((size_t)maxl + 1,
        std::vector<std::vector<Binding>>(NP));
    std::vector<std::vector<std::vector<VCheck>>> vchecks((size_t)maxl + 1,
        std::vector<std::vector<VCheck>>(NP));
    std::vector<std::vector<std::vector<int32_t>>> latoms((size_t)maxl + 1,
        std::vector<std::vector<int32_t>>((size_t)M));
    for (int m = 0; m < M; ++m) {
        const Query& q = *E.q[(size_t)m];
        for (size_t v = 0; v < q.vars.size(); ++v) {
            if (q.vars[v].kind != PatternVar::VERTEX) continue;
            const int32_t sp = vpos[(size_t)m][v];
            const int32_t lv = E.vlev[(size_t)m][v];
            if (sp >= 0 && sp < (int32_t)NP && lv >= 0 && lv <= maxl)
                at[(size_t)lv][(size_t)sp].push_back(Binding{m, (int32_t)v});
        }
        for (size_t a = 0; a < q.atoms.size(); ++a) {
            const Atom& x = q.atoms[a];
            if (x.col >= 0) continue;   // a WITH-column predicate, handled by the pipeline
            // A common-prefix envelope evaluates only the first MATCH; atoms of the private
            // suffix are applied by the per-candidate continuation, never by the prefix walk.
            if (E.prefix_end >= 0 && (x.clause < 0 || x.clause > E.prefix_end || x.post)) continue;
            int32_t lv = -1;
            if (x.col < 0) {
                if (x.var >= 0 && (size_t)x.var < vpos[(size_t)m].size() &&
                    vpos[(size_t)m][(size_t)x.var] >= 0)
                    lv = E.vlev[(size_t)m][(size_t)x.var];
                if (x.cmp.rhs == Comparison::VAR && x.cmp.var >= 0 &&
                    (size_t)x.cmp.var < vpos[(size_t)m].size() &&
                    vpos[(size_t)m][(size_t)x.cmp.var] >= 0)
                    lv = std::max(lv, E.vlev[(size_t)m][(size_t)x.cmp.var]);
            }
            if (lv < 0 && x.clause >= 0) {
                const std::vector<int32_t>* ml =
                    (size_t)m < E.mclause_level.size() ? &E.mclause_level[(size_t)m] : nullptr;
                if (ml && (size_t)x.clause < ml->size())
                    lv = (*ml)[(size_t)x.clause];
                else if ((size_t)x.clause < E.clause_level.size())
                    lv = E.clause_level[(size_t)x.clause];
            }
            if (lv < 0 || lv > maxl) continue;
            bool early = x.cmp.rhs != Comparison::VAR && x.or_group < 0 && x.var >= 0 &&
                         (size_t)x.var < vpos[(size_t)m].size() &&
                         vpos[(size_t)m][(size_t)x.var] >= 0 &&
                         E.vlev[(size_t)m][(size_t)x.var] == lv;
            if (early) {
                vchecks[(size_t)lv][(size_t)vpos[(size_t)m][(size_t)x.var]].push_back(VCheck{m, &x});
            } else {
                latoms[(size_t)lv][(size_t)m].push_back((int32_t)a);
            }
        }
    }

    std::vector<Env> scratch((size_t)M);
    for (int m = 0; m < M; ++m) scratch[(size_t)m].v.assign(E.q[(size_t)m]->vars.size(), kNoVal);

    const uint64_t full = (M == 64) ? ~0ULL : ((1ULL << M) - 1ULL);
    // Clause-wide vertex occupancy for a topological envelope: vertex -> members bound to it by
    // any position of the single MATCH clause.  Updating it on bind/unbind makes the injectivity
    // test one array read instead of a scan over all skeleton positions.
    const bool occ_on = injective && E.topology;
    std::vector<uint64_t> occ(occ_on ? g.num_vertices() : 0, 0ULL);
    // The membership test of one position is a function of the candidate's label and, only for
    // a member that constrains properties, of the candidate's property columns.  A MASC round
    // tests the same 2-12 positions millions of times, so the per-atom `vertex_matches` walk
    // (which resolves label strings on every call) is folded once per (level, position) into a
    // label -> member word table.  The walk then does one label read and two bit operations.
    std::vector<std::vector<std::vector<uint64_t>>> labbits(
        (size_t)maxl + 1, std::vector<std::vector<uint64_t>>(NP));
    std::vector<std::vector<uint64_t>> freebits((size_t)maxl + 1, std::vector<uint64_t>(NP, 0));
    std::vector<std::vector<uint64_t>> propbase((size_t)maxl + 1, std::vector<uint64_t>(NP, 0));
    std::vector<std::vector<std::vector<Binding>>> propb(
        (size_t)maxl + 1, std::vector<std::vector<Binding>>(NP));
    std::vector<std::vector<char>> trivial((size_t)maxl + 1, std::vector<char>(NP, 0));
    for (int32_t l = 0; l <= maxl; ++l) {
        for (size_t p = 0; p < NP; ++p) {
            trivial[(size_t)l][p] = at[(size_t)l][p].empty() && vchecks[(size_t)l][p].empty();
            // A member with no variable at this position has no constraint: its bit must
            // survive the position so it can continue to its own private levels.
            freebits[(size_t)l][p] = full;
            if (at[(size_t)l][p].empty()) continue;
            labbits[(size_t)l][p].assign(s.labels.size(), 0ULL);
            for (const auto& b : at[(size_t)l][p]) {
                const PatternVar& pv = E.q[(size_t)b.m]->vars[(size_t)b.var];
                const uint64_t bit = 1ULL << b.m;
                const bool constrained = !pv.labels.empty() || !pv.props.empty();
                if (constrained) freebits[(size_t)l][p] &= ~bit;
                bool has_label = false;
                for (const auto& lab : pv.labels) {
                    const int32_t id = s.label_of(lab);
                    if (id >= 0) {
                        labbits[(size_t)l][p][(size_t)id] |= bit;
                        has_label = true;
                    }
                }
                if (!pv.props.empty()) {
                    propb[(size_t)l][p].push_back(b);
                    if (!has_label) propbase[(size_t)l][p] |= bit;
                }
            }
        }
    }

    // the members that admit vertex x at position p of level l: one word, no allocation
    auto accepted = [&](int32_t l, int32_t p, uint32_t x) -> uint64_t {
        if (trivial[(size_t)l][(size_t)p]) return full;
        uint64_t bits = freebits[(size_t)l][(size_t)p];
        const auto& lb = labbits[(size_t)l][(size_t)p];
        if (!lb.empty()) bits |= lb[(size_t)g.label(x)];
        bits |= propbase[(size_t)l][(size_t)p];
        for (const auto& b : propb[(size_t)l][(size_t)p]) {
            const uint64_t bit = 1ULL << b.m;
            if (!(bits & bit)) continue;
            if (prof) ++prof_atom[(size_t)l];
            if (!vertex_matches(g, E.q[(size_t)b.m]->vars[(size_t)b.var], x)) bits &= ~bit;
        }
        for (const auto& vc : vchecks[(size_t)l][(size_t)p]) {
            const uint64_t bit = 1ULL << vc.m;
            if (!(bits & bit)) continue;
            Env& e = scratch[(size_t)vc.m];
            e.v[(size_t)vc.atom->var] = x;
            const int r = QueryExec::eval_atom(g, *E.q[(size_t)vc.m], e, *vc.atom);
            e.v[(size_t)vc.atom->var] = kNoVal;
            if (r != 1) bits &= ~bit;
        }
        return bits;
    };

    stage_mark("after-arrays");
    // MCS (round-local masked candidate space): built for this envelope only when its estimated
    // build work is below the expansion work it saves (the cost is in the envelope's W either
    // way).  Candidate sets and their member masks are position-indexed, the edge layer is a
    // position-to-position CSR grouped by mask.
    const double mcs_min_budget = std::getenv("V3_MCS_MIN_BUDGET")
                                      ? std::atof(std::getenv("V3_MCS_MIN_BUDGET")) : 0.02;
    double gain = E.est_sum > 0.0 ? E.est_sum - E.est_work : 1.0;
    bool worth = true;
    if (!std::getenv("V3_MCS_GATE_OFF") && E.est_build > 0.0 && gain > 0.0) {
        const double share = std::atof(std::getenv("V3_MCS_BUILD_SHARE") ?: "0.10");
        worth = E.est_build <= share * gain;
    }
    // A topological envelope has several private levels per member and positions that only a
    // subset binds; the C_S candidate space is built for one shared tree, so it is not used
    // there (the masked walk runs on the data graph, as on the no-C_S small-batch path).
    bool use_mcs = M > 1 && !E.topology && !std::getenv("V3_MCS_OFF") &&
                   E.budget_sec >= mcs_min_budget && worth;
    if (std::getenv("V3_DEBUG_MCS"))
        std::fprintf(stderr,
                     "[mcs gate] K=%d budget=%.6f min=%.6f est_build=%.0f gain=%.0f worth=%d use=%d\n",
                     M, E.budget_sec, mcs_min_budget, E.est_build, gain, (int)worth, (int)use_mcs);
    Mcs mcs(g, E, M);
    if (use_mcs) {
        const double mcs_t0 = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const uint64_t e0 = cnt ? cnt->mcs_edges : 0, v0 = cnt ? cnt->mcs_vertices : 0;
        mcs.build([&](int32_t l, int32_t p, uint32_t x, uint64_t* out) {
            *out = accepted(l, p, x);
        }, cnt);
        use_mcs = mcs.ready();
        const double bdt = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count() - mcs_t0;
        if (cnt) cnt->mcs_build_sec += bdt;
        stage_mcs = bdt;
        if (std::getenv("V3_DEBUG_MCS"))
            std::fprintf(stderr,
                         "[mcs built] ready=%d sec=%.3f edges=%llu verts=%llu bytes=%llu\n",
                         (int)use_mcs, bdt,
                         (unsigned long long)(cnt ? cnt->mcs_edges - e0 : 0),
                         (unsigned long long)(cnt ? cnt->mcs_vertices - v0 : 0),
                         (unsigned long long)mcs.bytes());
    }

    stage_mark("after-mcs");
    // per level: the walk forest, its roots and the close edges.  Positions bound by an earlier
    // level are the roots, and the tree grows from them through the level's edges: rooting at a
    // bound vertex and walking the edge backwards is what keeps an unanchored class (a whole
    // Comment table) from being scanned just because it is the written side of the edge.
    struct LevelPlan {
        std::vector<int32_t> order;
        std::vector<int32_t> parent;       // -2 unvisited, -1 root, >=0 parent
        std::vector<char> scan;            // root: scan it (1) or already bound (0)
        std::vector<Link> link;            // edge taken from the parent
        std::vector<std::vector<Link>> next;
        std::vector<Close> closes;
        std::vector<std::vector<int32_t>> close_at;  // order index -> closes completed there
        std::vector<uint64_t> pmask;        // position -> members that bind it at this level
    };
    std::vector<LevelPlan> lp((size_t)maxl + 1);
    for (int32_t l = 0; l <= maxl; ++l) {
        LevelPlan& P = lp[(size_t)l];
        P.parent.assign(NP, -2);
        P.scan.assign(NP, 0);
        P.link.assign(NP, Link());
        P.next.assign(NP, {});
        const Level& L = E.levels[(size_t)l];
        std::vector<char> used(L.edges.size(), 0);
        std::vector<char> in_level(NP, 0);
        for (const auto& le : L.edges) {
            if (le.u >= 0 && (size_t)le.u < NP) in_level[(size_t)le.u] = 1;
            if (le.w >= 0 && (size_t)le.w < NP) in_level[(size_t)le.w] = 1;
        }
        // Grow one tree from `root` over the level's unused edges.
        auto grow = [&](int32_t root, bool scan_root_pos) {
            P.parent[(size_t)root] = -1;
            P.scan[(size_t)root] = scan_root_pos ? 1 : 0;
            P.order.push_back(root);
            std::vector<int32_t> q{root};
            while (!q.empty()) {
                const int32_t cur = q.back();
                q.pop_back();
                for (size_t e = 0; e < L.edges.size(); ++e) {
                    if (used[e]) continue;
                    const LevelEdge& le = L.edges[e];
                    if (le.u < 0 || le.w < 0 || (size_t)le.u >= NP || (size_t)le.w >= NP) continue;
                    // A close edge never grows the tree: both of its classes are bound by the
                    // shared tree, and the edge is a per-member test at the leaf.
                    if (!le.tree) {
                        used[e] = 1;
                        P.closes.push_back(Close{(int32_t)e, le.u, le.w});
                        continue;
                    }
                    int32_t nxt = -1;
                    bool rev = false;
                    if (le.u == cur) { nxt = le.w; rev = false; }
                    else if (le.w == cur) { nxt = le.u; rev = true; }
                    else continue;
                    used[e] = 1;
                    if (P.parent[(size_t)nxt] == -2) {
                        P.parent[(size_t)nxt] = cur;
                        P.link[(size_t)nxt] = Link{(int32_t)e, nxt, rev};
                        P.next[(size_t)cur].push_back(Link{(int32_t)e, nxt, rev});
                        P.order.push_back(nxt);
                        q.push_back(nxt);
                    } else {
                        P.closes.push_back(Close{(int32_t)e, cur, nxt});
                    }
                }
            }
        };
        // 1. the positions an earlier level binds seed the trees
        if (l > 0) {
            std::vector<char> touch(NP, 0);
            for (const auto& le : L.edges) {
                if (le.u >= 0 && (size_t)le.u < NP) touch[(size_t)le.u] = 1;
                if (le.w >= 0 && (size_t)le.w < NP) touch[(size_t)le.w] = 1;
            }
            for (size_t p = 0; p < NP; ++p)
                if (E.pos_lev[p] < l && touch[p] && P.parent[p] == -2) grow((int32_t)p, false);
        }
        // 2. a root that scans the most selective class starts the remaining trees
        auto score = [&](int32_t p) {
            bool anchored = false;
            for (const auto& b : at[(size_t)l][(size_t)p])
                if (var_const_anchored(*E.q[(size_t)b.m], b.var)) anchored = true;
            double n = 0.0;
            for (const auto& lab : E.pos[(size_t)p].labels) {
                const int32_t id = s.label_of(lab);
                if (id >= 0) n += (double)g.vertices_of((uint32_t)id).size();
            }
            if (n <= 0.0) n = (double)g.num_vertices();
            // A constant equality (id, name, or an inline property map) has one root for the
            // class, independent of how large the label domain is.  Charging the label domain
            // here made the walk root at a small unanchored class (e.g. Country by name) and
            // expand a variable-length edge backwards into the whole Person class before the
            // id anchor filtered it.  Root at a constant class instead.
            return anchored ? 1.0 : n;
        };
        std::vector<char> root_done(NP, 0);
        while (true) {
            int32_t root = -1;
            for (size_t p = 0; p < NP; ++p)
                if (in_level[p] && !root_done[p] && (E.pos_lev[p] == l || l > 0) &&
                    P.parent[p] == -2 && (root < 0 || score((int32_t)p) < score(root)))
                    root = (int32_t)p;
            if (root < 0) break;
            root_done[(size_t)root] = 1;
            grow(root, E.pos_lev[(size_t)root] == l);
        }
        // isolated positions are scans of their own
        for (size_t p = 0; p < NP; ++p)
            if (E.pos_lev[p] == l && P.parent[p] == -2) {
                P.parent[p] = -1;
                P.scan[p] = 1;
                P.order.push_back((int32_t)p);
            }
        // A close edge is complete once the later of its two positions is bound.  Index them by
        // that order position so the walk can test the edge there instead of at the level leaf.
        P.close_at.assign(P.order.size(), {});
        for (size_t ci = 0; ci < P.closes.size(); ++ci) {
            const Close& c = P.closes[ci];
            int32_t iu = -1, iw = -1;
            for (size_t j = 0; j < P.order.size(); ++j) {
                if (P.order[j] == c.u) iu = (int32_t)j;
                if (P.order[j] == c.w) iw = (int32_t)j;
            }
            const int32_t at = std::max(iu, iw);
            if (at >= 0) P.close_at[(size_t)at].push_back((int32_t)ci);
        }
    }

    if (std::getenv("V3_DEBUG_LEVELPLAN")) {
        for (int32_t l = 0; l <= maxl; ++l) {
            const LevelPlan& P = lp[(size_t)l];
            const Level& L = E.levels[(size_t)l];
            std::fprintf(stderr, "[lp] l=%d order:", l);
            for (int32_t p : P.order) std::fprintf(stderr, " %d(scan=%d,parent=%d)", p,
                         (int)P.scan[(size_t)p], P.parent[(size_t)p]);
            std::fprintf(stderr, "\n");
            for (size_t e = 0; e < L.edges.size(); ++e) {
                const LevelEdge& le = L.edges[e];
                std::fprintf(stderr, "[lp] l=%d e=%zu %d->%d tree=%d lo=%d hi=%d\n", l, e,
                             le.u, le.w, (int)le.tree, le.lo, le.hi);
            }
            for (const auto& c : P.closes)
                std::fprintf(stderr, "[lp] l=%d close e=%d %d->%d\n", l, c.e, c.u, c.w);
        }
    }
    stage_mark("after-levelplan");
    // Vertex injectivity inside one level: a member may not bind two positions of the same
    // MATCH clause to one data vertex.  pmask[p] names the members that bind position p here;
    // when a new position takes x, those members are removed from the candidate word for every
    // other bound position that already holds x.  Cypher and WALK do not use this.
    if (injective) {
        for (int32_t l = 0; l <= maxl; ++l) {
            LevelPlan& P = lp[(size_t)l];
            const Level& L = E.levels[(size_t)l];
            P.pmask.assign(NP, 0ULL);
            for (int m = 0; m < M; ++m)
                for (size_t p = 0; p < NP; ++p)
                    if (L.present[(size_t)m][p]) P.pmask[p] |= 1ULL << m;
        }
    }

    // The participating members and the new positions are properties of the level, not of the
    // word: compute them once.
    std::vector<uint64_t> partl((size_t)maxl + 1, 0ULL), optl((size_t)maxl + 1, 0ULL);
    std::vector<std::vector<char>> newposl((size_t)maxl + 1, std::vector<char>(NP, 0));
    for (int32_t l = 0; l <= maxl; ++l) {
        const Level& L = E.levels[(size_t)l];
        for (size_t p = 0; p < NP; ++p)
            if (E.pos_lev[p] == l) newposl[(size_t)l][p] = 1;
        for (int m = 0; m < M; ++m) {
            bool has = false;
            for (size_t e = 0; e < L.edges.size(); ++e)
                if (!L.types[(size_t)m][(size_t)L.level_skel[e]].empty()) has = true;
            for (size_t p = 0; p < NP && !has; ++p)
                if (newposl[(size_t)l][p])
                    for (const auto& b : at[(size_t)l][p])
                        if (b.m == m) has = true;
            if (has) partl[(size_t)l] |= 1ULL << m;
            if (has && L.opt[(size_t)m]) optl[(size_t)l] |= 1ULL << m;
        }
    }

    // A topological envelope enumerates the common core once, then each member's private
    // suffix alone from every core row.  The old single DFS multiplied the members' private
    // branches: a member whose suffix ends early was emitted once per branch of the others
    // (DBLP beta9 batch1 member 1: 145 rows for one matching).  `mlevels[m]` is the ascending
    // list of private levels that member m actually walks.
    std::vector<std::vector<int32_t>> mlevels((size_t)M);
    for (int32_t l = 1; l <= maxl; ++l)
        for (int m = 0; m < M; ++m)
            if (partl[(size_t)l] & (1ULL << m)) mlevels[(size_t)m].push_back(l);
    std::vector<std::vector<char>> is_vl((size_t)maxl + 1);
    for (int32_t l = 0; l <= maxl; ++l) {
        const Level& L = E.levels[(size_t)l];
        is_vl[(size_t)l].assign(L.edges.size(), 0);
        for (size_t e = 0; e < L.edges.size(); ++e) {
            const int32_t se = L.level_skel[e];
            for (int m = 0; m < M; ++m)
                if (!L.types[(size_t)m][(size_t)se].empty() &&
                    (L.hop_lo[(size_t)m][(size_t)se] != 1 || L.hop_hi[(size_t)m][(size_t)se] != 1))
                    is_vl[(size_t)l][e] = 1;
        }
    }

    // Fixed-hop level edge: the members accepting each relationship type and each traversed
    // direction are properties of the edge, not of the candidate.  Precompute both words once;
    // the old loop compared label strings for every member and every adjacency entry.
    struct FixedAux {
        std::vector<int32_t> tids;
        std::vector<uint64_t> tbits;                // type id -> members accepting it
        uint64_t dir_bits[2][2] = {{0, 0}, {0, 0}};  // [from-is-u][row direction]
    };
    std::vector<std::vector<FixedAux>> faux((size_t)maxl + 1);
    for (int32_t l = 0; l <= maxl; ++l) {
        const Level& L = E.levels[(size_t)l];
        faux[(size_t)l].resize(L.edges.size());
        for (size_t e = 0; e < L.edges.size(); ++e) {
            FixedAux& A = faux[(size_t)l][e];
            const int32_t se = L.level_skel[e];
            if (is_vl[(size_t)l][e]) continue;
            A.tbits.assign(s.types.size(), 0ULL);
            for (const auto& ty : L.edges[e].types) {
                const int32_t t = s.type_of(ty);
                if (t < 0) continue;
                if (std::find(A.tids.begin(), A.tids.end(), t) == A.tids.end()) A.tids.push_back(t);
                for (int m = 0; m < M; ++m) {
                    const uint64_t bit = 1ULL << m;
                    if (L.types[(size_t)m][(size_t)se].empty()) {
                        A.tbits[(size_t)t] |= bit;
                    } else {
                        for (const auto& mt : L.types[(size_t)m][(size_t)se])
                            if (mt == ty) { A.tbits[(size_t)t] |= bit; break; }
                    }
                }
            }
            for (int fu = 0; fu < 2; ++fu)
                for (int rd = 0; rd < 2; ++rd) {
                    uint64_t bits = 0;
                    for (int m = 0; m < M; ++m) {
                        const bool want = (L.dir[(size_t)m][(size_t)se] == (fu ? 1 : 0));
                        if (L.any_dir[(size_t)m][(size_t)se] || (want ? 1u : 0u) == (uint32_t)rd)
                            bits |= 1ULL << m;
                    }
                    A.dir_bits[fu][rd] = bits;
                }
        }
    }

    // Per-level-edge bit tables for the depth-masked variable-length walk.  The recursion used
    // to rebuild these masks member by member at every hop; on the heavy rounds that is billions
    // of redundant tests.  out[d] = members whose [lo,hi] contains d, live[d] = members that may
    // continue past d, type_bits[t] = members accepting type t, dir_walk[from][row] = members
    // whose own direction accepts a row traversed in that direction.
    struct VlAux {
        std::vector<uint64_t> out, upper, live, type_bits;
        uint64_t anyd = 0;
        uint64_t dir_walk[2][2] = {{0, 0}, {0, 0}};
    };
    constexpr int kVlDepth = 256;
    std::vector<std::vector<VlAux>> vlaux((size_t)maxl + 1);
    for (int32_t l = 0; l <= maxl; ++l) {
        const Level& L = E.levels[(size_t)l];
        vlaux[(size_t)l].resize(L.edges.size());
        for (size_t e = 0; e < L.edges.size(); ++e) {
            if (!is_vl[(size_t)l][e]) continue;
            const int32_t se = L.level_skel[e];
            VlAux& A = vlaux[(size_t)l][e];
            A.out.assign(kVlDepth, 0ULL);
            A.upper.assign(kVlDepth, 0ULL);
            A.live.assign(kVlDepth, 0ULL);
            A.type_bits.assign(s.types.size(), 0ULL);
            for (int m = 0; m < M; ++m) {
                const uint64_t bit = 1ULL << m;
                const int32_t lo = L.hop_lo[(size_t)m][(size_t)se];
                const int32_t hi = L.hop_hi[(size_t)m][(size_t)se];
                if (L.types[(size_t)m][(size_t)se].empty()) continue;
                for (int d = 0; d < kVlDepth; ++d) {
                    if (lo <= d && (hi < 0 || d <= hi)) A.out[(size_t)d] |= bit;
                    if (hi < 0 || d <= hi) A.upper[(size_t)d] |= bit;
                    if (hi < 0 || d < hi) A.live[(size_t)d] |= bit;
                }
                if (L.any_dir[(size_t)m][(size_t)se]) {
                    A.anyd |= bit;
                    for (int fu = 0; fu < 2; ++fu)
                        for (int rd = 0; rd < 2; ++rd) A.dir_walk[fu][rd] |= bit;
                } else {
                    const int want = L.dir[(size_t)m][(size_t)se] ? 1 : 0;
                    for (int fu = 0; fu < 2; ++fu)
                        A.dir_walk[fu][(want == fu) ? 1 : 0] |= bit;
                }
                for (const auto& ty : L.types[(size_t)m][(size_t)se]) {
                    const int32_t t = s.type_of(ty);
                    if (t >= 0) A.type_bits[(size_t)t] |= bit;
                }
            }
        }
    }

    // Depth-layer target groups of every variable-length level edge (paper §V-B): members that
    // share one reverse-BFS distance to their own edge target.  The BFS runs once per (target,
    // label set, types, direction, upper bound) for the whole round, never per input row.
    struct TargetGroup { uint64_t members = 0; const std::vector<int32_t>* dist = nullptr; };
    std::vector<std::vector<std::vector<TargetGroup>>> tgroups((size_t)maxl + 1);
    if (tdc) {
        for (int32_t l = 0; l <= maxl; ++l) {
            const Level& L = E.levels[(size_t)l];
            tgroups[(size_t)l].resize(L.edges.size());
            for (size_t e = 0; e < L.edges.size(); ++e) {
                if (!is_vl[(size_t)l][e]) continue;
                const int32_t se = L.level_skel[e];
                int32_t start_pos = -1, target_pos = -1;
                for (int32_t cur = 0; cur < NP && start_pos < 0; ++cur)
                    for (const auto& lk : lp[(size_t)l].next[(size_t)cur])
                        if (lk.e == (int32_t)e) { start_pos = cur; target_pos = lk.w; break; }
                if (start_pos < 0)
                    for (const auto& cl : lp[(size_t)l].closes)
                        if (cl.e == (int32_t)e) { start_pos = cl.u; target_pos = cl.w; break; }
                if (start_pos < 0 || target_pos < 0) continue;
                auto& out = tgroups[(size_t)l][e];
                for (int m = 0; m < M; ++m) {
                    if (L.types[(size_t)m][(size_t)se].empty()) continue;
                    // A fixed-hop member never continues past its one hop, so its target
                    // distance is never consulted: building the whole-graph BFS for it was
                    // pure setup cost (IS6/IS7 rounds: 0.07-0.12 s per fixed-hop member).
                    if (L.hop_lo[(size_t)m][(size_t)se] == 1 &&
                        L.hop_hi[(size_t)m][(size_t)se] == 1)
                        continue;
                    int32_t qe = -1;
                    for (size_t k = 0; k < epos[(size_t)m].size(); ++k)
                        if (epos[(size_t)m][k] == se) { qe = (int32_t)k; break; }
                    if (qe < 0) continue;
                    int32_t var_from = -1, var_to = -1;
                    for (size_t v = 0; v < vpos[(size_t)m].size(); ++v) {
                        if (vpos[(size_t)m][v] == start_pos) var_from = (int32_t)v;
                        if (vpos[(size_t)m][v] == target_pos) var_to = (int32_t)v;
                    }
                    if (var_from < 0 || var_to < 0) continue;
                    const int32_t hi = L.hop_hi[(size_t)m][(size_t)se];
                    if (hi < 0) continue;
                    const Query& q = *E.q[(size_t)m];
                    const PatternEdge& pe = q.edges[(size_t)qe];
                    const bool forward = (pe.from == var_from);
                    const double tdc_t0 = std::chrono::duration<double>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    const std::vector<int32_t>* d =
                        tdc->get(g, q, pe, var_from, var_to, hi, forward);
                    const double tdc_dt = std::chrono::duration<double>(
                        std::chrono::steady_clock::now().time_since_epoch()).count() - tdc_t0;
                    if (std::getenv("V3_DEBUG_TDC"))
                        std::fprintf(stderr, "[tdc] l=%d e=%zu m=%d hi=%d qe=%d fwd=%d d=%d sec=%.6f\n",
                                     l, e, m, hi, qe, (int)forward, d ? 1 : 0, tdc_dt);
                    if (!d) continue;
                    const uint64_t bit = 1ULL << m;
                    bool merged = false;
                    for (auto& tgr : out)
                        if (tgr.dist == d) { tgr.members |= bit; merged = true; break; }
                    if (!merged) out.push_back(TargetGroup{bit, d});
                }
            }
        }
    }

    stage_mark("after-tgroups");
    auto env_of = [&](const Word& w, int m) {
        Env e;
        const Query& q = *E.q[(size_t)m];
        e.v.assign(q.vars.size(), kNoVal);
        for (size_t v = 0; v < q.vars.size(); ++v) {
            if (q.vars[v].kind != PatternVar::VERTEX) continue;
            const int32_t sp = vpos[(size_t)m][v];
            if (sp >= 0 && sp < (int32_t)NP && w.val[(size_t)sp] != kNil &&
                !((w.absent[(size_t)sp] >> m) & 1ULL))
                e.v[v] = w.val[(size_t)sp];
        }
        for (size_t pe = 0; pe < q.edges.size(); ++pe) {
            if (q.edges[pe].var_name.empty()) continue;
            const int32_t qv = q.var_index(q.edges[pe].var_name);
            const int32_t se = epos[(size_t)m][pe];
            if (qv >= 0 && se >= 0 && (size_t)se < NE && w.rels[(size_t)se] != kNil)
                e.v[(size_t)qv] = w.rels[(size_t)se];
        }
        return e;
    };
    auto member_atoms_ok = [&](const Word& w, int m, int32_t l) {
        Env e = env_of(w, m);
        const Query& q = *E.q[(size_t)m];
        const auto& atoms = latoms[(size_t)l][(size_t)m];
        size_t k = 0;
        while (k < atoms.size()) {
            const Atom& a = q.atoms[(size_t)atoms[k]];
            if (!QueryExec::atom_evaluable(q, e, a)) { ++k; continue; }
            if (a.or_group >= 0) {
                bool any = false;
                size_t j = k;
                while (j < atoms.size() && q.atoms[(size_t)atoms[j]].or_group == a.or_group) {
                    const Atom& b = q.atoms[(size_t)atoms[j]];
                    if (QueryExec::atom_evaluable(q, e, b) && QueryExec::eval_atom(g, q, e, b) == 1)
                        any = true;
                    ++j;
                }
                if (!any) return false;
                k = j;
                continue;
            }
            if (QueryExec::eval_atom(g, q, e, a) != 1) return false;
            ++k;
        }
        return true;
    };

    stage_mark("before-word");
    Word st(NP, NE);
    stage_mark("after-word");
    std::vector<uint64_t> maskvec(1, 0ULL);
    st.val.assign(NP, kNil);

    // Relationship identity is the logical relationship (`edge_row`), not the directed row:
    // the two rows of one undirected relationship are the same edge.  The stack is scoped to
    // one MATCH clause, so consecutive MATCH clauses of one level may reuse a relationship.
    struct UsedRel { int32_t clause; uint32_t rid; };
    std::vector<UsedRel> used;
    // A topological envelope is one MATCH clause; its used relationship rows fit in a bitset,
    // so the uniqueness test is one bit read instead of a scan of the stack (WordNet beta12
    // checks about half a million adjacency entries in one cell).
    std::vector<uint64_t> used_bits;
    // A full relationship bitset costs one 4.3 MB zero page per envelope on SF1 (34.5M
    // relationships); a narrow topology envelope (IC8: five positions) touches a few dozen
    // rows, so the stack scan is cheaper than clearing the bitset.  Wide topology cores keep
    // the bitset (the WordNet 12-position case that motivated it).
    const bool rel_bits = E.topology && NP >= 8;
    if (rel_bits) used_bits.assign((size_t)((g.num_rels() + 63) / 64), 0ULL);
    auto rel_free = [&](int32_t se, uint32_t rel) -> bool {
        if (walk_sem) return true;
        const int32_t cl = (size_t)se < se_clause.size() ? se_clause[(size_t)se] : -1;
        const uint32_t rid = g.edge_row(rel);
        if (rel_bits)
            return (used_bits[(size_t)(rid >> 6)] & (1ULL << (rid & 63))) == 0;
        for (const auto& u : used)
            if (u.clause == cl && u.rid == rid) return false;
        return true;
    };
    auto rel_push = [&](int32_t se, uint32_t rel) {
        if (walk_sem) return;
        const uint32_t rid = g.edge_row(rel);
        used.push_back({se_clause[(size_t)se], rid});
        if (rel_bits) used_bits[(size_t)(rid >> 6)] |= 1ULL << (rid & 63);
    };
    auto rel_pop_to = [&](size_t before) {
        while (used.size() > before) {
            const UsedRel u = used.back();
            used.pop_back();
            if (rel_bits) used_bits[(size_t)(u.rid >> 6)] &= ~(1ULL << (u.rid & 63));
        }
    };
    auto injective_filter = [&](int32_t l, int32_t p, uint32_t x, uint64_t bits) -> uint64_t {
        if (!injective) return bits;
        if (E.topology) {
            bits &= ~occ[(size_t)x];
            return bits;
        }
        const LevelPlan& P = lp[(size_t)l];
        for (int32_t q : P.order) {
            if (q == p) continue;
            if (st.val[(size_t)q] == x) bits &= ~P.pmask[(size_t)q];
        }
        return bits;
    };

    if (std::getenv("V3_DEBUG_SETUP"))
        std::fprintf(stderr, "[setup] topology=%d num_rels=%llu np=%zu ne=%zu maxl=%d\n",
                     (int)E.topology, (unsigned long long)g.num_rels(), NP, NE, maxl);
    stage_mark("before-fn");
    std::function<void(int32_t, uint64_t)> process;
    std::function<void(int32_t, size_t, uint64_t, uint64_t, uint64_t*)> walk;
    std::function<void(int32_t, int32_t, uint64_t)> priv_process;
    std::function<void(int32_t, uint64_t)> advance;
    bool priv_active = false;
    int32_t priv_m = -1, priv_i = -1;

    // depth-masked variable-length expansion of one level edge from `cur`
    std::function<void(int32_t, size_t, const Link&, uint32_t, int32_t, int32_t, bool, uint64_t,
                       std::vector<uint32_t>&, uint64_t, uint64_t*)> vl_step;
    vl_step = [&](int32_t l, size_t k, const Link& lk, uint32_t cur, int32_t cur_pos,
                  int32_t depth, bool from_is_u, uint64_t mask,
                  std::vector<uint32_t>& vpath, uint64_t opt, uint64_t* ext) {
        if (aborted) return;
        const Level& L = E.levels[(size_t)l];
        const int32_t se = L.level_skel[(size_t)lk.e];
        const int32_t wpos = lk.w;
        const VlAux& A = vlaux[(size_t)l][lk.e];
        uint64_t dbits = 0;
        if (depth < kVlDepth) {
            dbits = A.out[(size_t)depth];
        } else {
            for (int m = 0; m < M; ++m) {
                const int32_t lo = L.hop_lo[(size_t)m][(size_t)se];
                const int32_t hi = L.hop_hi[(size_t)m][(size_t)se];
                if (depth >= lo && (hi < 0 || depth <= hi)) dbits |= 1ULL << m;
            }
        }
        // An undirected edge is expanded in both directions at every hop.  The MCS CSR is
        // keyed by the skeleton's u -> w direction, so for such an edge we read the data
        // adjacency and apply the target's candidate mask and depth lower bound per hop; the
        // per-member direction test below is the Cypher direction test of the edge.
        const bool anyd = A.anyd != 0;
        uint64_t ok = ~0ULL;
        // The edge layer materialises one-hop targets.  At depth 1 its masks are complete; at
        // depth >= 2 only a fully materialised target class can be used, otherwise a two-hop
        // vertex outside the one-hop subset would be pruned (the old bug).
        const bool target_complete = mcs.full(wpos) || depth <= 1;
        if (!(anyd && depth == 0))
            ok = (use_mcs && cur_pos >= 0 && mcs.built(wpos) && target_complete)
                     ? mcs.mask_at(wpos, cur_pos) : accepted(l, wpos, cur);
        uint64_t m2 = mask & dbits & ok;
        m2 = injective_filter(l, wpos, cur, m2);
        if (m2) {
            const uint32_t saved = st.val[(size_t)wpos];
            st.val[(size_t)wpos] = cur;
            const uint64_t saved_occ = occ_on ? occ[(size_t)cur] : 0ULL;
            if (occ_on) occ[(size_t)cur] |= m2;
            if (cnt) ++cnt->vl_rows;
            walk(l, k + 1, m2, opt, ext);
            if (occ_on) occ[(size_t)cur] = saved_occ;
            st.val[(size_t)wpos] = saved;
            if (aborted) return;
        }
        const int32_t maxd = L.edges[(size_t)lk.e].hi < 0 ? (int32_t)g.num_vertices()
                                                         : L.edges[(size_t)lk.e].hi;
        if (depth >= maxd) return;
        uint64_t live = 0;
        if (depth < kVlDepth) {
            live = A.live[(size_t)depth];
        } else {
            for (int m = 0; m < M; ++m) {
                const int32_t hi = L.hop_hi[(size_t)m][(size_t)se];
                if (hi >= 0 && depth >= hi) continue;
                live |= 1ULL << m;
            }
        }
        if (!live) return;
        // MCS edge layer: the CSR is the u -> w direction, so it serves a forward walk from a
        // candidate of the u class; a reversed walk or an intermediate vertex outside the class
        // falls back to the data adjacency below.
        // The MCS edge layer stores candidates of the endpoint position only.  An intermediate
        // vertex of a variable-length path need not be an endpoint candidate (its label may
        // differ), so the continuation must use the data adjacency.  The endpoint mask is
        // still applied when a depth is emitted.
        const int32_t upos = -1;
        if (upos >= 0) {
            const McsEdge& me = mcs.edge(se);
            const uint64_t cont = mask & live;
            for (uint32_t sg = me.off[(size_t)upos]; sg < me.off[(size_t)upos + 1]; ++sg) {
                const uint64_t cm = cont & me.segs[sg].mask;
                if (!cm) continue;
                for (uint32_t j = me.segs[sg].begin; j < me.segs[sg].end; ++j) {
                    const uint32_t rel = me.rel[j];
                    if (!rel_free(se, rel)) continue;
                    const uint32_t cp = me.child[j];
                    const uint32_t child = mcs.verts((int32_t)E.edges[(size_t)se].w)[cp];
                    if (simple_path &&
                        std::find(vpath.begin(), vpath.end(), child) != vpath.end()) continue;
                    uint64_t c2 = cm;
                    if (!me.dist.empty() && mcs.full(wpos)) {
                        const int8_t d = me.dist[(size_t)cp];
                        if (d < 0) continue;
                        const int32_t dd = depth + 1 + (int32_t)d;
                        const uint64_t can = dd < kVlDepth ? A.upper[(size_t)dd] : ~0ULL;
                        c2 &= can;
                        if (!c2) continue;
                    }
                    c2 = injective_filter(l, wpos, child, c2);
                    if (!c2) continue;
                    const size_t before = used.size();
                    rel_push(se, rel);
                    vpath.push_back(child);
                    if (cnt) ++cnt->vl_steps;
                    if (prof) ++prof_vl[(size_t)l];
                    vl_step(l, k, lk, child, (int32_t)cp, depth + 1, from_is_u, c2, vpath, opt,
                            ext);
                    vpath.pop_back();
                    rel_pop_to(before);
                    if (aborted) return;
                }
            }
            return;
        }
        for (const auto& ty : L.edges[(size_t)lk.e].types) {
            const int32_t t = s.type_of(ty);
            if (t < 0) continue;
            const uint64_t tbits = A.type_bits[(size_t)t] & live;
            const uint64_t cont = mask & tbits;
            if (!cont) continue;
            const AdjView a = g.typed((uint32_t)t);
            const auto& gr = g.typed_rels((uint32_t)t);
            for (uint32_t j = (*a.off)[cur]; j < (*a.off)[cur + 1]; ++j) {
                const uint32_t rel = gr[j];
                if (!rel_free(se, rel)) continue;
                const uint32_t child = (*a.nbr)[j];
                if (simple_path &&
                    std::find(vpath.begin(), vpath.end(), child) != vpath.end()) continue;
                const uint32_t row_dir = (uint32_t)(*a.dir)[j];
                const uint64_t dbits2 = A.dir_walk[from_is_u ? 1 : 0][row_dir & 1u];
                // The child is an intermediate vertex on this hop.  Do not apply the endpoint
                // position's label/predicate mask here: `vl_step` applies it when the depth is
                // emitted.  Applying it at every hop drops paths through vertices whose label
                // differs from the endpoint's.
                uint64_t c2 = cont & dbits2;
                if (!c2) continue;
                // depth-layer prune: members whose target is farther than their upper bound
                // minus the hops already taken can no longer complete this path
                const auto& tgs = tgroups[(size_t)l][lk.e];
                for (const auto& tgr : tgs) {
                    if (!(c2 & tgr.members)) continue;
                    const int32_t dd = (*tgr.dist)[child];
                    uint64_t allow = 0;
                    if (dd >= 0) {
                        const uint64_t total = (uint64_t)depth + 1 + (uint64_t)dd;
                        allow = total < (uint64_t)kVlDepth ? A.upper[(size_t)total] : ~0ULL;
                    }
                    c2 &= (allow | ~tgr.members);
                }
                if (!c2) continue;
                c2 = injective_filter(l, wpos, child, c2);
                if (!c2) continue;
                const size_t before = used.size();
                rel_push(se, rel);
                vpath.push_back(child);
                if (cnt) ++cnt->vl_steps;
                if (prof) ++prof_vl[(size_t)l];
                vl_step(l, k, lk, child, -1, depth + 1, from_is_u, c2, vpath, opt,
                        ext);
                vpath.pop_back();
                rel_pop_to(before);
                if (aborted) return;
            }
        }
    };

    // A close edge can prune as soon as both endpoints are bound.  Waiting for the level leaf let
    // a 12-position core expand 17.8k partial rows to find 22 complete ones (WordNet beta12); the
    // early test only narrows the member mask, while the leaf test below still binds the
    // relationship row and the used set, so the two together keep the existing semantics.
    auto early_closes = [&](int32_t l, int32_t k, uint64_t m) -> uint64_t {
        const LevelPlan& P = lp[(size_t)l];
        if (k < 0 || (size_t)k >= P.close_at.size() || P.close_at[(size_t)k].empty()) return m;
        const Level& L = E.levels[(size_t)l];
        const AdjView ad = g.all();
        const auto& gr = g.all_rels();
        for (int32_t ci : P.close_at[(size_t)k]) {
            const Close& c = P.closes[(size_t)ci];
            const uint32_t a = st.val[(size_t)c.u], b = st.val[(size_t)c.w];
            if (a == kNil || b == kNil) continue;
            const int32_t se = L.level_skel[(size_t)c.e];
            const bool from_is_u = c.u == L.edges[(size_t)c.e].u;
            uint64_t ok = 0;
            for (int mm = 0; mm < M; ++mm)
                if (L.types[(size_t)mm][(size_t)se].empty()) ok |= 1ULL << mm;
            const uint32_t da = (*ad.off)[a + 1] - (*ad.off)[a];
            const uint32_t db = (*ad.off)[b + 1] - (*ad.off)[b];
            const bool scan_b = db < da;
            const uint32_t from_v = scan_b ? b : a;
            const uint32_t to_v = scan_b ? a : b;
            auto take = [&](uint32_t rel) {
                if (!rel_free(se, rel)) return;
                const uint32_t t = g.edge_type(rel);
                uint32_t row_dir = (uint32_t)g.edge_dir(rel);
                if (scan_b) row_dir ^= 1u;
                for (int mm = 0; mm < M; ++mm) {
                    const bool want = L.dir[(size_t)mm][(size_t)se] == (from_is_u ? 1 : 0);
                    if (!L.any_dir[(size_t)mm][(size_t)se] && row_dir != (want ? 1u : 0u)) continue;
                    for (const auto& ty : L.types[(size_t)mm][(size_t)se]) {
                        const int32_t id = s.type_of(ty);
                        if (id >= 0 && (uint32_t)id == t) { ok |= 1ULL << mm; break; }
                    }
                }
            };
            const std::vector<std::pair<uint32_t, uint32_t>>* srt =
                std::min(da, db) > 64 ? g.sorted_adj(from_v) : nullptr;
            if (srt) {
                auto lb = std::lower_bound(srt->begin(), srt->end(), to_v,
                                           [](const std::pair<uint32_t, uint32_t>& x, uint32_t v) {
                                               return x.first < v;
                                           });
                auto ub = std::upper_bound(lb, srt->end(), to_v,
                                           [](uint32_t v, const std::pair<uint32_t, uint32_t>& x) {
                                               return v < x.first;
                                           });
                for (auto it = lb; it != ub; ++it) take(it->second);
            } else {
                for (uint32_t j = (*ad.off)[from_v]; j < (*ad.off)[from_v + 1]; ++j)
                    if ((*ad.nbr)[j] == to_v) take(gr[j]);
            }
            m &= ok;
            if (!m) return 0;
        }
        return m;
    };

    // One level: `mask` holds the members alive at the positions bound so far.  `opt`/`ext` are
    // set when the level has OPTIONAL members: `ext` accumulates the masks of the extensions
    // that survived, so the members without one are padded afterwards.
    walk = [&](int32_t l, size_t k, uint64_t mask, uint64_t opt, uint64_t* ext) {
        if (aborted) return;
        if (budget_hit()) return;
        const LevelPlan& P = lp[(size_t)l];
        if (k > 0 && k < P.order.size()) {
            mask = early_closes(l, (int32_t)k - 1, mask);
            if (!mask) return;
        }
        if (k == P.order.size()) {
            const Level& L = E.levels[(size_t)l];
            uint64_t m = mask;
            // close edges: both ends bound, a relationship of the member's type joins them.
            // The relationships already used by the clause's tree edges are excluded.
            std::vector<std::pair<int32_t, uint32_t>> saved_rel;
            const size_t used_before = used.size();
            for (const auto& c : P.closes) {
                const uint32_t a = st.val[(size_t)c.u], b = st.val[(size_t)c.w];
                if (a == kNil || b == kNil) continue;
                const int32_t se = L.level_skel[(size_t)c.e];
                // The close is walked from c.u to c.w, which may be the reverse of the
                // skeleton's u -> w: the direction test must flip with the traversal.
                const bool from_is_u = c.u == L.edges[(size_t)c.e].u;
                uint64_t ok = 0;
                for (int mm = 0; mm < M; ++mm)
                    if (L.types[(size_t)mm][(size_t)se].empty()) ok |= 1ULL << mm;
                const AdjView ad = g.all();
                const auto& gr = g.all_rels();
                // Both endpoints are bound, so the close is an edge lookup, not a fan-out: scan
                // the endpoint with the smaller degree, or binary-search its sorted adjacency.
                // Scanning the hub side cost 2.3M adjacency entries for 22 core rows on WordNet
                // beta12 while the close itself was one edge per row.
                const uint32_t da = (*ad.off)[a + 1] - (*ad.off)[a];
                const uint32_t db = (*ad.off)[b + 1] - (*ad.off)[b];
                const bool scan_b = db < da;
                const uint32_t from_v = scan_b ? b : a;
                const uint32_t to_v = scan_b ? a : b;
                auto consider = [&](uint32_t rel) {
                    if (!rel_free(se, rel)) return;
                    const uint32_t t = g.edge_type(rel);
                    uint32_t row_dir = (uint32_t)g.edge_dir(rel);
                    // A row found from c.w is the reverse copy: its direction bit is complemented
                    // for the c.u -> c.w traversal.  The logical relationship (`edge_row`) is the
                    // same row, which is what identity and the returned value use.
                    if (scan_b) row_dir ^= 1u;
                    uint64_t hit = 0;
                    for (int mm = 0; mm < M; ++mm) {
                        const bool want = L.dir[(size_t)mm][(size_t)se] == (from_is_u ? 1 : 0);
                        if (!L.any_dir[(size_t)mm][(size_t)se] && row_dir != (want ? 1u : 0u)) continue;
                        for (const auto& ty : L.types[(size_t)mm][(size_t)se]) {
                            const int32_t id = s.type_of(ty);
                            if (id >= 0 && (uint32_t)id == t) hit |= 1ULL << mm;
                        }
                    }
                    if (!hit) return;
                    ok |= hit;
                    if (st.rels[(size_t)se] == kNil) {
                        saved_rel.emplace_back(se, st.rels[(size_t)se]);
                        st.rels[(size_t)se] = rel;
                        rel_push((int32_t)se, rel);
                    }
                };
                const std::vector<std::pair<uint32_t, uint32_t>>* srt =
                    std::min(da, db) > 64 ? g.sorted_adj(from_v) : nullptr;
                if (srt) {
                    if (prof) ++prof_close[(size_t)l];
                    auto lo = std::lower_bound(srt->begin(), srt->end(), to_v,
                                               [](const std::pair<uint32_t, uint32_t>& x, uint32_t v) {
                                                   return x.first < v;
                                               });
                    auto hi = std::upper_bound(lo, srt->end(), to_v,
                                               [](uint32_t v, const std::pair<uint32_t, uint32_t>& x) {
                                                   return v < x.first;
                                               });
                    for (auto it = lo; it != hi; ++it) consider(it->second);
                } else {
                    for (uint32_t j = (*ad.off)[from_v]; j < (*ad.off)[from_v + 1]; ++j) {
                        if (prof) ++prof_close[(size_t)l];
                        if ((*ad.nbr)[j] != to_v) continue;
                        consider(gr[j]);
                    }
                }
                m &= ok;
                if (!m) break;
            }
            if (m) {
                for (int mm = 0; mm < M; ++mm) {
                    if (!((m >> mm) & 1ULL)) continue;
                    if (!member_atoms_ok(st, mm, l)) m &= ~(1ULL << mm);
                }
            }
            if (m) {
                if (ext) *ext |= m & opt;
                advance(l, m);
            }
            for (const auto& sr : saved_rel) st.rels[(size_t)sr.first] = sr.second;
            rel_pop_to(used_before);
            return;
        }
        const int32_t p = P.order[k];
        if (P.parent[(size_t)p] == -1) {
            if (!P.scan[(size_t)p]) {
                if (st.val[(size_t)p] == kNil) return;
                walk(l, k + 1, mask, opt, ext);
                return;
            }
            const auto& labels = E.pos[(size_t)p].labels;
            auto take = [&](uint32_t x, uint64_t bits) {
                // One charged row per candidate the root scan examines (the per-candidate
                // Walker counts its scans the same way).
                if (cnt) ++cnt->init_tuples;
                if (prof) ++prof_init[(size_t)l];
                uint64_t m2 = mask & bits;
                m2 = injective_filter(l, p, x, m2);
                if (!m2) return;
                const uint32_t saved = st.val[(size_t)p];
                st.val[(size_t)p] = x;
                const uint64_t saved_occ = occ_on ? occ[(size_t)x] : 0ULL;
                if (occ_on) occ[(size_t)x] |= m2;
                walk(l, k + 1, m2, opt, ext);
                if (occ_on) occ[(size_t)x] = saved_occ;
                st.val[(size_t)p] = saved;
            };
            // A position whose every member is a constant `id` vertex is materialised from
            // the same per-label id index the serial path uses: one lookup per anchor, not a
            // scan of the label class.  Positions with an unanchored member keep the scan.
            std::vector<uint32_t> id_roots;
            bool all_id = !at[(size_t)l][(size_t)p].empty();
            for (const auto& b : at[(size_t)l][(size_t)p]) {
                std::vector<uint32_t> vs;
                if (!id_anchored_vertices(g, *E.q[(size_t)b.m], b.var, &vs)) {
                    all_id = false;
                    break;
                }
                id_roots.insert(id_roots.end(), vs.begin(), vs.end());
            }
            if (!all_id) {
                // Same probe for an ordinary property anchor: every member at this position
                // must have one, otherwise the union would miss the unanchored member's class.
                bool all_anch = !at[(size_t)l][(size_t)p].empty();
                for (const auto& b : at[(size_t)l][(size_t)p]) {
                    std::vector<uint32_t> vs;
                    if (!anchored_vertices(g, *E.q[(size_t)b.m], b.var, &vs)) {
                        all_anch = false;
                        break;
                    }
                    id_roots.insert(id_roots.end(), vs.begin(), vs.end());
                }
                if (all_anch) {
                    std::sort(id_roots.begin(), id_roots.end());
                    id_roots.erase(std::unique(id_roots.begin(), id_roots.end()), id_roots.end());
                    for (uint32_t x : id_roots) {
                        take(x, accepted(l, p, x));
                        if (aborted) return;
                    }
                    return;
                }
                id_roots.clear();
            }
            if (all_id) {
                std::sort(id_roots.begin(), id_roots.end());
                id_roots.erase(std::unique(id_roots.begin(), id_roots.end()), id_roots.end());
                if (id_roots.empty()) {
                    if (cnt) ++cnt->init_tuples;
                    return;
                }
                for (uint32_t x : id_roots) {
                    take(x, accepted(l, p, x));
                    if (aborted) return;
                }
                return;
            }
            if (use_mcs && mcs.built((int32_t)p)) {
                const std::vector<uint32_t>& vs = mcs.verts((int32_t)p);
                const std::vector<uint64_t>& ms = mcs.masks((int32_t)p);
                for (uint32_t i = 0; i < vs.size(); ++i) {
                    take(vs[i], ms[i]);
                    if (aborted) return;
                }
            } else if (labels.empty()) {
                for (uint32_t x = 0; x < g.num_vertices(); ++x) {
                    take(x, accepted(l, p, x));
                    if (aborted) return;
                }
            } else {
                for (const auto& lab : labels) {
                    const int32_t id = s.label_of(lab);
                    if (id < 0) continue;
                    for (uint32_t x : g.vertices_of((uint32_t)id)) {
                        take(x, accepted(l, p, x));
                        if (aborted) return;
                    }
                }
            }
            return;
        }
        const int32_t par = P.parent[(size_t)p];
        if (par < 0) return;
        const Link lk = P.link[(size_t)p];
        const Level& L = E.levels[(size_t)l];
        const int32_t se = L.level_skel[(size_t)lk.e];
        const uint32_t from = st.val[(size_t)par];
        if (from == kNil) return;
        // members that are NULL at the parent cannot extend; the optional pass pads them
        const uint64_t rowmask = mask & ~st.absent[(size_t)par];
        if (!rowmask) return;
        const bool from_is_u = !lk.rev;
        if (is_vl[(size_t)l][(size_t)lk.e]) {
            std::vector<uint32_t> vpath;
            vpath.reserve(16);
            if (simple_path) vpath.push_back(from);
            const int32_t from_pos = (use_mcs && mcs.built(E.edges[(size_t)se].w))
                                         ? mcs.position((int32_t)E.edges[(size_t)se].w, from) : -1;
            vl_step(l, k, lk, from, from_pos, 0, from_is_u, rowmask, vpath, opt, ext);
            return;
        }
        if (use_mcs && !lk.rev && mcs.built((int32_t)par)) {
            const int32_t pi = mcs.position((int32_t)par, from);
            if (pi >= 0) {
                const McsEdge& me = mcs.edge(se);
                for (uint32_t sg = me.off[(size_t)pi]; sg < me.off[(size_t)pi + 1]; ++sg) {
                    if (!(rowmask & me.segs[sg].mask)) continue;
                    if (cnt) ++cnt->mask_checks;
                    if (prof) ++prof_mask[(size_t)l];
                    for (uint32_t j = me.segs[sg].begin; j < me.segs[sg].end; ++j) {
                        const uint32_t child =
                            mcs.verts((int32_t)E.edges[(size_t)se].w)[me.child[j]];
                        uint64_t m2 = rowmask & me.segs[sg].mask;
                        m2 = injective_filter(l, lk.w, child, m2);
                        if (!m2) continue;
                        const uint32_t rel = me.rel[j];
                        if (!rel_free(se, rel)) continue;
                        const uint32_t saved_v = st.val[(size_t)lk.w];
                        const uint32_t saved_r = st.rels[(size_t)se];
                        const size_t before = used.size();
                        rel_push((int32_t)se, rel);
                        st.val[(size_t)lk.w] = child;
                        const uint64_t saved_occ = occ_on ? occ[(size_t)child] : 0ULL;
                        if (occ_on) occ[(size_t)child] |= m2;
                        st.rels[(size_t)se] = rel;
                        if (cnt) ++cnt->edges_expanded;
                        if (prof) ++prof_edges[(size_t)l];
                        walk(l, k + 1, m2, opt, ext);
                        if (occ_on) occ[(size_t)child] = saved_occ;
                        st.val[(size_t)lk.w] = saved_v;
                        st.rels[(size_t)se] = saved_r;
                        rel_pop_to(before);
                        if (aborted) return;
                    }
                }
                return;
            }
        }
        const FixedAux& A = faux[(size_t)l][(size_t)lk.e];
        // Reverse selection for a fixed-hop tree edge: when the child's class is much smaller
        // than the parent's fan-out, enumerate the child class and look the edge back up from the
        // child instead of scanning a hub's whole adjacency.  Both ends of the edge row identify
        // the same logical relationship; only the direction bit is complemented on the reverse
        // copy.  The candidate count is the union of the position's label lists; an empty list
        // (any label) keeps the forward scan.
        {
            const std::vector<std::string>& clabels = E.pos[(size_t)lk.w].labels;
            size_t ncand = 0;
            if (!clabels.empty()) {
                for (const auto& lab : clabels) {
                    const int32_t id = s.label_of(lab);
                    if (id >= 0) ncand += g.vertices_of((uint32_t)id).size();
                }
            }
            // 16 = the binary search over an adjacency of ~64k entries; the forward scan pays one
            // `accepted` (label word + predicate checks) per entry of the parent's fan-out.
            if (ncand > 0 && ncand * 16 < (size_t)g.degree(from)) {
                for (const auto& lab : clabels) {
                    const int32_t id = s.label_of(lab);
                    if (id < 0) continue;
                    for (uint32_t child : g.vertices_of((uint32_t)id)) {
                        if (aborted) return;
                        uint64_t m2 = rowmask & accepted(l, lk.w, (int32_t)child);
                        m2 = injective_filter(l, lk.w, child, m2);
                        if (!m2) continue;
                        const AdjView ad = g.all();
                        const auto& gr = g.all_rels();
                        const std::vector<std::pair<uint32_t, uint32_t>>* srt = g.sorted_adj(child);
                        uint32_t lo = (*ad.off)[child], hi = (*ad.off)[child + 1];
                        if (srt) {
                            auto lb = std::lower_bound(srt->begin(), srt->end(), from,
                                                       [](const std::pair<uint32_t, uint32_t>& x,
                                                          uint32_t v) { return x.first < v; });
                            auto ub = std::upper_bound(lb, srt->end(), from,
                                                       [](uint32_t v,
                                                          const std::pair<uint32_t, uint32_t>& x) {
                                                           return v < x.first;
                                                       });
                            for (auto it = lb; it != ub; ++it) {
                                const uint32_t rel = it->second;
                                const uint32_t t = g.edge_type(rel);
                                uint64_t m3 = m2 & A.tbits[(size_t)t];
                                if (!m3) continue;
                                const uint32_t row_dir = (uint32_t)g.edge_dir(rel) ^ 1u;
                                m3 &= A.dir_bits[from_is_u ? 1 : 0][row_dir & 1u];
                                if (!m3) continue;
                                if (prof) ++prof_relfree[(size_t)l];
                                if (!rel_free(se, rel)) continue;
                                if (cnt) { ++cnt->edges_expanded; ++cnt->mask_checks; }
                                if (prof) { ++prof_edges[(size_t)l]; ++prof_mask[(size_t)l]; }
                                const uint32_t saved_v = st.val[(size_t)lk.w];
                                const uint32_t saved_r = st.rels[(size_t)se];
                                const size_t before = used.size();
                                rel_push((int32_t)se, rel);
                                st.val[(size_t)lk.w] = child;
                                const uint64_t saved_occ = occ_on ? occ[(size_t)child] : 0ULL;
                                if (occ_on) occ[(size_t)child] |= m3;
                                st.rels[(size_t)se] = rel;
                                walk(l, k + 1, m3, opt, ext);
                                if (occ_on) occ[(size_t)child] = saved_occ;
                                st.val[(size_t)lk.w] = saved_v;
                                st.rels[(size_t)se] = saved_r;
                                rel_pop_to(before);
                                if (aborted) return;
                            }
                        } else {
                            for (uint32_t j = lo; j < hi; ++j) {
                                if ((*ad.nbr)[j] != from) continue;
                                const uint32_t rel = gr[j];
                                const uint32_t t = g.edge_type(rel);
                                uint64_t m3 = m2 & A.tbits[(size_t)t];
                                if (!m3) continue;
                                const uint32_t row_dir = (uint32_t)g.edge_dir(rel) ^ 1u;
                                m3 &= A.dir_bits[from_is_u ? 1 : 0][row_dir & 1u];
                                if (!m3) continue;
                                if (prof) ++prof_relfree[(size_t)l];
                                if (!rel_free(se, rel)) continue;
                                if (cnt) { ++cnt->edges_expanded; ++cnt->mask_checks; }
                                if (prof) { ++prof_edges[(size_t)l]; ++prof_mask[(size_t)l]; }
                                const uint32_t saved_v = st.val[(size_t)lk.w];
                                const uint32_t saved_r = st.rels[(size_t)se];
                                const size_t before = used.size();
                                rel_push((int32_t)se, rel);
                                st.val[(size_t)lk.w] = child;
                                const uint64_t saved_occ = occ_on ? occ[(size_t)child] : 0ULL;
                                if (occ_on) occ[(size_t)child] |= m3;
                                st.rels[(size_t)se] = rel;
                                walk(l, k + 1, m3, opt, ext);
                                if (occ_on) occ[(size_t)child] = saved_occ;
                                st.val[(size_t)lk.w] = saved_v;
                                st.rels[(size_t)se] = saved_r;
                                rel_pop_to(before);
                                if (aborted) return;
                            }
                        }
                    }
                }
                return;
            }
        }
        for (int32_t t : A.tids) {
            const uint64_t tbits = A.tbits[(size_t)t];
            if (!tbits) continue;
            const AdjView a = g.typed((uint32_t)t);
            const auto& gr = g.typed_rels((uint32_t)t);
            for (uint32_t j = (*a.off)[from]; j < (*a.off)[from + 1]; ++j) {
                const uint32_t child = (*a.nbr)[j];
                const uint32_t rel = gr[j];
                const uint32_t row_dir = (uint32_t)(*a.dir)[j];
                const uint64_t dbits = A.dir_bits[from_is_u ? 1 : 0][row_dir & 1u];
                if (!dbits) continue;
                if (prof) ++prof_relfree[(size_t)l];
                if (!rel_free(se, rel)) continue;
                const uint64_t ok = accepted(l, lk.w, child);
                uint64_t m2 = rowmask & tbits & dbits & ok;
                m2 = injective_filter(l, lk.w, child, m2);
                if (!m2) continue;
                if (cnt) { ++cnt->edges_expanded; ++cnt->mask_checks; }
                if (prof) { ++prof_edges[(size_t)l]; ++prof_mask[(size_t)l]; }
                const uint32_t saved_v = st.val[(size_t)lk.w];
                const uint32_t saved_r = st.rels[(size_t)se];
                const size_t before = used.size();
                rel_push((int32_t)se, rel);
                st.val[(size_t)lk.w] = child;
                const uint64_t saved_occ = occ_on ? occ[(size_t)child] : 0ULL;
                if (occ_on) occ[(size_t)child] |= m2;
                st.rels[(size_t)se] = rel;
                walk(l, k + 1, m2, opt, ext);
                if (occ_on) occ[(size_t)child] = saved_occ;
                st.val[(size_t)lk.w] = saved_v;
                st.rels[(size_t)se] = saved_r;
                rel_pop_to(before);
                if (aborted) return;
            }
        }
    };

    // A member's private suffix alone, from the current core row.  `mask` is one bit.
    priv_process = [&](int32_t m, int32_t idx, uint64_t mask) {
        if (aborted) return;
        if (idx >= (int32_t)mlevels[(size_t)m].size()) {
            if (cnt) cnt->solutions += (uint64_t)__builtin_popcountll(mask);
            if (prof) {
                prof_sol += (uint64_t)__builtin_popcountll(mask);
                for (int mm = 0; mm < M; ++mm)
                    if ((mask >> mm) & 1ULL) ++prof_member_sol[mm];
            }
            maskvec[0] = mask;
            if (sink) {
                if (prof) {
                    const double s0 = pnow();
                    sink->put(st.val, st.rels, st.absent, maskvec);
                    prof_sink_sec += pnow() - s0;
                    ++prof_sink_rows;
                } else {
                    sink->put(st.val, st.rels, st.absent, maskvec);
                }
            }
            return;
        }
        const int32_t l = mlevels[(size_t)m][(size_t)idx];
        if (prof) ++prof_priv_walks[(size_t)m];
        if (cnt) cnt->tuples += (uint64_t)__builtin_popcountll(mask);
        if (prof) prof_tuples[(size_t)l] += (uint64_t)__builtin_popcountll(mask);
        const int32_t save_m = priv_m, save_i = priv_i;
        const bool save_active = priv_active;
        priv_m = m;
        priv_i = idx;
        priv_active = true;
        const double ps0 = prof ? pnow() : 0.0;
        walk(l, 0, mask, 0, nullptr);
        if (prof) prof_priv_sec += pnow() - ps0;
        priv_m = save_m;
        priv_i = save_i;
        priv_active = save_active;
    };
    // Called when a level's walk completes.  In private mode continue this member's next
    // private level; at the end of the core run every alive member's suffix once.
    advance = [&](int32_t l, uint64_t mask) {
        if (priv_active) {
            priv_process(priv_m, priv_i + 1, mask);
            return;
        }
        if (E.topology && l == 0) {
            if (cnt) cnt->tuples += (uint64_t)__builtin_popcountll(mask);
            if (prof) {
                prof_tuples[(size_t)1] += (uint64_t)__builtin_popcountll(mask);
                ++prof_core_rows;
                std::vector<uint32_t> cv;
                for (int32_t pp = 0; pp < E.core_positions; ++pp) cv.push_back(st.val[(size_t)pp]);
                prof_core_set.insert(std::move(cv));
                for (int m = 0; m < M; ++m)
                    if ((mask >> m) & 1ULL) ++prof_core_m[m];
            }
            for (int m = 0; m < M; ++m)
                if ((mask >> m) & 1ULL) priv_process(m, 0, 1ULL << m);
            return;
        }
        process(l + 1, mask);
    };

    // Descend into a level: split the members that participate (walk) from those that do not
    // (pass through with the level's new positions NULL); after the walk, pad the OPTIONAL
    // members that found no extension (left outer join).
    process = [&](int32_t l, uint64_t mask) {
        const double pt0 = prof ? pnow() : 0.0;
        if (aborted) { if (prof) prof_incl[(size_t)l] += pnow() - pt0; return; }
        if (budget_hit()) { if (prof) prof_incl[(size_t)l] += pnow() - pt0; return; }
        // W: the masked path and the per-candidate path count the same logical intermediate
        // rows.  A word at level l carries one row per member alive in its mask; the empty
        // prefix before level 0 is not a row, so only l >= 1 contributes.
        if (cnt && l >= 1) cnt->tuples += (uint64_t)__builtin_popcountll(mask);
        if (prof && l >= 1) prof_tuples[(size_t)l] += (uint64_t)__builtin_popcountll(mask);
        if (l > maxl) {
            if (cnt) cnt->solutions += (uint64_t)__builtin_popcountll(mask);
            if (prof) {
                prof_sol += (uint64_t)__builtin_popcountll(mask);
                for (int mm = 0; mm < M; ++mm)
                    if ((mask >> mm) & 1ULL) ++prof_member_sol[mm];
            }
            maskvec[0] = mask;
            if (sink) {
                if (prof) {
                    const double s0 = pnow();
                    sink->put(st.val, st.rels, st.absent, maskvec);
                    prof_sink_sec += pnow() - s0;
                    ++prof_sink_rows;
                } else {
                    sink->put(st.val, st.rels, st.absent, maskvec);
                }
            }
            if (prof) prof_incl[(size_t)l] += pnow() - pt0;
            return;
        }
        const uint64_t part = partl[(size_t)l], opt = optl[(size_t)l];
        if (dbg_align) {
            std::fprintf(stderr, "process level %d mask %llx part %llx\n", l,
                         (unsigned long long)mask, (unsigned long long)part);
        }
        const uint64_t pass = mask & ~part;
        const uint64_t walkmask = mask & part;
        if (pass) {
            if (prof) prof_t0 = pnow();
            for (size_t p = 0; p < NP; ++p)
                if (newposl[(size_t)l][p]) st.absent[p] |= pass;
            if (prof) { prof_split[(size_t)l] += pnow() - prof_t0; prof_t0 = pnow(); }
            process(l + 1, pass);
            if (prof) prof_t0 = pnow();
            for (size_t p = 0; p < NP; ++p)
                if (newposl[(size_t)l][p]) st.absent[p] &= ~pass;
            if (prof) prof_split[(size_t)l] += pnow() - prof_t0;
        }
        if (!walkmask) { if (prof) prof_incl[(size_t)l] += pnow() - pt0; return; }
        uint64_t ext = 0;
        if (prof) ++prof_walkrows[(size_t)l];
        walk(l, 0, walkmask, opt, opt ? &ext : nullptr);
        if (!opt) { if (prof) prof_incl[(size_t)l] += pnow() - pt0; return; }
        const uint64_t pad = walkmask & opt & ~ext;
        if (!pad) { if (prof) prof_incl[(size_t)l] += pnow() - pt0; return; }
        if (prof) prof_t0 = pnow();
        for (size_t p = 0; p < NP; ++p)
            if (newposl[(size_t)l][p]) st.absent[p] |= pad;
        if (prof) { prof_split[(size_t)l] += pnow() - prof_t0; prof_t0 = pnow(); }
        process(l + 1, pad);
        if (prof) prof_t0 = pnow();
        for (size_t p = 0; p < NP; ++p)
            if (newposl[(size_t)l][p]) st.absent[p] &= ~pad;
        if (prof) { prof_split[(size_t)l] += pnow() - prof_t0; prof_incl[(size_t)l] += pnow() - pt0; }
    };

    stage_mark("before-process");
    const double stage_walk0 = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    stage_mark("after-fn");
    process(0, full);
    const double stage_walk1 = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (stage) {
        std::fprintf(stderr,
                     "[stage] run=%.6f prewalk=%.6f mcs=%.6f walk=%.6f np=%zu maxl=%d\n",
                     stage_walk1 - stage_t0, stage_walk0 - stage_t0, stage_mcs,
                     stage_walk1 - stage_walk0, NP, maxl);
        std::fflush(stderr);
    }
    if (prof) {
        std::fprintf(stderr, "[prof] env members=%d core_pos=%d np=%zu maxl=%d sink_rows=%.0f sink_sec=%.6f sol=%llu\n",
                     M, E.core_positions, NP, maxl, prof_sink_rows, prof_sink_sec,
                     (unsigned long long)prof_sol);
        std::fprintf(stderr, "[prof] core_unique=%zu priv_sec=%.6f\n", prof_core_set.size(),
                     prof_priv_sec);
        std::fprintf(stderr, "[prof] core_rows=%llu core_m",
                     (unsigned long long)prof_core_rows);
        for (int mm = 0; mm < M; ++mm)
            std::fprintf(stderr, " %d:%llu", mm, (unsigned long long)prof_core_m[mm]);
        std::fprintf(stderr, " priv_walks");
        for (int mm = 0; mm < M; ++mm)
            std::fprintf(stderr, " %d:%llu", mm, (unsigned long long)prof_priv_walks[mm]);
        std::fprintf(stderr, "\n");
        std::fprintf(stderr, "[prof] member_sol");
        for (int mm = 0; mm < M; ++mm)
            std::fprintf(stderr, " %d:%llu", mm, (unsigned long long)prof_member_sol[mm]);
        std::fprintf(stderr, "\n");
        for (int32_t l = 0; l <= maxl + 1; ++l) {
            std::fprintf(stderr,
                         "[prof] lvl=%d incl=%.6f excl=%.6f split=%.6f tuples=%llu edges=%llu init=%llu vl=%llu mask=%llu walks=%llu\n",
                         l, prof_incl[(size_t)l],
                         prof_incl[(size_t)l] - (l < maxl + 1 ? prof_incl[(size_t)l + 1] : 0.0),
                         prof_split[(size_t)l],
                         (unsigned long long)prof_tuples[(size_t)l],
                         (unsigned long long)prof_edges[(size_t)l],
                         (unsigned long long)prof_init[(size_t)l],
                         (unsigned long long)prof_vl[(size_t)l],
                         (unsigned long long)prof_mask[(size_t)l],
                         (unsigned long long)prof_walkrows[(size_t)l]);
            std::fprintf(stderr,
                         "[prof2] lvl=%d atom=%llu type=%llu typecmp=%llu close=%llu relfree=%llu\n",
                         l, (unsigned long long)prof_atom[(size_t)l],
                         (unsigned long long)prof_type[(size_t)l],
                         (unsigned long long)prof_typecmp[(size_t)l],
                         (unsigned long long)prof_close[(size_t)l],
                         (unsigned long long)prof_relfree[(size_t)l]);
        }
    }
    return !aborted;
}

}  // namespace v3
