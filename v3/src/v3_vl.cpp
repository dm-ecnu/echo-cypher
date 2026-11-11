// ECHO v3 depth-masked variable-length expansion: paths from a start vertex of depth lo..hi over
// the edge's type set, in the walked direction(s).  The relationship ledger is keyed by the
// logical relationship, not by the directed adjacency row: an undirected relationship has two
// rows, and traversing one and then the other is still using the same relationship twice.
// SIMPLE additionally forbids a repeated vertex on the path; WALK allows both.
#include "v3_vl.h"

#include <algorithm>

#include "v3_exec.h"

namespace v3 {

namespace {
struct Expander {
    const GraphStore& g;
    const PatternEdge& e;
    int32_t lo, hi;
    bool forward, both;
    int32_t hi_bound = 0;
    Scratch* sc;
    const std::function<void(uint32_t, int32_t)>* f;
    VlStats* st;
    const std::vector<int32_t>* dist = nullptr;
    std::vector<uint32_t> vpath;        // SIMPLE: vertices on the current path

    bool rel_ok(uint32_t rel) const {
        const uint32_t t = g.edge_type(rel);
        if (!e.types.empty()) {
            bool ok = false;
            for (const auto& T : e.types)
                if ((uint32_t)g.schema().type_of(T) == t) {
                    ok = true;
                    break;
                }
            if (!ok) return false;
        }
        for (const auto& p : e.props) {
            const int32_t col = g.type_col(t, p.first);
            if (col < 0) return false;
            bool is_null = false;
            const Value x = g.edge_prop(rel, t, (uint32_t)col, &is_null);
            if (is_null || !cmp_values(x, p.second, "=")) return false;
        }
        return true;
    }

    void dfs(uint32_t u, int32_t depth) {
        if (depth >= hi) return;
        const int32_t d = depth + 1;
        if (e.types.size() == 1) {
            const int32_t t = g.schema().type_of(e.types[0]);
            if (t < 0) return;
            const AdjView a = g.typed((uint32_t)t);
            const auto& rels = g.typed_rels((uint32_t)t);
            for (uint32_t k = (*a.off)[u]; k < (*a.off)[u + 1]; ++k) {
                const uint32_t rel = rels[k];
                const uint32_t lid = g.edge_row(rel);
                if ((!e.walk && sc->vis_[lid]) ||
                    (!both && g.edge_dir(rel) != (forward ? 1u : 0u)) || !rel_ok(rel)) continue;
                const uint32_t w = (*a.nbr)[k];
                if (e.simple && std::find(vpath.begin(), vpath.end(), w) != vpath.end()) continue;
                if (dist && ((*dist)[w] < 0 || d + (*dist)[w] > hi_bound)) continue;
                if (!e.walk) sc->vis_[lid] = 1;
                if (st) ++st->steps;
                if (d >= lo) {
                    if (st) ++st->rows;
                    (*f)(w, d);
                }
                vpath.push_back(w);
                dfs(w, d);
                vpath.pop_back();
                if (!e.walk) sc->vis_[lid] = 0;
            }
            return;
        }
        const AdjView a = g.all();
        for (uint32_t k = (*a.off)[u]; k < (*a.off)[u + 1]; ++k) {
            const uint32_t rel = g.all_rels()[k];
            const uint32_t lid = g.edge_row(rel);
            if ((!e.walk && sc->vis_[lid]) ||
                (!both && g.edge_dir(rel) != (forward ? 1u : 0u)) || !rel_ok(rel)) continue;
            const uint32_t w = (*a.nbr)[k];
            if (e.simple && std::find(vpath.begin(), vpath.end(), w) != vpath.end()) continue;
            if (dist && ((*dist)[w] < 0 || d + (*dist)[w] > hi_bound)) continue;
            if (!e.walk) sc->vis_[lid] = 1;
            if (st) ++st->steps;
            if (d >= lo) {
                if (st) ++st->rows;
                (*f)(w, d);
            }
            vpath.push_back(w);
            dfs(w, d);
            vpath.pop_back();
            if (!e.walk) sc->vis_[lid] = 0;
        }
    }
};
}  // namespace

int32_t vl_hi_bound(const GraphStore& g, const PatternEdge& e) {
    if (e.hi >= 0) return e.hi;
    return (int32_t)g.num_vertices();   // a trail uses each relationship once at most
}

bool vl_expand(const GraphStore& g, const PatternEdge& e, int32_t lo, int32_t hi, uint32_t start,
               bool forward, bool both, Scratch* sc, const std::function<void(uint32_t, int32_t)>& f,
               VlStats* st, std::string* err, const std::vector<int32_t>* dist) {
    (void)err;
    if (sc->vis_.size() < g.num_rels()) sc->vis_.assign(g.num_rels(), 0);
    if (dist && (start >= dist->size() || (*dist)[start] < 0 || (*dist)[start] > hi))
        return true;   // even the zero-hop result (start itself) is not a target within hi
    Expander x{g, e, lo, hi, forward, both, hi, sc, &f, st, dist};
    if (e.simple) x.vpath.push_back(start);
    if (lo <= 0) {
        if (st) ++st->rows;
        f(start, 0);
    }
    x.dfs(start, 0);
    return true;
}

}  // namespace v3
