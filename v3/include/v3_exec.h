// ECHO v3 round execution: M1 alignment, M2 partition, M3/M4 masked scan + expansion,
// M5 demultiplex, M6 fallback.
//
// One round R = {q_1..q_K}: every candidate is aligned to the round's skeleton (v3_align); the
// candidates that can follow the skeleton share one masked evaluation (v3_match writes, the
// reference reader in the check tools is the same structure per candidate); every solution of
// the masked run is demultiplexed by its mask into the per-candidate matchings; RETURN (plain
// or grouped) is then evaluated per candidate on those matchings.
//
// The round runner can also emit, per final matching of every candidate, a digest of the
// matching (for the check that the reference and the masked path agree per matching, not only
// per result) -- see run()'s `digests` output.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "v3_align.h"
#include "v3_graph.h"
#include "v3_target.h"
#include "v3_ir.h"
#include "v3_mask.h"

class Graph;

namespace v3 {

// ---------------------------------------------------------------- operator work (ledger)
struct Counters {
    uint64_t init_tuples = 0;       // first rows of each component (scans)
    uint64_t edges_expanded = 0;    // fixed-hop expansions
    uint64_t vl_steps = 0;          // variable-length expansion steps
    uint64_t vl_rows = 0;           // paths produced by the expansion
    uint64_t tuples = 0;            // rows of the growth (joins) incl. solutions
    uint64_t solutions = 0;         // matchings demultiplexed (M5)
    uint64_t rows_agg_in = 0, rows_agg_out = 0;
    uint64_t mask_checks = 0;
    uint64_t pred_evals = 0;
    // MCS construction is charged to the same envelope ledger: the adjacency scans that build
    // the edge layer are edge work, the candidate probes are intermediate tuples.
    uint64_t mcs_edges = 0;
    uint64_t mcs_vertices = 0;
    uint64_t mcs_bytes = 0;
    double mcs_build_sec = 0.0;     // MCS construction, inside the envelope's wall time
    void add(const Counters& o) {
        init_tuples += o.init_tuples; edges_expanded += o.edges_expanded; vl_steps += o.vl_steps;
        vl_rows += o.vl_rows; tuples += o.tuples; solutions += o.solutions;
        rows_agg_in += o.rows_agg_in; rows_agg_out += o.rows_agg_out;
        mask_checks += o.mask_checks; pred_evals += o.pred_evals;
        mcs_edges += o.mcs_edges; mcs_vertices += o.mcs_vertices; mcs_bytes += o.mcs_bytes;
        mcs_build_sec += o.mcs_build_sec;
    }
    // The paper's W: successful edge traversals (fixed-hop, variable-length steps, and the
    // adjacency the MCS build scans) plus intermediate tuples (root scans, MCS candidates, and
    // the growth rows, including complete matchings).  Both the serial Walker and the masked
    // walk charge the same partial rows: one tuple per member per level, so the two ledgers are
    // directly comparable.  Aggregation rows and produced VL paths are not counted.
    uint64_t work() const {
        return init_tuples + edges_expanded + vl_steps + mcs_edges + mcs_vertices + tuples;
    }
};

struct CandidateOut {
    uint32_t ncols = 0;
    std::vector<Value> data;                     // row-major result rows
    std::vector<std::string> labels;             // column names
    std::vector<std::vector<uint32_t>> matchings;  // demultiplexed matchings (query var -> value)
    std::vector<uint64_t> digests;               // digest of each matching (if requested)
    bool planned = false;                        // false: M6 fallback (evaluated alone)
    bool aggregated = false;                     // M5 aggregated the words by (key, mask)
};

class Executor {
public:
    struct Options {
        bool collect = true;
        bool sort = true;
        bool distinct = true;
        bool apply_limit = true;
        long long row_limit = -1;
        double partition_budget_sec = 1.0;
        bool digests = false;
        bool masked_agg = true;   // aggregate an all-aggregating envelope by (key, mask)
        bool enforce_budget = true;   // stop an envelope after B_S and fall back
        bool serial_only = false;     // no merge: one candidate per envelope (P3 assertion)
        const Graph* base_graph = nullptr;   // v2 graph for base_count_pure in Fallback
    };
    Executor(const GraphStore& g, const Schema& s, Options opt);
    bool run(const Round& r, std::vector<CandidateOut>* out, Counters* cnt, RoundPlan* plan_out,
             std::string* err);
    struct Stats {
        uint64_t envelopes = 0, fallbacks = 0, replays = 0, solutions = 0;
        uint64_t prefix_envelopes = 0, prefix_members = 0;
        uint64_t departures = 0;   // members completed by the base after a prefix departure
        double plan_sec = 0.0, env_sec = 0.0, fallback_sec = 0.0;   // E6 timing segments
        double prefix_suffix_sec = 0.0;
    } stats;

private:
    const GraphStore& g_;
    const Schema& s_;
    Options opt_;
    TargetDistCache tdc_;   // reverse-BFS distances of this round's variable-length targets
};

std::string canonical_key(const Query& q);
bool cmp_values(const Value& a, const Value& b, const std::string& op);
Value id_value(const GraphStore& g, bool is_edge, uint32_t x);

// The values of a member's variables in the envelope's value order (relationship variables
// first, then vertices in first use) and the query variable -> envelope position map.  The
// masked walk binds a member through this order; its atoms are indexed by query variable.
std::vector<std::string> member_value_names(const Query& q);
int32_t member_var_pos(const EnvelopeSpec& E, size_t m, const Query& q, int32_t var);

// The vertex-side pattern check of one variable: its label set and its property map (the
// reference and the masked scan must agree on both).
bool vertex_matches(const GraphStore& g, const PatternVar& pv, uint32_t v);

// A variable is constant-anchored when the pattern binds it to a constant: an inline property
// map (`{id: c}`) or a WHERE equality atom with a constant (`WHERE x.id = c`).  The two forms
// are interchangeable for root selection and for the scan filter; otherwise the serial baseline
// starts from a whole unanchored class while the masked path starts from the constant.
bool var_const_anchored(const Query& q, int32_t var);

// The constant `id` anchor of a vertex (inline `{id: c}` or a WHERE equality `x.id = c`).
// Unlike var_const_anchored this is specifically the indexed id column.  id_anchored_vertices
// returns false when the variable is not indexable (no id anchor, no label, unknown labels);
// otherwise it fills every vertex the per-label id index maps this id to, possibly none.
bool const_id_anchor(const Query& q, int32_t var, int64_t* id);
bool id_anchored_vertices(const GraphStore& g, const Query& q, int32_t var,
                          std::vector<uint32_t>* out);

// Roots of a vertex variable with at least one equality constant on an ordinary property (an
// inline  map or a  atom).  Candidates come from the
// (label, property) equality index, so the label class is never scanned; the caller still
// applies its own label, property and mask tests to each returned vertex.  False means the
// variable has no usable anchor and the caller must scan its class.
bool anchored_vertices(const GraphStore& g, const Query& q, int32_t var,
                       std::vector<uint32_t>* out);
// Build the equality index of every anchor column the query can probe, outside timed cells.
void warm_anchors(const GraphStore& g, const Query& q);

// Cardinality of the class a variable scans unanchored: the sum of its label lists, or the whole
// vertex set without labels.  Used by the shared root score of the masked and serial paths.
double var_root_card(const GraphStore& g, const Query& q, int32_t var);

// The WHERE atoms of `clause` that are constant equalities on `var` and can be applied as soon
// as the vertex is bound (no OR group, no negation, no variable right-hand side).
std::vector<int32_t> const_atoms_of(const Query& q, int32_t var, int32_t clause);

}  // namespace v3
