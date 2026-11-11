// Per-query DPiso pipeline on the vendored base (filter + GQL order + LFTJ enumeration).
// This is the "per-query" method; ECHO reuses the very same DPiso filter code for N4.
#pragma once

#include <atomic>
#include <limits>
#include <vector>

#include "echo_common.h"

// LFTJ embedding hook (vendor/SubgraphMatching/matching/EvaluateQuery.h).
// Declared before EvaluateQuery.h is included so the emitter can call it.
namespace echo {
void emit_embedding(VertexID* embedding);
}

namespace echo {

struct PerQueryResult {
    bool ok = true;              // filter produced non-empty candidate sets
    bool timed_out = false;
    double filter_sec = 0;
    double order_sec = 0;        // table build + matching order
    double enum_sec = 0;
    uint64_t embeddings = 0;
    uint64_t filter_checks = 0;  // candidate probes in the DPiso refinement (see echo_dpiso.cpp)
};

struct LfrtControl {
    std::atomic<bool> abort{false};
    Clock::time_point deadline = Clock::time_point::max();
};

// Filter, order and enumerate one query. When `solutions` is non-null every embedding is
// appended (data vertex per query vertex). `control` may be null.
PerQueryResult dpiso_run_pinned(const Graph* data, Graph* query,
                               const std::vector<std::pair<int, uint32_t>>& pins,
                               std::vector<std::vector<uint32_t>>* solutions, LfrtControl* control);
PerQueryResult dpiso_run(const Graph* data, Graph* query, std::vector<std::vector<uint32_t>>* solutions,
                         LfrtControl* control = nullptr);

// DPiso filter only (used by ECHO's N4).
bool dpiso_filter(const Graph* data, Graph* query, ui**& candidates, ui*& candidates_count, ui*& order, TreeNode*& tree);

// Free what dpiso_filter returned.
void release_filter(ui** candidates, ui* candidates_count, ui* order, TreeNode* tree);

}  // namespace echo
