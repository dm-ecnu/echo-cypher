// ECHO v3 variable-length expansion (depth-masked expansion, operator of M3/M4).
//
// From a start vertex, follow relationships of a type set in one direction (or both), up to
// `hi` hops (hi < 0: unbounded, bounded by the graph size).  Every hop extends a *distinct
// relationship*: the zero hop is the start vertex itself; a path is a tuple
// (start, end, hops, mask) where the mask is the candidate mask under which the path counts.
// The depth-masked expansion takes the candidates' hop ranges [lo_i, hi_i] and produces the paths
// of depth d in [min lo, max hi] with mask = {i : lo_i <= d <= hi_i}; the executor then keeps
// the paths of the candidates that need them.
#pragma once

#include <cstdint>
#include <vector>

#include "v3_graph.h"

namespace v3 {

struct VlSpec {
    int32_t lo = 1;
    int32_t hi = 1;             // -1: unbounded
    std::vector<uint64_t> mask; // per-block candidate mask of the edge
};

struct VlPath {
    uint32_t start = 0;
    uint32_t end = 0;
    uint32_t hops = 0;
};

}  // namespace v3
