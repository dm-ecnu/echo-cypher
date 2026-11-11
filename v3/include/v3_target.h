// ECHO v3 target-distance pruning for variable-length edges (paper §V-B depth layer).
//
// A variable-length edge is walked from its bound endpoint; a vertex w reached at depth d can
// only continue to the edge's target when d + delta(w) <= hi, where delta(w) is the number of
// hops from w to a target vertex along the edge's type set and direction.  delta is one reverse
// BFS per (target anchor or target candidate set, label set, types, direction, upper bound),
// computed the first time the round needs it and reused by every input row, candidate and
// envelope of that round.  Previously the per-candidate path rebuilt the whole-graph BFS once
// per row (opp12) and the masked path did not prune at all.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "v3_graph.h"
#include "v3_ir.h"

namespace v3 {

// Resolve the target of `pe` when it is walked from `from_var` to `to_var`.  An anchored target
// (`{id: c}` / `WHERE x.id = c`) gives the exact anchor vertex; otherwise the target class is
// the union of the variable's label lists.  `hi` is the hop upper bound of the edge (a negative
// value means unbounded, for which no distance is built).  Returns false when no useful target
// set exists; the caller then expands without pruning.
bool target_spec(const GraphStore& g, const Query& q, const PatternEdge& pe, int32_t from_var,
                 int32_t to_var, int32_t hi, std::string* key, std::vector<uint32_t>* sources);

// Multi-source reverse BFS from `sources` along the reverse of the walked direction.
std::vector<int32_t> target_bfs(const GraphStore& g, const PatternEdge& pe, bool forward,
                                int32_t hi, const std::vector<uint32_t>& sources);

class TargetDistCache {
public:
    // Distance vector for the edge's target, or nullptr when the edge has no anchored / selective
    // target (or the cache is full).  `forward` is true when the walk follows pe.from -> pe.to.
    const std::vector<int32_t>* get(const GraphStore& g, const Query& q, const PatternEdge& pe,
                                    int32_t from_var, int32_t to_var, int32_t hi, bool forward);

    size_t entries() const { return map_.size(); }
    uint64_t bytes() const { return bytes_; }
    void clear() { map_.clear(); bytes_ = 0; }

private:
    static constexpr size_t kMaxEntries = 64;
    static constexpr uint64_t kMaxBytes = 512ULL << 20;
    std::unordered_map<std::string, std::vector<int32_t>> map_;
    uint64_t bytes_ = 0;
};

}  // namespace v3
