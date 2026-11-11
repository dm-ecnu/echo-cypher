// MASC Sections 4-5: visit-only-once backtracking over shared candidate sets, the candidate
// overlap aware matching order (4.2), and shared block reuse with a block memorization table (5).
//
// Structure of the search.  A node has an active query set S; every query in S carries its own
// prefix.  The node takes the queries at the smallest depth, groups them by the label of their
// next query vertex, and processes one group G:
//   * candidates that are candidates for at least two queries of G form the shared candidate set
//     (SCS); each such data vertex is visited once, checked for every query of G, and the branch
//     continues with the subset of queries it satisfies (visit-only-once, Algorithm 2);
//   * the remaining candidates are unshared and are enumerated per query.
// After G is exhausted the node recurses on S \ G: the two parts are independent, so no cross
// product of their embeddings is ever materialised.
//
// Blocks (Section 5): a query is decomposed at a single cut vertex into a boundary vertex and an
// interior whose labels are disjoint from the rest of the query (Lemma 5.3).  The interior is
// matched last, so its matchings depend only on the boundary mapping; they are enumerated once
// per (block isomorphism class, boundary data vertex) and reused by every query that reaches the
// same boundary mapping.
#include "masc.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <unordered_map>

namespace echo {
namespace {

constexpr uint32_t kInvalid = 0xFFFFFFFFu;

// ---------------------------------------------------------------- block decomposition

void biconnected_components(const QueryGraph& q, std::vector<std::vector<uint32_t>>& comps) {
    const uint32_t n = q.n;
    std::vector<int> disc(n, -1), low(n, 0);
    std::vector<std::pair<uint32_t, uint32_t>> stack;
    int timer = 0;
    std::function<void(uint32_t, uint32_t)> dfs = [&](uint32_t u, uint32_t parent) {
        disc[u] = low[u] = timer++;
        for (uint32_t w : q.adj[u]) {
            if (w == parent) continue;
            if (disc[w] == -1) {
                stack.emplace_back(u, w);
                dfs(w, u);
                low[u] = std::min(low[u], low[w]);
                if (low[w] >= disc[u]) {
                    std::vector<uint32_t> comp;
                    while (true) {
                        auto e = stack.back();
                        stack.pop_back();
                        comp.push_back(e.first);
                        comp.push_back(e.second);
                        if (e.first == u && e.second == w) break;
                    }
                    std::sort(comp.begin(), comp.end());
                    comp.erase(std::unique(comp.begin(), comp.end()), comp.end());
                    comps.push_back(comp);
                }
            } else if (disc[w] < disc[u]) {
                stack.emplace_back(u, w);
                low[u] = std::min(low[u], disc[w]);
            }
        }
    };
    for (uint32_t u = 0; u < n; ++u)
        if (disc[u] == -1) dfs(u, kInvalid);
}

std::string block_signature(const QueryGraph& q, const std::vector<uint32_t>& canon) {
    std::string s;
    const size_t b = canon.size();
    s.reserve(b * 4 + b * b);
    for (uint32_t u : canon) {
        uint32_t l = q.label[u];
        s.append(reinterpret_cast<const char*>(&l), sizeof(l));
    }
    std::string bits(b * b, '\0');
    for (size_t i = 0; i < b; ++i)
        for (size_t j = 0; j < b; ++j)
            if (i != j && std::binary_search(q.adj[canon[i]].begin(), q.adj[canon[i]].end(), canon[j]))
                bits[i * b + j] = 1;
    s.append(bits);
    if (q.predicates() != nullptr) {
        for (uint32_t u : canon) {
            uint32_t count = q.predicates()->constrained[u] ? (uint32_t)q.predicates()->allowed[u].size() : UINT32_MAX;
            s.append(reinterpret_cast<const char*>(&count), sizeof(count));
            if (q.predicates()->constrained[u])
                for (uint32_t v : q.predicates()->allowed[u]) s.append(reinterpret_cast<const char*>(&v), sizeof(v));
        }
    }
    return s;
}

}  // namespace

Block choose_block(const QueryGraph& q) {
    Block best;
    std::vector<std::vector<uint32_t>> comps;
    biconnected_components(q, comps);
    for (const auto& comp : comps) {
        if (comp.size() < 3 || comp.size() > 8) continue;  // memo keys must stay small
        std::vector<char> in(q.n, 0);
        for (uint32_t u : comp) in[u] = 1;
        std::vector<uint32_t> boundary;
        for (uint32_t u : comp) {
            for (uint32_t w : q.adj[u]) {
                if (!in[w]) { boundary.push_back(u); break; }
            }
        }
        if (boundary.size() != 1) continue;  // the paper only reuses blocks with |dB| = 1
        // Lemma 5.3 precondition: no label of the block appears outside it.
        std::map<uint32_t, std::pair<char, char>> label_side;  // label -> (inside, outside)
        for (uint32_t u = 0; u < q.n; ++u) {
            auto& side = label_side[q.label[u]];
            if (in[u]) side.first = 1; else side.second = 1;
        }
        bool disjoint = true;
        for (const auto& kv : label_side)
            if (kv.second.first && kv.second.second) { disjoint = false; break; }
        if (!disjoint) continue;

        std::vector<uint32_t> rest;
        for (uint32_t u : comp)
            if (u != boundary[0]) rest.push_back(u);
        std::sort(rest.begin(), rest.end());
        std::vector<uint32_t> perm(comp.size());
        perm[0] = boundary[0];
        std::string best_sig;
        std::vector<uint32_t> best_perm;
        do {
            for (size_t i = 0; i < rest.size(); ++i) perm[i + 1] = rest[i];
            std::string sig = block_signature(q, perm);
            if (best_perm.empty() || sig < best_sig) {
                best_sig = sig;
                best_perm = perm;
            }
        } while (std::next_permutation(rest.begin(), rest.end()));

        if (!best.valid || comp.size() > best.canon.size()) {
            best.valid = true;
            best.boundary = boundary[0];
            best.canon = best_perm;
            best.sig = best_sig;
            best.interior.assign(best_perm.begin() + 1, best_perm.end());
            best.labels_disjoint = true;
            best.clabel.clear();
            best.cadj.assign(best_perm.size(), {});
            std::map<uint32_t, uint32_t> local;
            for (size_t i = 0; i < best_perm.size(); ++i) local[best_perm[i]] = (uint32_t)i;
            for (size_t i = 0; i < best_perm.size(); ++i) {
                best.clabel.push_back(q.label[best_perm[i]]);
                for (uint32_t w : q.adj[best_perm[i]]) {
                    auto it = local.find(w);
                    if (it != local.end()) best.cadj[i].push_back(it->second);
                }
                std::sort(best.cadj[i].begin(), best.cadj[i].end());
            }
        }
    }
    return best;
}

namespace {

// ---------------------------------------------------------------- search state

struct QState {
    const QueryGraph* g = nullptr;
    std::vector<uint32_t> order;   // position -> query vertex
    std::vector<uint32_t> pos;     // query vertex -> position
    std::vector<uint32_t> assign;  // query vertex -> data vertex (kInvalid if unassigned)
    uint32_t depth = 0;            // number of assigned vertices (= next position)
    Block block;
};

struct Ctx {
    const Graph* data = nullptr;
    uint32_t nq = 0;
    std::vector<QState> qs;
    std::vector<std::vector<std::vector<uint32_t>>>* cand = nullptr;
    MascOptions opt;
    Clock::time_point deadline = Clock::time_point::max();
    std::atomic<bool>* abort = nullptr;
    MascResult* res = nullptr;

