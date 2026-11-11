// ECHO v3 MCS: the round-local masked candidate space.
//
// One MCS belongs to one envelope and is built before its masked walk.  Every skeleton position
// has a candidate array (the vertices its member labels / predicates admit) and a parallel array
// of member masks.  Every skeleton edge has a CSR from *parent position* to *child position*:
// off[pi]..off[pi+1] are the segments of parent candidate pi, a segment groups the children that
// share one member mask, and every entry carries the relationship row.  A walk therefore reads a
// mask once per segment and skips the whole segment when the row mask does not intersect it.
//
// Variable-length edges additionally carry a depth layer: a reverse-BFS lower bound to the target
// class per target candidate, used to clear the members whose upper hop bound cannot be met.
//
// Only candidate vertices and accepted edges are materialised, and the build is capped: a class
// that would cost more than the expansions it saves is not built (the planner's estimate already
// charges the build work to the envelope).
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "v3_align.h"
#include "v3_graph.h"

namespace v3 {

struct Counters;

struct McsSeg {
    uint32_t begin = 0, end = 0;
    uint64_t mask = 0;
};

struct McsEdge {
    std::vector<uint32_t> off;       // parent position -> first segment / end sentinel
    std::vector<McsSeg> segs;
    std::vector<uint32_t> child;     // position in the target class
    std::vector<uint32_t> rel;       // relationship row
    std::vector<int8_t> dist;        // target position -> lower bound to the target (-1: none)
};

struct McsPos {
    std::vector<uint32_t> verts;
    std::vector<uint64_t> masks;     // parallel to verts
};

class Mcs {
public:
    // accept(level, position, vertex, out_mask): the member mask of a vertex before edge
    // refinement (labels, property maps and early predicates).
    using Accept = std::function<void(int32_t, int32_t, uint32_t, uint64_t*)>;

    Mcs(const GraphStore& g, const EnvelopeSpec& env, int K);

    bool eligible() const { return K_ > 1 && K_ <= 64; }
    bool ready() const { return eligible() && !disabled_; }
    void build(const Accept& accept, Counters* cnt);

    bool built(int p) const { return (size_t)p < built_.size() && built_[p]; }
    // The class holds every candidate the member queries admit (a full label scan or an
    // anchor-complete id lookup), not just the one-hop subset an edge layer discovered.
    bool full(int p) const { return (size_t)p < full_.size() && full_[p]; }
    const std::vector<uint32_t>& verts(int p) const { return pos_[(size_t)p].verts; }
    const std::vector<uint64_t>& masks(int p) const { return pos_[(size_t)p].masks; }
    uint64_t mask(int p, uint32_t v) const {
        const int32_t i = position(p, v);
        return i < 0 ? 0ULL : pos_[(size_t)p].masks[(size_t)i];
    }
    uint64_t mask_at(int p, int32_t i) const {
        return (i < 0 || (size_t)i >= pos_[(size_t)p].masks.size()) ? 0ULL
                                                                  : pos_[(size_t)p].masks[(size_t)i];
    }
    // candidate position of vertex v in class p, -1 when it is not a candidate.  During the
    // build the hash index is used; once the build is complete the candidates are sorted by
    // vertex id and the walk binary-searches a compact array instead of hashing every edge.
    int32_t position(int p, uint32_t v) const {
        if ((size_t)p >= index_.size()) return -1;
        if (finalized_) {
            const auto& sv = sorted_[(size_t)p];
            auto it = std::lower_bound(sv.begin(), sv.end(), v,
                                       [](const std::pair<uint32_t, uint32_t>& a, uint32_t b) {
                                           return a.first < b;
                                       });
            return (it != sv.end() && it->first == v) ? (int32_t)it->second : -1;
        }
        auto it = index_[(size_t)p].find(v);
        return it == index_[(size_t)p].end() ? -1 : (int32_t)it->second;
    }
    const McsEdge& edge(int se) const { return edges_[(size_t)se]; }
    uint64_t bytes() const { return bytes_; }

private:
    const GraphStore& g_;
    const EnvelopeSpec& E_;
    int K_;
    std::vector<McsPos> pos_;
    std::vector<std::unordered_map<uint32_t, uint32_t>> index_;
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> sorted_;   // vertex -> position
    std::vector<uint8_t> built_;
    std::vector<uint8_t> full_;
    std::vector<McsEdge> edges_;
    bool disabled_ = false;
    bool finalized_ = false;
    uint64_t scanned_edges_ = 0;
    uint64_t max_scanned_edges_ = 0;
    uint64_t max_candidates_ = 0;
    uint64_t bytes_ = 0;
    bool add(int p, uint32_t v, uint64_t m, Counters* cnt);
    void build_edge(int se, Counters* cnt, const Accept& accept);
    void refine(Counters* cnt);
    void depth_bound(int se);
    void finalize();
};

}  // namespace v3
