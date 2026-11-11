// Depth-mask chain batch: a round whose candidates are fixed-length directed walks of the
// same label/type over one chain template and differ only in their length bound.  The planner
// marks such a round only when the normal alignment forms no full envelope; the executor then
// expands the common chain once and keeps one count per depth, which is the depth-mask
// VarExpand of the paper (member i is live at depths l_i..u_i).
#pragma once

#include <cstdint>
#include <vector>

#include "v3_ir.h"

namespace v3 {

class GraphStore;

struct DepthChainSpec {
    bool ok = false;
    int32_t label_id = -1;
    int32_t type_id = -1;
    int32_t max_len = 0;
    std::vector<int32_t> members;   // candidate indices, ascending by length
    std::vector<int32_t> lengths;   // parallel to members
};

// True iff every candidate is a single fixed-length directed chain with one label and one
// relationship type, all lengths distinct, and every shorter chain is a prefix of the longest
// (including the variable names, which fixes the direction).  count(*) is the only projection.
bool detect_depth_chain(const GraphStore& g, const Round& r, DepthChainSpec* out);

}  // namespace v3
