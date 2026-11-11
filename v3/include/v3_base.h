// ECHO v3 reference evaluation on the shared base (v3_base.cpp).
//
// The per-candidate reference of a round is the base's per-query pipeline (the code path of the
// v2 reference, echo::dpiso_run: DPiso filter -> BuildTable -> GQL order -> LFTJ), run on a graph
// view of the candidate built from the v2 export's encoding (see v3_base.cpp).  The view's
// vertices are labelled by the data-side label token (entity: the stored label; connector:
// 20000 + type for an OUT side, 30000 + type for an IN side) and mapped to sidecar nodes.
// Candidates outside the supported shape get `supported == false` and a reason; they are
// counted, never silently approximated.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "v3_graph.h"
#include "v3_ir.h"

class Graph;

namespace v3 {

// Count a pure pattern (one MATCH, COUNT(*), no predicates) with the base's own DPiso + LFTJ
// pipeline on the v2 graph.  The fallback is required to cost no more than per-query execution;
// this path counts without materialising the matchings.
bool base_count_pure(const Graph* data, const Query& q, uint64_t* out, std::string* why);

class BaseQuery {
public:
    struct Result {
        bool ok = false;
        bool supported = false;
        std::string why;
        std::vector<std::vector<uint32_t>> embeds;   // per matching: the sidecar node of each var
        double filter_sec = 0, order_sec = 0, enum_sec = 0;
        uint64_t embeddings = 0;
        uint64_t view_vertices = 0, view_edges = 0;
    };
    static Result run(const GraphStore& g, const std::string& sidecar_dir, const Query& q);
};

}  // namespace v3
