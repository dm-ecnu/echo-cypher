// ECHO v3 M1 round alignment and M2 envelope partitioning.
//
// M1.  Every candidate is brought to a canonical form (variables numbered by first use, label
//      sets / type sets / property maps sorted) and deduplicated.  The candidates of a round are
//      then aligned to one *skeleton*: the union, over the round, of the first MATCH-level
//      pattern.  Each candidate maps its pattern vertices to skeleton positions and its pattern
//      edges to skeleton edges; the skeleton takes the union of the aligned labels, relationship
//      types, directions and hop ranges.
//
// M2.  A candidate can follow the skeleton (be *merged*) when
//        - every vertex it binds to a skeleton position carries exactly that position's labels
//          (the union equals the candidate's set: no relaxation happened for it),
//        - every skeleton edge it writes carries exactly the skeleton's types and direction
//          (its hop range within the skeleton's; an absent edge is a skeleton edge with lo = 0),
//        - its remaining clauses (further MATCH / OPTIONAL MATCH / WITH / RETURN) can run on the
//          skeleton's bindings and its own local vertices, and every predicate becomes evaluable.
//      Candidates that fail are left to the fallback (M6) and the round is planned around them.
//      A merged round is the envelope; the mask makes every unioned choice per candidate.
//
//      The cost choice of M2 (greedy merge while the envelope's estimated work does not exceed
//      the members' own, hard time limit) is applied after the alignment: it splits an envelope
//      only when the estimate says the members are cheaper alone (P3).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "v3_ir.h"

