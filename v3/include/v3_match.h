// ECHO v3 masked scan / expansion of one level of an envelope (M3/M4).
//
// A row of the growth is (values of the skeleton vertices bound so far, candidate mask).  The
// mask is exact: it holds member m when m's own clause at this level admits the word along the
// growth.  Membership is tested against the restrictors of the skeleton vertex (the data rows of
// the union label set) and of the level edge (the relationships of the union type set) through
// their position indexes, and against member m's own relationship type list for the edge.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "v3_align.h"
#include "v3_graph.h"
#include "v3_ir.h"
#include "v3_mask.h"

namespace v3 {

struct Counters;
class TargetDistCache;

// The receiver of the masked walk's words (M5 in flight): every word with its exact member mask.
// `rels` is the relationship row the walk took on each level edge (kNil when the edge was not
// walked, e.g. a closed edge or a later level); a member's relationship variable reads it.
class MaskSink {
public:
    virtual ~MaskSink() {}
    // `absent[p]` has the members for which skeleton position p is NULL (an OPTIONAL branch
    // that found no extension); the value of p is the one another member bound.
    virtual void put(const std::vector<uint32_t>& sk, const std::vector<uint32_t>& rels,
                     const std::vector<uint64_t>& absent,
                     const std::vector<uint64_t>& mask) = 0;
};

class MaskedMatch {
public:
    // One level of an envelope, K = number of members (slots); every word is handed to `sink`.
    static bool run_impl(const GraphStore& g, const Schema& s, const EnvelopeSpec& E, int K,
                         int32_t level, MaskSink* sink, Counters* cnt, std::string* err,
                         TargetDistCache* tdc = nullptr);
};

}  // namespace v3