    std::unordered_map<uint64_t, std::vector<std::vector<uint32_t>>> memo;
    size_t memo_stored = 0;
    uint64_t nodes = 0;
    bool timed_out = false;
    std::vector<std::vector<uint32_t>> stack_sets;  // in-flight active sets (timeout accounting)
};

inline bool stopped(Ctx& c) {
    if (c.abort && c.abort->load()) { c.timed_out = true; return true; }
    if ((++c.nodes & 255ULL) == 0ULL) {
        if (Clock::now() > c.deadline) {
            c.timed_out = true;
            if (c.abort) c.abort->store(true);
            return true;
        }
    }
    return false;
}

inline bool vertex_used(const QState& s, uint32_t v) {
    for (uint32_t a : s.assign)
        if (a == v) return true;
    return false;
}

inline bool consistent_vertex(Ctx& c, uint32_t qi, uint32_t u, uint32_t v) {
    QState& s = c.qs[qi];
    if (vertex_used(s, v)) return false;
    for (uint32_t w : s.g->adj[u]) {
        uint32_t fw = s.assign[w];
        if (fw != kInvalid && !c.data->checkEdgeExistence(v, fw)) return false;
    }
    return true;
}

void emit(Ctx& c, uint32_t qi) {
    c.res->embeddings[qi] += 1;
    if (!c.opt.collect) return;
    QState& s = c.qs[qi];
    auto& sols = c.res->per_query[qi];
    sols.emplace_back(s.g->n);
    std::vector<uint32_t>& sol = sols.back();
    for (uint32_t u = 0; u < s.g->n; ++u) sol[u] = s.assign[u];
}

// Interior matchings of a block cost the same for every query that shares the block class, so
// they are enumerated on the block-local candidate sets (boundary position 0 fixed to the
// boundary mapping).  This is what makes a memorised entry valid for other queries (Lemma 5.6).
void enumerate_interior(Ctx& c, const Block& blk, uint32_t boundary_value, size_t idx,
                        std::vector<uint32_t>& cur, std::vector<std::vector<uint32_t>>& out) {
    if (idx == blk.interior.size()) {
        out.push_back(cur);
        return;
    }
    const uint32_t pos = (uint32_t)idx + 1;  // canonical position of this interior vertex
    if (pos >= blk.cand.size()) return;
    for (uint32_t v : blk.cand[pos]) {
        if (stopped(c)) return;
        bool ok = true;
        for (size_t k = 0; k < pos; ++k) {
            uint32_t other = (k == 0) ? boundary_value : cur[k - 1];
            if (other == v) { ok = false; break; }  // injectivity inside the block
            bool adjacent = std::binary_search(blk.cadj[pos].begin(), blk.cadj[pos].end(), (uint32_t)k);
            if (adjacent && !c.data->checkEdgeExistence(v, other)) { ok = false; break; }
        }
        if (!ok) continue;
        cur.push_back(v);
        enumerate_interior(c, blk, boundary_value, idx + 1, cur, out);
        cur.pop_back();
        if (c.timed_out) return;
    }
}

// The query's remaining vertices are exactly its block interior and the boundary is assigned:
// matchings of the interior depend only on the boundary mapping (Lemma 5.3 / 5.6).
void finish_block(Ctx& c, uint32_t qi) {
    QState& s = c.qs[qi];
    const uint32_t boundary_value = s.assign[s.block.boundary];
    const uint64_t key = ((uint64_t)s.block.class_id << 32) | (uint64_t)boundary_value;
    auto it = c.memo.find(key);
    if (it != c.memo.end()) {
        c.res->stats.memb_lock_hits += 1;
        for (const auto& m : it->second) {
            for (size_t i = 0; i < m.size(); ++i) s.assign[s.block.interior[i]] = m[i];
            emit(c, qi);
            for (size_t i = 0; i < m.size(); ++i) s.assign[s.block.interior[i]] = kInvalid;
        }
        return;
    }
    c.res->stats.memb_lock_misses += 1;
    std::vector<std::vector<uint32_t>> maps;
    std::vector<uint32_t> cur;
    enumerate_interior(c, s.block, boundary_value, 0, cur, maps);
    if (!c.timed_out) {
        if (c.memo_stored + maps.size() <= c.opt.memo_cap) {
            c.memo_stored += maps.size();
            c.res->stats.memb_lock_stored += maps.size();
            c.memo.emplace(key, maps);
        } else {
            c.res->stats.memb_lock_skipped += 1;
        }
    }
    for (const auto& m : maps) {
        for (size_t i = 0; i < m.size(); ++i) s.assign[s.block.interior[i]] = m[i];
        emit(c, qi);
        for (size_t i = 0; i < m.size(); ++i) s.assign[s.block.interior[i]] = kInvalid;
    }
}

}  // namespace
}  // namespace echo

