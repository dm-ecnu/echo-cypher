// ECHO envelope construction (N2 edit identification, N3 envelope partitioning).
//
// An envelope of a query group S is a pattern E with a projection pi_Q: V(Q) -> V(E) for every
// Q in S. E is built by walking the queries in a common order: at each query vertex the builder
// either places the vertex in a fresh envelope vertex (the queries diverge here) or joins the
// class of a neighbouring class of the same label (they share this vertex). A class with a
// single member is labelled by that vertex; a class with m >= 2 members is an envelope vertex
// whose label is the set of members' labels and whose degree is the set of members' degrees.
//
// N5 verifies every envelope solution against each masked query, so envelope sharing is always
// sound. The builder's only job is to keep the "free" part (classes of one member, i.e. the
// pattern positions that only one query traverses) small, because a free class is searched once
// per query anyway.
#pragma once

#include <cstdint>
#include <random>
#include <vector>

#include "echo_common.h"

namespace echo {

struct Envelope {
    std::vector<uint32_t> members;                    // global query indices
    uint32_t n = 0;                                   // envelope vertices
    uint32_t q = 0;                                   // terms per envelope vertex (label x degree)
    std::vector<uint32_t> plabel;                     // [u] query label (all members agree)
    uint32_t label_base = 0;                          // shell i holds data label label_base + i
    uint32_t nterms = 0;                              // term_list size
    std::vector<std::vector<uint32_t>> labels;        // [u] label shell ids
    std::vector<std::vector<uint32_t>> degrees;       // [u] pair key per shell (shell, degree)
    std::vector<std::vector<uint32_t>> adj;           // [u] neighbours, sorted
    std::vector<std::vector<uint32_t>> e_label;       // [u] edge label per adjacency position
    std::vector<std::vector<uint32_t>> span_v;        // [u] union of members' vertex classes
    std::vector<std::vector<uint64_t>> reach;         // [u] span as bitset
    std::vector<std::vector<uint64_t>> sep;           // [u] span shells minus own shells, as bitset
    std::vector<std::vector<uint32_t>> map;           // [k][i] -> envelope vertex
    std::vector<char> shared;                         // [u] class has >= 2 members
    std::vector<std::vector<uint32_t>> reps;          // [m][i] envelope vertex of query vertex i
    std::vector<uint32_t> canon;                      // [query * 64 + vertex] canonical key of the label
    uint32_t shared_vertices = 0;      // classes holding vertices of two or more members
    int fitted = 1;                    // 0 when a family member could not be assembled in the class limit
    uint64_t estimated_cost = 0;
    // N1: query vertices with one projection and the same labels replay each other's embeddings.
    std::vector<std::vector<uint32_t>> equiv;  // [m] groups of query vertices
};

// Per-query term table (one entry per distinct label, one per (label, degree)).
struct Terms {
    uint32_t nvert = 0;
    uint32_t nterms = 0;
    std::vector<uint32_t> lab_term, deg_term, plabel;
    std::vector<uint32_t> order;   // greedy matching order
    std::vector<uint32_t> parent;  // parent in the order (kNoMatch for the root)
    // Walk order: r (self loops, placed by their own query, with the DPiso start rule) first,
    // then the query's greedy order, then r0 (placed by a hashing scheme).
    std::vector<uint32_t> oq, orr, rw, rv, rparent;
    std::vector<int> nw, nv;
};

// Data-side pair lists for an envelope's terms in `q` label shells (shell = letter of label i).
struct Cands {
    uint32_t nterms = 0;
    std::vector<std::vector<uint32_t>> term_list;     // term -> data vertices
    std::vector<std::vector<uint32_t>> shell_vlist;   // shell -> data vertices with that label
    std::vector<std::vector<uint32_t>> label_words;   // data label -> data vertices
    std::vector<uint32_t> data_label;
};

Cands build_cands(const Graph* data, const Envelope& env);
// `freq[l]` = number of data vertices carrying shell label l (used for the root choice).
Terms query_terms(const QueryGraph& q, const std::vector<uint32_t>& freq);

// Exact isomorphism test (WL colours as a filter, backtracking for the decision).
bool common_core_size(const QueryGraph& a, const QueryGraph& b, uint32_t min_sz);
bool wl_iso(const QueryGraph& a, const QueryGraph& b);
inline bool exact_isomorphic(const QueryGraph& a, const QueryGraph& b) { return wl_iso(a, b); }
inline uint32_t pair_key(uint32_t shell, uint32_t degree) { return shell * 4096u + std::min(degree, 4095u); }

// N1 + N2 + N3 of a whole workload: each family is assembled into one envelope; families whose
// envelope would keep too little shared structure stay as single-query envelopes.
std::vector<Envelope> build_envelopes(const std::vector<QueryGraph>& queries, const std::vector<uint32_t>& family,
                                      const Graph* data, double max_free_fraction, uint32_t size_limit,
                                      uint64_t seed, double& merge_seconds);

// One envelope over a group (tests and the per-family runner).
// Maximum common induced subgraph with an explicit correspondence (ma[u] = b vertex or -1).
struct CoreMatch {
    std::vector<int> ma;              // [a.n] -> b vertex or -1
    std::vector<uint32_t> colour;     // [a.n] joint 1-WL colour of a's vertices
    uint32_t size = 0;
};
CoreMatch core_match(const QueryGraph& a, const QueryGraph& b, uint64_t budget);
// Relaxed edit alignment: labels ignored, edge agreement maximised.
CoreMatch align_match(const QueryGraph& a, const QueryGraph& b, uint64_t budget);

// Core envelope: shared classes = the family's maximum common induced subgraph with an explicit
// correspondence; private vertices are enumerated per member.
// One member alone: its own classes, no sharing (E5 no-envelope control, N3 split branch).
Envelope make_single_envelope(const std::vector<QueryGraph>& queries, uint32_t q);

Envelope make_core_envelope(const std::vector<QueryGraph>& queries, const std::vector<uint32_t>& grp,
                            const Graph* data, uint32_t min_core);
Envelope make_envelope_of(const std::vector<QueryGraph>& queries, const std::vector<uint32_t>& grp,
                          const Graph* data, double max_free_fraction, uint32_t size_limit, uint64_t seed);

// Families of the workload: queries whose common core with the first member has at least
// `min_overlap` vertices (the generator extends every core by a walk, so the remaining machines
// of size <= min_overlap are single-query pieces).
std::vector<std::vector<uint32_t>> families_of(const std::vector<QueryGraph>& queries, const Graph* data,
                                               uint32_t k, uint32_t min_overlap);
std::vector<std::vector<uint32_t>> families_of_min(const std::vector<QueryGraph>& queries, const Graph* data,
                                                   uint32_t k, uint32_t min_overlap);

// Envelope of one family (empty on failure).
Envelope make_envelope_of(const std::vector<QueryGraph>& queries, const std::vector<uint32_t>& grp,
                          const Graph* data, double max_free_fraction, uint32_t size_limit, uint64_t seed);

}  // namespace echo
