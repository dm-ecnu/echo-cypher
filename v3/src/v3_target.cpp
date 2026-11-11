#include "v3_target.h"

#include <algorithm>
#include <cstdio>

#include "v3_exec.h"

namespace v3 {
namespace {

void append_u32(std::string* key, uint32_t x) {
    key->push_back(';');
    key->append(std::to_string(x));
}

bool anchor_id(const Query& q, int32_t var, int64_t* id) {
    if (var < 0 || (size_t)var >= q.vars.size()) return false;
    const PatternVar& pv = q.vars[(size_t)var];
    for (const auto& p : pv.props) {
        if (p.first != "id" || p.second.is_null || p.second.is_str) continue;
        *id = p.second.i;
        return true;
    }
    for (const auto& a : q.atoms) {
        if (a.var != var || a.prop != "id" || a.col >= 0 || a.post || a.negated) continue;
        if (a.or_group >= 0 || a.cmp.rhs != Comparison::CONST || a.cmp.op != "=") continue;
        if (a.cmp.consts.size() != 1 || a.cmp.consts[0].is_null || a.cmp.consts[0].is_str) continue;
        *id = a.cmp.consts[0].i;
        return true;
    }
    return false;
}

std::vector<uint32_t> label_ids(const GraphStore& g, const PatternVar& pv) {
    std::vector<uint32_t> ids;
    for (const auto& L : pv.labels) {
        const int32_t x = g.schema().label_of(L);
        if (x >= 0) ids.push_back((uint32_t)x);
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

std::vector<uint32_t> type_ids(const GraphStore& g, const PatternEdge& pe) {
    std::vector<uint32_t> ids;
    for (const auto& T : pe.types) {
        const int32_t x = g.schema().type_of(T);
        if (x >= 0) ids.push_back((uint32_t)x);
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

}  // namespace

bool target_spec(const GraphStore& g, const Query& q, const PatternEdge& pe, int32_t from_var,
                 int32_t to_var, int32_t hi, std::string* key, std::vector<uint32_t>* sources) {
    sources->clear();
    key->clear();
    if (hi < 0 || (size_t)to_var >= q.vars.size()) return false;
    const PatternVar& pv = q.vars[(size_t)to_var];
    if (pv.kind != PatternVar::VERTEX) return false;
    const std::vector<uint32_t> lids = label_ids(g, pv);
    const std::vector<uint32_t> tids = type_ids(g, pe);
    if (!pe.types.empty() && tids.empty()) return false;
    const bool forward = (from_var == pe.from);
    int64_t anchor = 0;
    if (anchor_id(q, to_var, &anchor)) {
        // exact target vertex: label-indexed id lookup (or a bounded scan for an unlabelled var)
        if (!lids.empty()) {
            for (uint32_t l : lids) {
                uint32_t v = 0;
                if (g.lookup_vertex(l, anchor, &v)) sources->push_back(v);
            }
        } else if (g.num_vertices() <= 200000) {
            for (uint32_t v = 0; v < g.num_vertices(); ++v)
                if (g.vertex_id(v) == anchor) sources->push_back(v);
        } else {
            return false;
        }
        std::sort(sources->begin(), sources->end());
        sources->erase(std::unique(sources->begin(), sources->end()), sources->end());
        key->append("a");
        key->append(std::to_string(anchor));
    } else {
        // target class: every vertex carrying one of the variable's labels (a superset when the
        // variable also has property predicates, which only weakens the pruning, never the
        // result).  A class of every vertex is not selective enough to pay for a BFS.
        if (lids.empty()) return false;
        for (uint32_t l : lids) {
            const auto& vs = g.vertices_of(l);
            sources->insert(sources->end(), vs.begin(), vs.end());
        }
        std::sort(sources->begin(), sources->end());
        sources->erase(std::unique(sources->begin(), sources->end()), sources->end());
        key->append("c");
        for (uint32_t l : lids) append_u32(key, l);
    }
    key->append("|t");
    for (uint32_t t : tids) append_u32(key, t);
    key->append(pe.any_dir ? "|both|" : (forward ? "|f|" : "|b|"));
    key->append(std::to_string(hi));
    return true;
}

std::vector<int32_t> target_bfs(const GraphStore& g, const PatternEdge& pe, bool forward,
                                int32_t hi, const std::vector<uint32_t>& sources) {
    std::vector<int32_t> dist(g.num_vertices(), -1);
    if (sources.empty() || hi < 0) return dist;
    const std::vector<uint32_t> tids = type_ids(g, pe);
    std::vector<uint64_t> tbits;
    if (tids.size() > 1) {
        tbits.assign((g.schema().types.size() + 63) / 64, 0ULL);
        for (uint32_t t : tids) tbits[t >> 6] |= 1ULL << (t & 63);
    }
    const bool one_type = tids.size() == 1;
    const uint32_t want = forward ? 0u : 1u;   // reverse of the walked direction
    std::vector<uint32_t> queue;
    queue.reserve(sources.size());
    for (uint32_t v : sources) {
        if (dist[v] == 0) continue;
        dist[v] = 0;
        queue.push_back(v);
    }
    for (size_t qi = 0; qi < queue.size(); ++qi) {
        const uint32_t u = queue[qi];
        if (dist[u] >= hi) continue;
        if (one_type) {
            const AdjView a = g.typed(tids[0]);
            const auto& rels = g.typed_rels(tids[0]);
            for (uint32_t j = (*a.off)[u]; j < (*a.off)[u + 1]; ++j) {
                if (!pe.any_dir && (uint32_t)(*a.dir)[j] != want) continue;
                const uint32_t w = (*a.nbr)[j];
                if (dist[w] >= 0) continue;
                dist[w] = dist[u] + 1;
                queue.push_back(w);
            }
        } else {
            const AdjView a = g.all();
            const auto& rels = g.all_rels();
            for (uint32_t j = (*a.off)[u]; j < (*a.off)[u + 1]; ++j) {
                const uint32_t row_dir = (*a.dir)[j];
                if (!pe.any_dir && row_dir != want) continue;
                if (!tbits.empty() && !((tbits[g.edge_type(rels[j]) >> 6] >>
                                         (g.edge_type(rels[j]) & 63)) & 1ULL)) continue;
                const uint32_t w = (*a.nbr)[j];
                if (dist[w] >= 0) continue;
                dist[w] = dist[u] + 1;
                queue.push_back(w);
            }
        }
    }
    return dist;
}

const std::vector<int32_t>* TargetDistCache::get(const GraphStore& g, const Query& q,
                                                 const PatternEdge& pe, int32_t from_var,
                                                 int32_t to_var, int32_t hi, bool forward) {
    std::string key;
    std::vector<uint32_t> sources;
    if (!target_spec(g, q, pe, from_var, to_var, hi, &key, &sources)) return nullptr;
    auto it = map_.find(key);
    if (it != map_.end()) return &it->second;
    if (map_.size() >= kMaxEntries) return nullptr;
    const uint64_t add = (uint64_t)g.num_vertices() * sizeof(int32_t);
    if (bytes_ + add > kMaxBytes) return nullptr;
    std::vector<int32_t> dist = target_bfs(g, pe, forward, hi, sources);
    bytes_ += add;
    auto ins = map_.emplace(std::move(key), std::move(dist));
    return &ins.first->second;
}

}  // namespace v3
