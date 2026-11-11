// Row masks in counted blocks: the per-candidate (mask-bits) bag of a scan / expansion node,
// ROWSIZE(w, j) words of B bits for block j, so K <= 64 candidates share one word and the loop
// over the unrolled per-candidate STARTS is 64 data cases wide.
#include <cstring>

#include "v3_mask.h"

namespace v3 {
void RowMask::init(uint32_t k, uint32_t blocks) {
    K = k;
    B = blocks;
    w.assign((size_t)B * mwords(K), 0ULL);
}

bool RowMask::same(const uint64_t* a, const uint64_t* b) const {
    return std::memcmp(a, b, (size_t)mwords(K) * sizeof(uint64_t)) == 0;
}
}  // namespace v3
