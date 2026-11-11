// ECHO v3 M2 cost choice: given the round's aligned skeleton and the candidates that can follow
// it, decide which candidates share one envelope.
//
// Estimate (M2): the expected number of initial tuples of a component is min over its variables
// of |label set of the variable| (the union the envelope would scan); the expected cost of a
// candidate alone is the same quantity with its own labels.  A candidate joins the envelope when
// the envelope's estimate for the variables it matches does not exceed the sum of the members'
// own estimates, i.e. when sharing pays under this estimate.  The choice is greedy over the
// candidates in round order and stops at a hard time limit; candidates that are not chosen are
// evaluated alone (M6) inside the same round, so P3 holds by construction under the estimate.
// The loop terminates deterministically, so the same input always yields the same partition.
#pragma once

#include <cstdint>
#include <vector>

#include "v3_align.h"
#include "v3_graph.h"

namespace v3 {

struct Partition {
    std::vector<std::vector<int32_t>> envelopes;   // member candidate indices
    std::vector<int32_t> alone;                    // candidates evaluated by themselves
    std::vector<double> env_est, alone_est;        // estimates (ledger only)
};

class Partitioner {
public:
    Partitioner(const GraphStore& g, uint32_t k_limit);
    // cands: candidate indices, spec.vmap / emap their alignment.  `pos` the skeleton.
    Partition choose(const Round& r, const EnvelopeSpec& spec, double budget_sec) const;

private:
    const GraphStore& g_;
    uint32_t k_;
    std::vector<double> label_sel_;   // |vertices of label| and per-envelope unions
};

}  // namespace v3