namespace v3 {

class GraphStore;

struct SkeletonPos {
    std::string name;                  // representative variable name
    std::vector<std::string> labels;   // union over the aligned candidates (sorted)
    // Topological envelopes (the seventh edit class) execute the common core first and then
    // one private suffix step per distinct extension.  A position introduced by a private
    // step records that step's execution level here; -1 keeps the clause-level rule.
    int32_t exec_level = -1;
};

struct SkeletonEdge {
    int32_t u = -1, w = -1;            // skeleton positions; direction u -> w as written
    std::vector<std::string> types;    // union of relationship types (empty: any)
    bool any_dir = false;              // traversed in both directions by some candidate
    int32_t lo = 1, hi = 1;            // union of hop ranges (hi = -1: unbounded)
    int32_t clause = -1;               // pattern clause that writes the edge
    int32_t level = 0;                 // clause level (the first MATCH is level 0)
    int32_t exec_level = -1;           // private suffix step; -1 keeps the clause level
    uint32_t owners = 0;               // member bits that write the edge (topological envelope)
    bool core = false;                 // edge of the common core
    // A tree edge belongs to the shared pattern P: every member spanning its endpoints has it
    // with the same direction, so the masked walk can traverse it to bind a class.  A close
    // edge is a member test between two already bound classes and is never used to grow a tree.
    bool tree = true;
};

// The clause-level pattern a skeleton component stands for: one entry per frozen clause
// position of the members (a MATCH-changing clause or a same-existence OPTIONAL MATCH).
struct LevelEdge {
    int32_t u = -1, w = -1;            // skeleton positions; u is the walked-from side in the level
    std::vector<std::string> types;    // union over the members writing this edge here
    std::vector<uint8_t> opt;          // per member: present in that member's clause at all
    int32_t lo = 1, hi = 1;            // over the members that have the edge (0..0: absent)
    bool any_dir = false;
    bool tree = true;                  // false: a close edge (both ends already bound)
};

struct Level {
    std::vector<std::vector<uint8_t>> present;  // member -> skeleton vertex appears at this level
    std::vector<std::vector<uint8_t>> path_dir; // member -> level edge: the edge is walked from its
                                                // skeleton-u side by that member (1) or reversed (0)
    std::vector<uint32_t> opts;
    std::vector<std::vector<std::string>> labels;   // member -> label set of a skeleton vertex
    std::vector<std::vector<std::vector<std::string>>> types;  // member -> edge -> type list
    std::vector<std::vector<uint8_t>> dir;          // member -> edge -> pattern direction as written: 1 = u->w
    std::vector<std::vector<uint8_t>> any_dir;      // member -> edge -> the pattern edge is undirected
    std::vector<std::vector<int32_t>> hop_lo, hop_hi;          // member -> edge
    std::vector<LevelEdge> edges;
    std::vector<int32_t> level_skel;   // level edge -> skeleton edge
    std::vector<uint8_t> opt;          // per member: this level has an OPTIONAL MATCH
    std::vector<int32_t> opt_index;    // per member: which OPTIONAL attempt this level is (-1)
};

struct EnvelopeSpec {
    int32_t vbase = 0;        // skeleton positions of earlier envelopes (round-global numbering)
    std::vector<SkeletonPos> pos;
    std::vector<SkeletonEdge> edges;
    std::vector<int32_t> pos_lev;                        // position -> level that first binds it
    std::vector<int32_t> clause_level;                   // clause -> pattern level (-1: not a pattern)
    std::vector<std::vector<int32_t>> mclause_level;      // per member: its own clause -> level
    std::vector<int32_t> members;                        // round positions, ascending
    // [member]: entry 0 = the member's variable count, entry 1 + i = the skeleton position of
    // the member's i-th variable (relationship variables first, then vertices in first use)
    std::vector<std::vector<int32_t>> vmap;
    std::vector<std::vector<int32_t>> emap;              // [member] query edge -> skeleton edge / -1
    std::vector<std::vector<int32_t>> vlev;              // [member] query var -> first level binding it
    std::vector<std::vector<int32_t>> elev;              // [member] query edge -> level
    // A prefix envelope evaluates the shared classes once; a member whose pattern has a
    // private suffix departs at this clause and the base enumerator completes it from the
    // prefix rows (-1: the shared prefix covers the whole pattern).
    std::vector<int32_t> depart_clause;
    std::vector<Level> levels;                           // level 0 = the first MATCH clause
    std::vector<const Query*> q;                         // [member] its query (set by the executor)
    bool serial = false;                                 // one member: plain per-query plan
    double budget_sec = 0.0;                             // B_S of the merge rule (0: no budget)
    // M2's estimate of this envelope: the members' own work, the merged walk and the MCS build.
    // The build is charged to W(S) and gates whether the MCS is worth building at run time.
    double est_sum = 0.0, est_work = 0.0, est_build = 0.0;
    double est_gain = 0.0;                               // est_sum - est_work (MCS gate)
    bool mcs_worth = true;                               // est_build <= 0.10 * est_gain
    std::string mcs_gate;                                // why the gate accepted / refused
    int32_t prefix_end = -1;                             // last clause of a common-prefix envelope
    double deadline = 0.0;                               // wall-clock deadline (set by the executor)
    bool topology = false;                               // seventh edit class: core + private trie
    int32_t core_positions = 0;                          // positions of the common core
};

struct RoundPlan {
    std::vector<EnvelopeSpec> envelopes;
    std::vector<EnvelopeSpec> prefix_envelopes;          // shared first-MATCH prefixes
    std::vector<int32_t> fallback;                       // M6: not planned into any envelope
    std::vector<int32_t> replay;                         // candidate -> identical earlier candidate
    std::vector<std::string> why;                        // per candidate: split / fallback reason
    // Per-candidate planning ledger (plan-only output; empty in the execution path is fine).
    std::vector<double> est_own, est_before, est_after;  // M2 estimates seen for the candidate
    std::vector<int32_t> env_of, prefix_of;              // full / prefix envelope index or -1
    // Depth-mask chain batch: the normal planner formed no full envelope, but every candidate
    // is a prefix of the same fixed-length directed walk and only the length bound differs.
    bool depth_chain = false;
    std::vector<int32_t> dc_members, dc_lengths;        // ascending by length
    int32_t dc_max_len = 0;
    // Per depth 1..max_len (index 0 unused): logical walks, aggregate edge traversals, and
    // aggregate (depth, vertex) states.  Filled by the depth-chain executor.
    std::vector<uint64_t> dc_counts, dc_edges, dc_states;
    // The same depths under path-level accounting: every walk prefix would traverse its own
    // outgoing edge.  Reported next to the aggregate operator ledger so the paper can state
    // both the materialised work and the path-equivalent work.
    std::vector<uint64_t> dc_logical_edges;
};

RoundPlan align_and_partition(const GraphStore& g, const Round& r, uint32_t k_limit,
                            bool serial_only = false);

// A per-candidate work estimate (edge expansions + rows) used by M2 only.
double estimate_query(const GraphStore& g, const Query& q);

// The measured planning + C_S build cost of a tiny round (seconds), and the same threshold in
// estimated work units at the Method's gamma.  One value for every graph.  Measured on SF1
// IS5 round-0207, five reps: plan 0.278 ms + MCS build 0.030 ms (median), so 0.3 ms; at
// gamma = 1e-6 s/work the threshold is 300 work units.
constexpr double kTinyPlanSec = 3.0e-4;
constexpr double kWorkGamma = 1.0e-6;

// Per-candidate estimate in the order the per-query executor actually uses: a constant-anchored
// vertex is the root (one candidate), not the label class.  Used only by the tiny-round rule.
double estimate_query_serial_order(const GraphStore& g, const Query& q);

// Decide whether the whole round is below the planning + C_S break-even and, if so, prepare the
// canonical replay map.  Called after load and before the timed cells, for every method.
void prepare_tiny_round(const GraphStore& g, Round* r);

}  // namespace v3
