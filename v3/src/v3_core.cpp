// Restrictor word-position index and value comparison semantics.
#include <algorithm>

#include "v3_mask.h"

namespace v3 {

// The restrictor is a set of positions (vertices or relationship rows).  Its blocks are runs of
// B = 32 consecutive positions (the last one shorter): block b holds the members of positions
// [32 b, 32 b + 32) as a bit word, and `word[b]` is the first index in `w64` of block b.  A scan
// over the restrictor walks `w64` in block order and decodes a member's position from the word
// and the block (`rem[b]`, the width of block b's word).  Blocks without members take no entry,
// so a sparse restrictor costs one word per non-empty block.
void Restrictor::build(uint32_t nbits) {
    n = nbits;
    const uint32_t nb = (nbits + 31u) >> 5;   // blocks of 32 positions
    word.assign((size_t)nb + 1, 0);
    w64.clear();
    rem.clear();
    base.clear();
    for (uint32_t b = 0; b < nb; ++b) {
        word[b] = (uint32_t)w64.size();
        const uint32_t lo = 32u * b;
        const uint32_t hi = std::min<uint32_t>(nbits, lo + 32u);
        uint32_t bits = 0;
        for (uint32_t p = lo; p < hi; ++p)
            if ((m.w[p >> 6] >> (p & 63)) & 1ULL) bits |= (1u << (p - lo));
        if (!bits) continue;
        w64.push_back(bits);
        base.push_back(lo);                          // the block's first position
        rem.push_back((uint8_t)(32u - (hi - lo)));   // the block's width
    }
    word[nb] = (uint32_t)w64.size();
}

namespace {
inline int cmp3(int64_t a, int64_t b) { return a < b ? -1 : (a > b ? 1 : 0); }
}  // namespace

// Cypher comparison of two non-NULL values.  Two strings compare bytewise, two numbers
// numerically; a string never equals a number (only <> is true).  NULL is handled by callers.
bool cmp_values(const Value& a, const Value& b, const std::string& op) {
    int c;
    if (a.is_str || b.is_str) {
        if (a.is_str != b.is_str) return op == "<>";
        c = a.s.compare(b.s);
        c = c < 0 ? -1 : (c > 0 ? 1 : 0);
    } else {
        c = cmp3(a.i, b.i);
    }
    if (op == "=") return c == 0;
    if (op == "<>") return c != 0;
    if (op == "<") return c < 0;
    if (op == ">") return c > 0;
    if (op == "<=") return c <= 0;
    if (op == ">=") return c >= 0;
    return false;
}

}  // namespace v3
