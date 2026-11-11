// MASC (SIGMOD 2026) port on the vendored SubgraphMatching base (bcaa935).
//
// Three mechanisms from the paper, implemented in full (not a simplified variant):
//   1. Section 3  - filter with shared (hierarchical) signatures: DPiso-style refinement
//                   evaluated once per signature cell instead of once per query vertex.
//   2. Section 4  - visit-only-once backtracking over shared candidate sets (SCS), with the
//                   candidate-overlap-aware matching order of Section 4.2.
//   3. Section 5  - shared block reuse: blocks with |boundary| = 1, isomorphic blocks whose
//                   boundaries correspond, and a block memorization table keyed by
//                   (block class, boundary mapping).
//
// Nothing here changes the data graph or the embedding semantics: an embedding is an injective
// label- and edge-preserving map, exactly as tools/perquery.cpp enumerates it.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "echo_common.h"

namespace echo {

// ---------------------------------------------------------------- filtering (Section 3)

struct SigFilterStats {
    uint64_t cells = 0;          // distinct signature cells over all refinement levels
    uint64_t cell_evals = 0;     // candidate-set evaluations actually performed (once per cell)
    uint64_t vertex_evals = 0;   // evaluations a per-query filter would have performed
    uint64_t probes = 0;         // (data vertex, query vertex) candidate checks
    double sec = 0;
};

// Data vertices grouped by label and sorted by degree: an LDF cell (label, degree) is one
// contiguous range, so a cell's candidates are materialised once instead of once per vertex.
struct LabelIndex {
    std::unordered_map<uint32_t, std::vector<uint32_t>> vertices;  // label -> ids, degree-sorted
    std::unordered_map<uint32_t, std::vector<uint32_t>> degrees;   // parallel degrees
    void build(const Graph* g);
    std::vector<uint32_t> ldf(uint32_t label, uint32_t degree) const;
};

// Hierarchical signature filter shared by the whole batch.
// cand[q][u] = candidate data vertices of query vertex u in query q (sorted, superset-safe).
bool signature_filter(const Graph* data, const std::vector<QueryGraph>& queries,
                      std::vector<std::vector<std::vector<uint32_t>>>& cand, SigFilterStats* st);
bool signature_filter(const Graph* data, const LabelIndex& index, const std::vector<QueryGraph>& queries,
                      std::vector<std::vector<std::vector<uint32_t>>>& cand, SigFilterStats* st);

// BFS order used by the refinement (root, order, position, backward/forward neighbours).
struct RefineOrder {
    uint32_t root = 0;
    std::vector<uint32_t> order;                 // position -> query vertex
    std::vector<uint32_t> pos;                   // query vertex -> position
    std::vector<std::vector<uint32_t>> bn, fn;   // backward / forward neighbours
};
RefineOrder build_refine_order(const QueryGraph& q,
                               const std::vector<std::vector<uint32_t>>& cand);

// ---------------------------------------------------------------- enumeration (Sections 4-5)

struct Block {
    bool valid = false;
    uint32_t boundary = 0;                 // the single cut vertex (|dB| = 1)
    std::vector<uint32_t> interior;        // block vertices minus boundary, canonical order
    std::vector<uint32_t> canon;           // block vertices, canon[0] = boundary
    std::string sig;                       // canonical form (labels + adjacency, boundary first)
    uint32_t class_id = 0;                 // isomorphism class of (block, boundary position)
    bool labels_disjoint = false;          // Lemma 5.3 precondition, verified
    // Block-local structure and candidates, indexed by canonical position (0 = boundary).  They
    // depend on the isomorphism class only, so the memorised interior matchings are complete for
    // every query that shares the class (Lemma 5.6), not just for the query that filled the table.
    std::vector<uint32_t> clabel;
    std::vector<std::vector<uint32_t>> cadj;
    std::vector<std::vector<uint32_t>> cand;
};

struct MascStats {
    uint64_t nodes = 0;            // backtracking nodes
    uint64_t cand_visits = 0;      // (candidate, query) validity checks
    uint64_t shared_visits = 0;    // candidate visits that served >= 2 queries at once
    uint64_t memb_lock_hits = 0;   // block memorization table lookups served from the table
    uint64_t memb_lock_misses = 0;
    uint64_t memb_lock_stored = 0; // interior mappings kept in the table
    uint64_t memb_lock_skipped = 0;// tables beyond the memory cap (enumerated, not stored)
    uint64_t embeddings = 0;
    double filter_sec = 0;
    double order_sec = 0;        // block setup + matching order (MASC's pre-enumeration work)
    double block_sec = 0;        // part of order_sec spent on block classes
    double enum_sec = 0;
};

struct MascOptions {
    bool collect = false;                 // keep every embedding (per query)
    double timeout_sec = 120.0;           // per-query limit, MASC's single-query default
    double batch_deadline_sec = 3600.0;   // whole-batch limit (MASC main setting)
    double ratio_delta = 0.1;             // Section 4.2 threshold delta (paper leaves it open)
    size_t memo_cap = 4000000;            // max interior mappings kept in memory
};

struct MascResult {
    bool batch_timeout = false;
    std::vector<bool> query_timeout;
    std::vector<bool> filter_empty;
    std::vector<uint64_t> embeddings;                        // per query
    std::vector<std::vector<std::vector<uint32_t>>> per_query;  // [q] embeddings (collect mode)
    MascStats stats;
};

MascResult masc_run(const Graph* data, const std::vector<QueryGraph>& queries,
                    const std::vector<std::vector<std::vector<uint32_t>>>& cand, const MascOptions& opt,
                    Clock::time_point deadline, std::atomic<bool>* abort,
                    const LabelIndex* index = nullptr);

// Block decomposition used by mechanism 3 (exposed for the assertion script).
Block choose_block(const QueryGraph& q);

}  // namespace echo