namespace echo {
namespace {

// ---------------------------------------------------------------- matching order (Section 4.2)

double selectivity(const Ctx& c, uint32_t qi, uint32_t u) {
    double deg = (double)std::max<size_t>(1, c.qs[qi].g->adj[u].size());
    return (double)(*c.cand)[qi][u].size() / deg;
}

// |intersection| / |union| of the candidate sets of a group of (query, vertex) pairs.
double shared_ratio(const Ctx& c, const std::vector<std::pair<uint32_t, uint32_t>>& grp) {
    if (grp.size() < 2) return 1.0;
    std::vector<uint32_t> inter = (*c.cand)[grp[0].first][grp[0].second];
    uint64_t uni = 0;
    for (auto& qu : grp) uni += (*c.cand)[qu.first][qu.second].size();
    for (size_t i = 1; i < grp.size(); ++i) {
        const std::vector<uint32_t>& b = (*c.cand)[grp[i].first][grp[i].second];
        std::vector<uint32_t> out;
        std::set_intersection(inter.begin(), inter.end(), b.begin(), b.end(), std::back_inserter(out));
        inter.swap(out);
    }
    // union = sum |C| - (overlaps); compute exactly by merging the sorted sets.
    std::vector<uint32_t> all;
    for (auto& qu : grp) {
        const std::vector<uint32_t>& b = (*c.cand)[qu.first][qu.second];
        all.insert(all.end(), b.begin(), b.end());
    }
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());
    return all.empty() ? 0.0 : (double)inter.size() / (double)all.size();
}

void build_orders(Ctx& c) {
    const uint32_t nq = c.nq;
    std::vector<std::vector<char>> chosen(nq), eligible(nq);
    for (uint32_t qi = 0; qi < nq; ++qi) {
        const QueryGraph& g = *c.qs[qi].g;
        eligible[qi].assign(g.n, 1);
        for (uint32_t u : c.qs[qi].block.interior) eligible[qi][u] = 0;
        chosen[qi].assign(g.n, 0);
        uint32_t root = kInvalid;
        double best = 1e300;
        for (uint32_t u = 0; u < g.n; ++u) {
            if (eligible[qi][u]) {
                double sc = selectivity(c, qi, u);
                if (sc < best) { best = sc; root = u; }
            }
        }
        if (root == kInvalid) root = 0;
        c.qs[qi].order.clear();
        c.qs[qi].order.push_back(root);
        chosen[qi][root] = 1;
    }

    while (true) {
        std::map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> groups;
        for (uint32_t qi = 0; qi < nq; ++qi) {
            const QueryGraph& g = *c.qs[qi].g;
            size_t eligible_count = 0;
            for (uint32_t u = 0; u < g.n; ++u) eligible_count += eligible[qi][u] ? 1 : 0;
            if (c.qs[qi].order.size() >= eligible_count) continue;
            std::map<uint32_t, std::pair<double, uint32_t>> best_per_label;
            for (uint32_t u = 0; u < g.n; ++u) {
                if (!eligible[qi][u] || chosen[qi][u]) continue;
                bool adjacent = false;
                for (uint32_t w : g.adj[u])
                    if (chosen[qi][w]) { adjacent = true; break; }
                if (!adjacent) continue;
                double sc = selectivity(c, qi, u);
                auto it = best_per_label.find(g.label[u]);
                if (it == best_per_label.end() || sc < it->second.first)
                    best_per_label[g.label[u]] = {sc, u};
            }
            if (best_per_label.empty()) {  // disconnected remainder (should not happen)
                uint32_t pick = kInvalid;
                double sc_best = 1e300;
                for (uint32_t u = 0; u < g.n; ++u) {
                    if (!eligible[qi][u] || chosen[qi][u]) continue;
                    double sc = selectivity(c, qi, u);
                    if (sc < sc_best) { sc_best = sc; pick = u; }
                }
                if (pick != kInvalid) best_per_label[g.label[pick]] = {sc_best, pick};
            }
            for (auto& kv : best_per_label) groups[kv.first].push_back({qi, kv.second.second});
        }
        if (groups.empty()) break;

        double rmax = 0.0;
        std::map<uint32_t, double> ratio;
        for (auto& kv : groups) {
            ratio[kv.first] = shared_ratio(c, kv.second);
            rmax = std::max(rmax, ratio[kv.first]);
        }
        uint32_t best_label = kInvalid;
        size_t best_cov = 0;
        for (auto& kv : groups) {
            if (ratio[kv.first] < rmax - c.opt.ratio_delta) continue;
            size_t cov = 0;
            cov += kv.second.size();
            if (cov > best_cov) { best_cov = cov; best_label = kv.first; }
        }
        if (best_label == kInvalid) break;
        // Queries whose frontier has a vertex of the selected label advance it, so the batch
        // makes progress on the same label and the candidates can be shared.  A query without
        // such a vertex (nothing to share with) advances its own most selective frontier vertex,
        // which keeps the order construction at one layer per vertex instead of one per label.
        std::vector<char> advanced(nq, 0);
        if (groups.count(best_label)) {
            for (auto& qu : groups[best_label]) {
                c.qs[qu.first].order.push_back(qu.second);
                chosen[qu.first][qu.second] = 1;
                advanced[qu.first] = 1;
            }
        }
        for (uint32_t qi = 0; qi < nq; ++qi) {
            if (advanced[qi]) continue;
            const QueryGraph& g = *c.qs[qi].g;
            uint32_t pick = kInvalid;
            double best = 1e300;
            for (uint32_t u = 0; u < g.n; ++u) {
                if (!eligible[qi][u] || chosen[qi][u]) continue;
                bool adjacent = false;
                for (uint32_t w : g.adj[u])
                    if (chosen[qi][w]) { adjacent = true; break; }
                if (!adjacent) continue;
                double sc = selectivity(c, qi, u);
                if (sc < best) { best = sc; pick = u; }
            }
            if (pick != kInvalid) {
                c.qs[qi].order.push_back(pick);
                chosen[qi][pick] = 1;
            }
        }
    }

    // Interior vertices are matched last so that the block's matchings depend on the boundary
    // mapping only (Section 5); inside the block they are ordered most-selective first.
    for (uint32_t qi = 0; qi < nq; ++qi) {
        std::vector<uint32_t> interior = c.qs[qi].block.interior;
        std::vector<uint32_t> left = interior;
        while (!left.empty()) {
            uint32_t pick = kInvalid;
            double best = 1e300;
            for (uint32_t u : left) {
                double sc = selectivity(c, qi, u);
                if (sc < best) { best = sc; pick = u; }
            }
            c.qs[qi].order.push_back(pick);
            left.erase(std::find(left.begin(), left.end(), pick));
        }
        c.qs[qi].pos.assign(c.qs[qi].g->n, 0);
        for (uint32_t i = 0; i < c.qs[qi].order.size(); ++i) c.qs[qi].pos[c.qs[qi].order[i]] = i;
        c.qs[qi].assign.assign(c.qs[qi].g->n, kInvalid);
        c.qs[qi].depth = 0;
    }
}

// ---------------------------------------------------------------- joint backtracking

uint32_t next_vertex(const Ctx& c, uint32_t qi) {
    return c.qs[qi].order[c.qs[qi].depth];
}

size_t shared_candidate_count(Ctx& c, const std::vector<uint32_t>& G) {
    if (G.size() < 2) return 0;
    std::unordered_map<uint32_t, uint32_t> tag;
    for (uint32_t qi : G) {
        uint32_t u = next_vertex(c, qi);
        for (uint32_t v : (*c.cand)[qi][u]) tag[v] += 1;
    }
    size_t n = 0;
    for (auto& kv : tag)
        if (kv.second >= 2) ++n;
    return n;
}

void solve(Ctx& c, std::vector<uint32_t> S);

void enumerate_group(Ctx& c, const std::vector<uint32_t>& G) {
    if (G.empty()) return;
    if (G.size() == 1) {
        uint32_t qi = G[0];
        uint32_t u = next_vertex(c, qi);
        for (uint32_t v : (*c.cand)[qi][u]) {
            if (stopped(c)) return;
            ++c.res->stats.cand_visits;
            if (!consistent_vertex(c, qi, u, v)) continue;
            c.qs[qi].assign[u] = v;
            c.qs[qi].depth += 1;
            solve(c, std::vector<uint32_t>{qi});
            c.qs[qi].depth -= 1;
            c.qs[qi].assign[u] = kInvalid;
            if (c.timed_out) return;
        }
        return;
    }

    // Shared candidate set (SCS): one tag per data vertex, bit i = candidate of G[i].
    std::unordered_map<uint32_t, uint64_t> tag;
    tag.reserve(4096);
    for (size_t i = 0; i < G.size(); ++i) {
        uint32_t u = next_vertex(c, G[i]);
        for (uint32_t v : (*c.cand)[G[i]][u]) tag[v] |= (1ULL << i);
    }
    std::vector<std::pair<uint32_t, uint64_t>> shared;
    shared.reserve(tag.size());
    for (const auto& kv : tag)
        if (__builtin_popcountll(kv.second) >= 2) shared.push_back(kv);
    std::sort(shared.begin(), shared.end(), [](const std::pair<uint32_t, uint64_t>& a,
                                               const std::pair<uint32_t, uint64_t>& b) {
        int pa = __builtin_popcountll(a.second), pb = __builtin_popcountll(b.second);
        return pa != pb ? pa > pb : a.first < b.first;
    });

    for (const auto& kv : shared) {
        if (stopped(c)) return;
        const uint32_t v = kv.first;
        const uint64_t mask = kv.second;
        std::vector<uint32_t> satisfied;
        for (size_t i = 0; i < G.size(); ++i) {
            if (((mask >> i) & 1ULL) == 0ULL) continue;
            ++c.res->stats.cand_visits;
            if (consistent_vertex(c, G[i], next_vertex(c, G[i]), v)) satisfied.push_back(G[i]);
        }
        if (satisfied.empty()) continue;
        for (uint32_t qi : satisfied) {
            c.qs[qi].assign[next_vertex(c, qi)] = v;
            c.qs[qi].depth += 1;
        }
        if (satisfied.size() >= 2) c.res->stats.shared_visits += 1;
        solve(c, satisfied);
        for (uint32_t qi : satisfied) {
            c.qs[qi].depth -= 1;
            c.qs[qi].assign[next_vertex(c, qi)] = kInvalid;
        }
        if (c.timed_out) return;
    }

    // Unshared candidates: the same branch space, enumerated per query.
    for (uint32_t qi : G) {
        size_t idx = 0;
        for (size_t i = 0; i < G.size(); ++i)
            if (G[i] == qi) idx = i;
        uint32_t u = next_vertex(c, qi);
        for (uint32_t v : (*c.cand)[qi][u]) {
            auto it = tag.find(v);
            if (it != tag.end() && __builtin_popcountll(it->second) >= 2) continue;
            if (stopped(c)) return;
            ++c.res->stats.cand_visits;
            if (!consistent_vertex(c, qi, u, v)) continue;
            c.qs[qi].assign[u] = v;
            c.qs[qi].depth += 1;
            solve(c, std::vector<uint32_t>{qi});
            c.qs[qi].depth -= 1;
            c.qs[qi].assign[u] = kInvalid;
            if (c.timed_out) return;
        }
        (void)idx;
    }
}

void solve(Ctx& c, std::vector<uint32_t> S) {
    if (S.empty() || c.timed_out) return;
    if (stopped(c)) return;
    std::vector<uint32_t> keep;
    for (uint32_t qi : S) {
        QState& s = c.qs[qi];
        const uint32_t k = s.g->n;
        if (s.depth == k) {
            emit(c, qi);
            continue;
        }
        if (s.block.valid && s.depth + s.block.interior.size() == k) {
            finish_block(c, qi);
            continue;
        }
        keep.push_back(qi);
    }
    if (keep.empty()) return;

    uint32_t min_depth = kInvalid;
    for (uint32_t qi : keep) min_depth = std::min(min_depth, c.qs[qi].depth);
    std::map<uint32_t, std::vector<uint32_t>> by_label;
    for (uint32_t qi : keep) {
        if (c.qs[qi].depth != min_depth) continue;
        by_label[c.qs[qi].g->label[next_vertex(c, qi)]].push_back(qi);
    }

    uint32_t best_label = kInvalid;
    size_t best_shared = 0, best_size = 0;
    for (auto& kv : by_label) {
        size_t sh = shared_candidate_count(c, kv.second);  // Algorithm 2, line 3
        if (best_label == kInvalid || sh > best_shared ||
            (sh == best_shared && kv.second.size() > best_size)) {
            best_label = kv.first;
            best_shared = sh;
            best_size = kv.second.size();
        }
    }
    std::vector<uint32_t> G = by_label[best_label];

    c.stack_sets.push_back(S);
    enumerate_group(c, G);
    c.stack_sets.pop_back();
    if (c.timed_out) return;

    std::vector<uint32_t> rest;
    for (uint32_t qi : keep) {
        bool in_group = false;
        for (uint32_t g : G)
            if (g == qi) { in_group = true; break; }
        if (!in_group) rest.push_back(qi);
    }
    solve(c, rest);
}

}  // namespace

