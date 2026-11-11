// ECHO v3: K-bit candidate masks, the per-alias restrictors, and row masks in counted blocks.
//
// A restrictor is the data set a pattern variable may be bound to under a conjunction of
// constraints -- "label in S" for a vertex, "type in T and direction d (plus hop range)" for an
// edge.  It is computed once per distinct restrictor (the dependency analysis of v3_align) and
// shared by every candidate of the envelope; membership is then O(1) per bit.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "v3_graph.h"

namespace v3 {

inline uint32_t mwords(uint32_t n) { return (n + 63u) >> 6; }

struct Mask {
    std::vector<uint64_t> w;
    void init(uint32_t n, bool full) {
        w.assign(mwords(n), 0ULL);
        if (full) {
            for (uint32_t k = 0; k < w.size(); ++k) w[k] = ~0ULL;
            if (n & 63u) w.back() &= (1ULL << (n & 63u)) - 1ULL;
        }
    }
    bool any() const {
        for (uint64_t x : w)
            if (x) return true;
        return false;
    }
    bool empty() const { return !any(); }
    uint32_t count() const {
        uint32_t c = 0;
        for (uint64_t x : w) c += (uint32_t)__builtin_popcountll(x);
        return c;
    }
    bool test(uint32_t i) const { return (w[i >> 6] >> (i & 63)) & 1ULL; }
    void set(uint32_t i) { w[i >> 6] |= 1ULL << (i & 63); }
    void reset(uint32_t i) { w[i >> 6] &= ~(1ULL << (i & 63)); }
    void and_with(const Mask& o) {
        for (uint32_t k = 0; k < w.size(); ++k) w[k] &= k < o.w.size() ? o.w[k] : 0ULL;
    }
    void or_with(const Mask& o) {
        for (uint32_t k = 0; k < w.size() && k < o.w.size(); ++k) w[k] |= o.w[k];
    }
    bool equals(const Mask& o) const { return w == o.w; }
    bool subset_of(const Mask& o) const {
        for (uint32_t k = 0; k < w.size(); ++k) {
            const uint64_t b = k < o.w.size() ? o.w[k] : 0ULL;
            if (w[k] & ~b) return false;
        }
        return true;
    }
    bool operator==(const Mask& o) const { return w == o.w; }
    // restrictor-list membership: `list` is the position index of a Restrictor mask
    bool in_list(const std::vector<uint32_t>& word, const std::vector<uint64_t>& list, uint32_t r,
                 uint32_t pos) const {
        const uint32_t blk = pos >> 5, bit = pos & 31;
        const uint32_t e = blk + 1 < word.size() ? word[blk + 1] : (uint32_t)list.size();
        for (uint32_t j = word[blk]; j < e; ++j)
            if ((list[j] >> r) & (1ULL << bit)) return true;
        return false;
    }
};

// Restrictor with a word-position index: for every 64-bit word with members, `(w << r)` lists
// the members in ascending position, so a sequential walk over blocks of 32 members keeps the
// data order (the order the per-query leaf starts iterate labels / vertices / typed neighbours).
struct Restrictor {
    Mask m;
    uint32_t n = 0;              // bits (vertices or relationships)
    std::vector<uint32_t> word;  // block -> first index into w64 (+ end sentinel)
    std::vector<uint64_t> w64;   // (mask word << r), r = 64 - n + 32 * block
    std::vector<uint32_t> base;  // w64 entry -> first position of its 32-position block
    std::vector<uint8_t> rem;    // 32 - r
    void build(uint32_t nbits);
};

// ---------------------------------------------------------------- row masks
// Rows carry their candidate mask forward.  RowMask counts *blocks*: it is the per-candidate
// mask-bits bag of a node, ROWSIZE(w, j) words of B bits for the block j.  Small blocks (K <= 64)
// carry the masks of up to 64 candidates in one word, so the loop over the restrictions of the
// scan / expansion is unrolled 64 data cases wide.
struct RowMask {
    uint32_t K = 0, B = 0;
    std::vector<uint64_t> w;
    void init(uint32_t k, uint32_t blocks);
    static void Set(uint64_t* dest, const RowMask& m, uint32_t width) {
        std::memcpy(dest, m.w.data(), (size_t)width * sizeof(uint64_t));
    }
    bool same(const uint64_t* a, const uint64_t* b) const;
};

// General-path flags of a (block, candidate) that the unrolled leaf does not prove.
enum : uint8_t {
    RF_ANY = 1,    // no mask check
    RF_SOME = 2,   // require an unvisited member (the ledger of previous members excludes it)
    RF_NEW = 4,    // this tuple is the first of its block for the candidate: ACC |= mask
    RF_ONE = 8,    // first(a, gamma): drop tuples whose mask repeats an earlier one
};

}  // namespace v3
