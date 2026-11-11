// ECHO N4 (N4 lists for the envelope, masks) and N5 (masked envelope enumeration).
#pragma once

#include <atomic>
#include <limits>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

#include "echo_common.h"
#include "echo_envelope.h"

namespace echo {

enum Mode {
    kEcho = 0,        // full ECHO (envelope + masks)
    kNoEnvelope = 1,  // E4 ablation: every member is its own envelope, masks per query
    kNoMask = 2,      // E4 ablation: envelope searched, candidates cut by N4 only, checks at leaves
};

struct EnvRun {
    bool ok = true;
    bool timed_out = false;
    bool candidate_exceeded = false;
    bool budget_exceeded = false;
    bool filter_budget_exceeded = false;
    Clock::time_point n5_deadline = Clock::time_point::max();
    uint64_t discarded_solutions = 0;
    uint64_t solutions = 0;                          // embeddings (sum over members)
    uint64_t nodes = 0;
    uint64_t mask_kills = 0;  // (member, class) placements a mask/edge check removed
    uint64_t member_cell_sum = 0;  // sum of the members' own candidate lists over the classes
    uint64_t cell_sum = 0;   // sum of the class candidate sets after N4 refinement (filter cost)
    uint32_t cell_max = 0;   // largest class candidate set
    uint32_t npos = 0;       // classes in the envelope
    double filter_sec = 0, search_sec = 0;
    std::vector<std::vector<std::vector<uint32_t>>> per_query;  // [m] embeddings
    // Members that left the walk, with the prefixes at which they left. The base pipeline
    // enumerates each of them once and keeps the matches agreeing with one of the prefixes.
    std::vector<std::pair<int, std::vector<std::vector<std::pair<uint32_t, uint32_t>>>>> depart;
};

struct EnvLimits {
    double max_candidates = std::numeric_limits<double>::infinity();
    double attempt_seconds = std::numeric_limits<double>::infinity();
    double walk_seconds = std::numeric_limits<double>::infinity();
};

// Build the N4 term lists of the envelope view (see echo_mask.cpp) and run the walk.
EnvRun run_envelope(const std::vector<QueryGraph>& queries, const Graph* data, const Envelope& env,
                    const std::vector<uint32_t>& label_vertices, int mode, std::atomic<bool>* abort,
                    Clock::time_point deadline, EnvLimits limits = {});

// Ordering plan used by the runner (kept here so that the runner does not depend on DPiso plans).
struct MatcherPlan {
    std::vector<uint32_t> walk;   // position -> query vertex
    std::vector<uint32_t> level;  // query vertex -> step
};
MatcherPlan plan_for(const QueryGraph& q, const std::vector<uint32_t>& label_vertices);

}  // namespace echo
