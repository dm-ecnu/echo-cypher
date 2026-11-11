// One shared executor for a round of point queries: a constant-anchored single vertex or one
// fixed typed hop.  The plan is compiled once for the round; same-shape candidates share one
// id-index lookup and one typed-adjacency scan, and the scan result is dispatched to the
// candidates' matchings by the constant that selected them.  ECHO and the per-query baseline
// both enter through Executor::run, so neither side pays for a private fast path.
#pragma once

#include <string>
#include <vector>

#include "v3_exec.h"
#include "v3_ir.h"

namespace v3 {

// Compile the point shape and canonical replay map for one round.  Called after the round is
// loaded and before the timed cells, next to query parsing; it uses only the query IR and the
// schema.  Returns false when the round is not all point candidates.
bool prepare_point_round(const GraphStore& g, Round* r);

// Try the point-round path.  Returns false before touching `out` when any candidate falls
// outside the supported shape (one constant-anchored vertex, or one fixed typed hop from such a
// vertex, with a plain KEY RETURN); the caller then runs the general planner.  On success it
// fills `out`, `cnt`, `plan` and `plan_sec`, and sets `exec_sec` to the execution phase.
bool run_point_round(const GraphStore& g, const Round& r, std::vector<CandidateOut>* out,
                     Counters* cnt, RoundPlan* plan, double* plan_sec, double* exec_sec,
                     std::string* err);

}  // namespace v3
