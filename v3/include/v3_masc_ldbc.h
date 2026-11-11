// MASC (SIGMOD 2026) port over the LDBC round workload.
//
// The frozen MASC port in include/masc.h works on the base framework's undirected,
// untyped Graph and on QueryGraph batches.  LDBC rounds are directed, typed and carry
// vertex id constants, so this adapter keeps the three MASC mechanisms that survive
// the mapping -- a shared signature/filter pass, visit-once over a shared candidate
// set, and per-query enumeration over the filtered sets -- and applies them to the
// v3 GraphStore.  The supported subset is deliberately narrow: one MATCH, fixed
// single-hop edges, no WHERE atom, at most one relationship, one type, one label per
// vertex, and an optional `id` constant.  Everything else returns covered=false and
// is evaluated by the caller through the serial path, exactly as the paper's
// "evaluates the supported subset jointly and the rest one at a time" describes.
//
// Contract of the round: every candidate returns its exact result, i.e. the RETURN
// projection / grouping / aggregation over its matchings, not only a count.  The
// adapter therefore keeps the matchings of every covered candidate and runs the same
// QueryExec::project_only pipeline as the serial executor; the projection time is part
// of the cell's wall time and the produced rows are part of the adapter's work.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "v3_executor.h"
#include "v3_graph.h"
#include "v3_ir.h"

namespace v3 {

struct MascLdbcCandidate {
    bool covered = false;
    std::string why;
    uint64_t solutions = 0;
    uint64_t edges = 0;    // neighbour probes
    uint64_t tuples = 0;   // root scans + output tuples
    uint64_t out_rows = 0; // rows produced by the RETURN pipeline
    double sec = 0.0;
    CandidateResult result;
};

struct MascLdbcRound {
    uint64_t attempted = 0;
    uint64_t covered = 0;
    uint64_t solutions = 0;
    uint64_t edges = 0;
    uint64_t tuples = 0;
    double sec = 0.0;
    std::vector<MascLdbcCandidate> per_query;
};

// Evaluate the supported candidates of `r` in one pass.  The caller runs the
// unsupported candidates through Executor::serial_only and adds the two ledgers.
MascLdbcRound masc_ldbc_run(const GraphStore& g, const Round& r, double cap_sec = 0.0);

}  // namespace v3
