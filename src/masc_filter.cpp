// MASC Section 3: filter with shared (hierarchical) signatures.
//
// The batch is filtered once, not once per query. At every refinement level query vertices of
// all queries are clustered into signature cells (Definition 3.2 / 3.4); a cell's candidate set
// is evaluated once and reused by every member (Algorithm 1). The refinement itself is DPiso's
// rule (Filter Rule 3.1) and follows the base's schedule: LDF initialisation, then three rounds
// alternating backward and forward refinement.
#include "masc.h"

#include <algorithm>
#include <map>
#include <tuple>

namespace echo {
namespace {

bool has_neighbor_in(const Graph* g, uint32_t v, const std::vector<uint32_t>& set) {
    ui cnt = 0;
    const uint32_t* nbrs = g->getVertexNeighbors(v, cnt);
    for (ui i = 0; i < cnt; ++i)
        if (std::binary_search(set.begin(), set.end(), nbrs[i])) return true;
    return false;
}

struct Level {
    std::vector<uint32_t> cell_of;                  // flattened (q,u) -> cell id
    std::vector<std::vector<uint32_t>> cell_cand;    // cell -> candidate data vertices (sorted)
    std::vector<std::vector<uint32_t>> cell_nbrs;    // cell -> neighbouring cells (sorted)
    std::vector<std::vector<uint32_t>> members;      // cell -> flattened (q,u) indices
};

}  // namespace

RefineOrder build_refine_order(const QueryGraph& q,
                               const std::vector<std::vector<uint32_t>>& cand) {
    RefineOrder r;
    r.pos.assign(q.n, 0);
    uint32_t best = 0;
    double best_score = 1e300;
    for (uint32_t u = 0; u < q.n; ++u) {
        double deg = (double)std::max<size_t>(1, q.adj[u].size());
        double score = (double)cand[u].size() / deg;
        if (score < best_score) { best_score = score; best = u; }
    }
    r.root = best;
    // BFS over the undirected query graph.
    std::vector<char> seen(q.n, 0);
    std::vector<uint32_t> queue{best};
    seen[best] = 1;
    r.order.push_back(best);
    for (size_t head = 0; head < queue.size(); ++head) {
        uint32_t u = queue[head];
        std::vector<uint32_t> nbrs = q.adj[u];
        std::sort(nbrs.begin(), nbrs.end(), [&](uint32_t a, uint32_t b) {
            return cand[a].size() != cand[b].size() ? cand[a].size() < cand[b].size() : a < b;
        });
        for (uint32_t w : nbrs) {
            if (seen[w]) continue;
            seen[w] = 1;
            queue.push_back(w);
            r.order.push_back(w);
        }
    }
    // Complete the input forest; connected inputs keep their original order.
    for (uint32_t root=0; root<q.n; ++root) {
        if (seen[root]) continue;
        queue.assign(1,root); seen[root]=1; r.order.push_back(root);
        for (size_t h=0; h<queue.size(); ++h)
            for (uint32_t v : q.adj[queue[h]]) if (!seen[v]) {
                seen[v]=1; queue.push_back(v); r.order.push_back(v);
            }
    }
    for (uint32_t i = 0; i < q.n; ++i) r.pos[r.order[i]] = i;
    r.bn.assign(q.n, {});
    r.fn.assign(q.n, {});
    for (uint32_t u = 0; u < q.n; ++u) {
        for (uint32_t w : q.adj[u]) {
            if (r.pos[w] < r.pos[u]) r.bn[u].push_back(w);
            else r.fn[u].push_back(w);
        }
        std::sort(r.bn[u].begin(), r.bn[u].end());
        std::sort(r.fn[u].begin(), r.fn[u].end());
    }
    return r;
}

void LabelIndex::build(const Graph* g) {
    const uint32_t n = g->getVerticesCount();
    for (uint32_t v = 0; v < n; ++v) vertices[g->getVertexLabel(v)].push_back(v);
    for (auto& kv : vertices) {
        auto& ids = kv.second;
        std::sort(ids.begin(), ids.end(), [g](uint32_t a, uint32_t b) {
            ui da = g->getVertexDegree(a), db = g->getVertexDegree(b);
            return da != db ? da < db : a < b;
        });
        auto& d = degrees[kv.first];
        d.resize(ids.size());
        for (size_t i = 0; i < ids.size(); ++i) d[i] = g->getVertexDegree(ids[i]);
    }
}

std::vector<uint32_t> LabelIndex::ldf(uint32_t label, uint32_t degree) const {
    auto it = vertices.find(label);
    if (it == vertices.end()) return {};
    const auto& ids = it->second;
    const auto& deg = degrees.at(label);
    size_t lo = std::lower_bound(deg.begin(), deg.end(), degree) - deg.begin();
    // Candidate lists are kept sorted by data vertex id: neighbour intersection tests and the
    // SCS merge rely on binary search / merge order.
    std::vector<uint32_t> out(ids.begin() + lo, ids.end());
    std::sort(out.begin(), out.end());
    return out;
}

bool signature_filter(const Graph* data, const std::vector<QueryGraph>& queries,
                      std::vector<std::vector<std::vector<uint32_t>>>& cand, SigFilterStats* st) {
    LabelIndex index;
    index.build(data);
    return signature_filter(data, index, queries, cand, st);
}

bool signature_filter(const Graph* data, const LabelIndex& index, const std::vector<QueryGraph>& queries,
                      std::vector<std::vector<std::vector<uint32_t>>>& cand, SigFilterStats* st) {
    Stopwatch sw;
    SigFilterStats local;
    SigFilterStats& s = st ? *st : local;

    const uint32_t nq = (uint32_t)queries.size();
    std::vector<uint32_t> offset(nq + 1, 0);
    for (uint32_t q = 0; q < nq; ++q) offset[q + 1] = offset[q] + queries[q].n;
    const uint32_t total = offset[nq];

    // ---- level 0: LDF cells, one evaluation per (label, degree) cell.
    Level cur;
    cur.cell_of.assign(total, 0);
    {
        std::map<std::tuple<uint32_t, uint32_t, bool, std::vector<uint32_t>>, uint32_t> ids;
        for (uint32_t q = 0; q < nq; ++q) {
            for (uint32_t u = 0; u < queries[q].n; ++u) {
                bool constrained = queries[q].predicates() && queries[q].predicates()->constrained[u];
                auto key = std::make_tuple(queries[q].label[u], (uint32_t)queries[q].adj[u].size(),
                                          constrained, constrained ? queries[q].predicates()->allowed[u] : std::vector<uint32_t>{});
                auto it = ids.find(key);
                uint32_t id;
                if (it == ids.end()) {
                    id = (uint32_t)cur.cell_cand.size();
                    ids.emplace(key, id);
                    cur.cell_cand.push_back(index.ldf(std::get<0>(key), std::get<1>(key)));
                    if (constrained) {
                        auto& c = cur.cell_cand.back();
                        c.erase(std::remove_if(c.begin(), c.end(), [&](uint32_t v) { return !queries[q].permits(u,v); }), c.end());
                    }
                    if (std::getenv("MASC_DEBUG"))
                        std::fprintf(stderr, "[dbg] label=%u deg=%u ldf=%zu labels_in_index=%zu\n",
                                     std::get<0>(key), std::get<1>(key), cur.cell_cand.back().size(),
                                     index.vertices.size());
                    cur.members.emplace_back();
                    ++s.cell_evals;
                } else {
                    id = it->second;
                }
                cur.cell_of[offset[q] + u] = id;
                cur.members[id].push_back(offset[q] + u);
                ++s.vertex_evals;
            }
        }
        s.cells += cur.cell_cand.size();
    }
    cur.cell_nbrs.assign(cur.cell_cand.size(), {});

    // ---- per-query DPiso refinement orders (root = min |C(u)|/deg(u)).
    std::vector<RefineOrder> orders(nq);
    for (uint32_t q = 0; q < nq; ++q) {
        std::vector<std::vector<uint32_t>> c(queries[q].n);
        for (uint32_t u = 0; u < queries[q].n; ++u) c[u] = cur.cell_cand[cur.cell_of[offset[q] + u]];
        orders[q] = build_refine_order(queries[q], c);
    }

    // ---- rounds 1..3: hierarchical signatures, clustered batch-wide, evaluated per cell.
    for (uint32_t round = 0; round < 3; ++round) {
        const bool backward = (round % 2 == 0);
        Level next;
        next.cell_of.assign(total, 0);
        std::map<std::vector<uint32_t>, uint32_t> ids;
        std::vector<uint32_t> rep;       // a representative flattened vertex per new cell
        std::vector<uint32_t> prev_of;   // previous-level cell of the representative
        for (uint32_t q = 0; q < nq; ++q) {
            const QueryGraph& gq = queries[q];
            const RefineOrder& ro = orders[q];
            for (uint32_t u = 0; u < gq.n; ++u) {
                const std::vector<uint32_t>& nbrs = backward ? ro.bn[u] : ro.fn[u];
                std::vector<uint32_t> key;
                key.push_back(cur.cell_of[offset[q] + u]);
                key.push_back(backward ? 0u : 1u);
                std::vector<uint32_t> ncell;
                for (uint32_t w : nbrs) ncell.push_back(cur.cell_of[offset[q] + w]);
                std::sort(ncell.begin(), ncell.end());
                ncell.erase(std::unique(ncell.begin(), ncell.end()), ncell.end());
                key.insert(key.end(), ncell.begin(), ncell.end());
                auto it = ids.find(key);
                uint32_t id;
                if (it == ids.end()) {
                    id = (uint32_t)next.cell_cand.size();
                    ids.emplace(key, id);
                    next.cell_cand.emplace_back();
                    next.cell_nbrs.push_back(ncell);
                    next.members.emplace_back();
                    rep.push_back(offset[q] + u);
                    prev_of.push_back(cur.cell_of[offset[q] + u]);
                } else {
                    id = it->second;
                }
                next.cell_of[offset[q] + u] = id;
                next.members[id].push_back(offset[q] + u);
            }
        }

        // Evaluation order: backward rounds walk the order forwards (skip the root), forward
        // rounds walk it backwards (skip the last vertex), as in the base's DPisoFilter.
        std::vector<uint32_t> eval_order(next.cell_cand.size());
        for (uint32_t c = 0; c < next.cell_cand.size(); ++c) eval_order[c] = c;
        std::vector<uint32_t> rank(next.cell_cand.size());
        for (uint32_t c = 0; c < next.cell_cand.size(); ++c) {
            uint32_t flat = rep[c];
            uint32_t q = (uint32_t)(std::upper_bound(offset.begin(), offset.end(), flat) - offset.begin()) - 1;
            uint32_t u = flat - offset[q];
            rank[c] = orders[q].pos[u];
        }
        std::sort(eval_order.begin(), eval_order.end(), [&](uint32_t a, uint32_t b) {
            return backward ? rank[a] < rank[b] : rank[a] > rank[b];
        });

        // Evaluate the cells of this round.  Membership of a data vertex in a neighbouring
        // cell's candidate set is answered with a stamped flag array (one stamp per neighbour
        // cell, up to 8 bits) instead of a binary search per neighbour.
        std::vector<uint32_t> stamp(data->getVerticesCount(), 0);
        uint32_t epoch_base = 0;
        const uint32_t kMaxNbrCells = 8;
        for (uint32_t c : eval_order) {
            const std::vector<uint32_t>& prev = cur.cell_cand[prev_of[c]];
            if (prev.empty()) continue;
            const std::vector<uint32_t>& nbrs = next.cell_nbrs[c];
            if (nbrs.size() > kMaxNbrCells) {  // fall back to per-neighbour scans
                std::vector<uint32_t> out;
                for (uint32_t v : prev) {
                    bool ok = true;
                    for (uint32_t nc : nbrs) {
                        if (!has_neighbor_in(data, v, cur.cell_cand[nc])) { ok = false; break; }
                    }
                    if (ok) out.push_back(v);
                }
                next.cell_cand[c].swap(out);
                ++s.cell_evals;
                s.probes += prev.size();
                s.vertex_evals += (uint64_t)next.members[c].size();
                continue;
            }
            epoch_base += 1u << 8;
            uint32_t need = 0;
            for (size_t i = 0; i < nbrs.size(); ++i) {
                const uint32_t bit = 1u << i;
                need |= bit;
                for (uint32_t x : cur.cell_cand[nbrs[i]]) {
                    // A data vertex may be a candidate of several neighbouring cells: keep all
                    // their bits, and reset stamps left over from another cell evaluation.
                    if ((stamp[x] & ~0xFFu) != epoch_base) stamp[x] = epoch_base | bit;
                    else stamp[x] |= bit;
                }
            }
            std::vector<uint32_t> out;
            out.reserve(prev.size());
            for (uint32_t v : prev) {
                ui cnt = 0;
                const uint32_t* nb = data->getVertexNeighbors(v, cnt);
                uint32_t got = 0;
                for (ui i = 0; i < cnt; ++i) {
                    const uint32_t sv = stamp[nb[i]];
                    if ((sv & ~0xFFu) == epoch_base) got |= (sv & 0xFFu);
                }
                if ((got & need) == need) out.push_back(v);
            }
            next.cell_cand[c].swap(out);
            ++s.cell_evals;
            s.probes += prev.size();
            s.vertex_evals += (uint64_t)next.members[c].size();
        }
        s.cells += next.cell_cand.size();
        cur = std::move(next);
    }

    cand.assign(nq, {});
    for (uint32_t q = 0; q < nq; ++q) {
        cand[q].resize(queries[q].n);
        for (uint32_t u = 0; u < queries[q].n; ++u)
            cand[q][u] = cur.cell_cand[cur.cell_of[offset[q] + u]];
    }
    s.sec = sw.seconds();
    return true;
}

}  // namespace echo