MascResult masc_run(const Graph* data, const std::vector<QueryGraph>& queries,
                    const std::vector<std::vector<std::vector<uint32_t>>>& cand, const MascOptions& opt,
                    Clock::time_point deadline, std::atomic<bool>* abort, const LabelIndex*) {
    MascResult res;
    const uint32_t nq = (uint32_t)queries.size();
    res.embeddings.assign(nq, 0);
    res.query_timeout.assign(nq, false);
    res.filter_empty.assign(nq, false);
    if (opt.collect) res.per_query.assign(nq, {});

    Ctx c;
    c.data = data;
    c.nq = nq;
    c.cand = const_cast<std::vector<std::vector<std::vector<uint32_t>>>*>(&cand);
    c.opt = opt;
    c.deadline = deadline;
    c.abort = abort;
    c.res = &res;
    c.qs.resize(nq);

    Stopwatch sw;
    std::map<std::string, uint32_t> classes;  // canonical block form -> class id
    std::vector<std::vector<uint32_t>> class_members;
    for (uint32_t qi = 0; qi < nq; ++qi) c.qs[qi].g = &queries[qi];
    for (uint32_t qi = 0; qi < nq; ++qi) {
        c.qs[qi].block = choose_block(queries[qi]);
        Block& blk = c.qs[qi].block;
        if (!blk.valid) continue;
        auto it = classes.find(blk.sig);
        if (it == classes.end()) {
            uint32_t id = (uint32_t)classes.size();
            classes.emplace(blk.sig, id);
            class_members.emplace_back();
            blk.class_id = id;
        } else {
            blk.class_id = it->second;
        }
        class_members[blk.class_id].push_back(qi);
    }
    // Block-local candidate sets are the union of the members' filtered candidates, indexed by
    // canonical position.  Any interior matching that is valid for a member is therefore kept
    // (completeness for every query of the class), and a memorised entry only contains mappings
    // that satisfy the block's labels, edges and injectivity, which Lemma 5.3 makes sufficient
    // for every member.
    std::vector<std::vector<std::vector<uint32_t>>> class_cand(classes.size());
    for (size_t cid = 0; cid < class_cand.size(); ++cid) {
        const Block& proto = c.qs[class_members[cid][0]].block;
        class_cand[cid].resize(proto.canon.size());
        for (size_t pos = 0; pos < proto.canon.size(); ++pos) {
            std::vector<uint32_t>& out = class_cand[cid][pos];
            for (uint32_t qi : class_members[cid]) {
                const std::vector<uint32_t>& src = cand[qi][c.qs[qi].block.canon[pos]];
                out.insert(out.end(), src.begin(), src.end());
            }
            std::sort(out.begin(), out.end());
            out.erase(std::unique(out.begin(), out.end()), out.end());
        }
    }
    for (uint32_t qi = 0; qi < nq; ++qi) {
        Block& blk = c.qs[qi].block;
        if (blk.valid) blk.cand = class_cand[blk.class_id];
    }
    res.stats.block_sec = sw.seconds();
    build_orders(c);
    res.stats.order_sec = sw.seconds();

    std::vector<uint32_t> active;
    for (uint32_t qi = 0; qi < nq; ++qi) {
        bool empty = false;
        for (uint32_t u = 0; u < queries[qi].n; ++u)
            if (cand[qi][u].empty()) { empty = true; break; }
        if (empty) res.filter_empty[qi] = true;
        else active.push_back(qi);
    }

    sw.reset();
    if (!active.empty()) solve(c, active);
    res.stats.enum_sec = sw.seconds();
    res.stats.nodes = c.nodes;
    for (uint32_t qi = 0; qi < nq; ++qi) res.stats.embeddings += res.embeddings[qi];
    res.batch_timeout = c.timed_out;
    if (c.timed_out) {
        for (const auto& set : c.stack_sets)
            for (uint32_t qi : set) res.query_timeout[qi] = true;
    }
    return res;
}

}  // namespace echo
