// ECHO v3 depth-masked variable-length expansion (M3/M4).
//
// From a start vertex s, follow relationships whose type is in the edge's type set, in the
// walked direction (or both when the pattern edge is undirected), up to `hi` hops (hi < 0:
// unbounded).  Hops form a simple path: no relationship is used twice (the zero hop is s
// itself).  The expansion adds every path (end, hops) of depth lo..hi with the edge's candidate
// mask; each candidate keeps the depths inside its own [lo_i, hi_i].
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "v3_graph.h"
#include "v3_ir.h"

namespace v3 {

struct VlStats {
    uint64_t steps = 0;   // relationship traversals (edge expansions)
    uint64_t rows = 0;    // paths emitted
};

class Scratch {
public:
    explicit Scratch(uint64_t rels) { if (rels) vis_.assign(rels, 0); }
    std::vector<uint8_t> vis_;
};

// Enumerate the paths of `edge` from `start`.  For every (end, hops) in [lo, hi] calls
// f(end, hops).  The dominance pruning (hop lower bound 0) skips depths that no candidate of
// the edge can use.
bool vl_expand(const GraphStore& g, const PatternEdge& e, int32_t lo, int32_t hi, uint32_t start,
               bool forward, bool both, Scratch* scratch,
               const std::function<void(uint32_t, int32_t)>& f, VlStats* st, std::string* err,
               const std::vector<int32_t>* dist_to_target = nullptr);

// For the envelope: the mask of a variable-length edge per candidate is handled by the caller;
// this expands to the union of the candidates' depths and reports the hop depth (ended at the
// i-th dst) of every candidate, so it needs no extra memory beyond the simple-path stack.
int32_t vl_hi_bound(const GraphStore& g, const PatternEdge& e);

struct VlParams {
    int32_t lo = 1;
    int32_t hi = 1;
    bool any_dir = false;
};

}  // namespace v3
