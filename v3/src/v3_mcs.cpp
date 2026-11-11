// ECHO v3 MCS: build the masked candidate space of one envelope.
#include "v3_mcs.h"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <chrono>
#include <cstdio>

#include "v3_exec.h"

namespace v3 {

namespace {

}  // namespace

Mcs::Mcs(const GraphStore& g, const EnvelopeSpec& env, int K)
    : g_(g), E_(env), K_(K) {}

bool Mcs::add(int p, uint32_t v, uint64_t m, Counters* cnt) {
    auto& idx = index_[(size_t)p];
    auto it = idx.find(v);
    if (it != idx.end()) {
        pos_[(size_t)p].masks[(size_t)it->second] |= m;
        return true;
    }
    if (pos_[(size_t)p].verts.size() >= max_candidates_) {
        disabled_ = true;
        return false;
    }
    const uint32_t i = (uint32_t)pos_[(size_t)p].verts.size();
    idx.emplace(v, i);
    pos_[(size_t)p].verts.push_back(v);
    pos_[(size_t)p].masks.push_back(m);
    bytes_ += sizeof(uint32_t) + sizeof(uint64_t);
    if (cnt) ++cnt->mcs_vertices;
    return true;
}

static double mcs_now() {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void Mcs::build(const Accept& accept, Counters* cnt) {
    if (!eligible()) return;
    const bool mprof = std::getenv("V3_MCS_PROFILE") != nullptr;
    struct MS {
        const char* n;
        double t0;
        bool on;
        ~MS() {
            if (!on) return;
            std::fprintf(stderr, "[mcs-phase] %s %.6f\n", n, mcs_now() - t0);
            std::fflush(stderr);
        }
    };
    const MS ms_all{"all", mcs_now(), mprof};
    const size_t NP = E_.pos.size(), NE = E_.edges.size();
    const size_t kClassMax = (size_t)std::max(1, std::atoi(std::getenv("V3_MCS_CLASS_MAX") ?: "100000"));
    max_scanned_edges_ = (uint64_t)std::max(1.0, std::atof(std::getenv("V3_MCS_MAX_EDGES") ?: "10000"));
    // A candidate space above 50k is not a shared class in practice: the root scan and the
    // first edge layer already cost more than the walk they prune (SF1 IS6/IS7 measured
    // 10-18 ms of build for an empty walk).  Keep the override for experiments.
    max_candidates_ = (uint64_t)std::max(1.0, std::atof(std::getenv("V3_MCS_MAX_CAND") ?: "50000"));
    pos_.assign(NP, McsPos());
    index_.assign(NP, {});
    built_.assign(NP, 0);
    full_.assign(NP, 0);
    std::vector<double> card(NP, 0.0);
    std::vector<uint8_t> anchored(NP, 0);
    std::vector<std::vector<std::pair<int, int>>> bind(NP);
    for (int m = 0; m < K_; ++m) {
        const Query& q = *E_.q[(size_t)m];
        for (size_t v = 0; v < q.vars.size(); ++v) {
            if (q.vars[v].kind != PatternVar::VERTEX) continue;
            const int32_t p = member_var_pos(E_, (size_t)m, q, (int32_t)v);
            if (p < 0 || p >= (int32_t)NP) continue;
            bind[(size_t)p].push_back({m, (int32_t)v});
        }
    }
    for (size_t p = 0; p < NP; ++p) {
        for (const auto& lab : E_.pos[p].labels) {
            const int32_t id = g_.schema().label_of(lab);
            if (id >= 0) card[p] += (double)g_.vertices_of((uint32_t)id).size();
        }
        if (E_.pos[p].labels.empty()) card[p] = (double)g_.num_vertices();
        for (const auto& b : bind[p])
            if (var_const_anchored(*E_.q[(size_t)b.first], b.second)) anchored[p] = 1;
    }
    auto scan_root = [&](size_t p) {
        const bool prof_root = std::getenv("V3_MCS_PROFILE") != nullptr;
        const double t_root = mcs_now();
        built_[p] = 1;
        const int32_t lv = E_.pos_lev[p];
        auto take = [&](uint32_t v) {
            // `accept` already applies the members' label / property / early-predicate tests;
            // a separate fast_mask pass only repeated them and doubled the root scan.
            uint64_t m = 0;
            accept(lv, (int32_t)p, v, &m);
            if (m) add((int)p, v, m, cnt);
        };
        // A position whose every member is a constant `id` vertex is materialised from the
        // same per-label id index the serial and masked root paths use: one lookup per anchor,
        // not a class scan.  This is the common case on the heavy LDBC rounds (Person/Message).
        bool all_id = !bind[p].empty();
        std::vector<uint32_t> roots;
        for (const auto& b : bind[p]) {
            std::vector<uint32_t> vs;
            if (!id_anchored_vertices(g_, *E_.q[(size_t)b.first], b.second, &vs)) {
                all_id = false;
                break;
            }
            roots.insert(roots.end(), vs.begin(), vs.end());
        }
        if (all_id) {
            full_[p] = 1;   // the members' id anchors are the complete candidate set
            std::sort(roots.begin(), roots.end());
            roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
            for (uint32_t v : roots) {
                take(v);
                if (disabled_) return;
            }
            return;
        }
        // An ordinary property anchor probes the (label, property) index instead of scanning
        // the class: the anchor sets of all members at this position are complete on their own.
        {
            bool all_anch = !bind[p].empty();
            std::vector<uint32_t> aroots;
            for (const auto& b : bind[p]) {
                std::vector<uint32_t> vs;
                if (!anchored_vertices(g_, *E_.q[(size_t)b.first], b.second, &vs)) {
                    all_anch = false;
                    break;
                }
                aroots.insert(aroots.end(), vs.begin(), vs.end());
            }
            if (all_anch) {
                full_[p] = 1;
                std::sort(aroots.begin(), aroots.end());
                aroots.erase(std::unique(aroots.begin(), aroots.end()), aroots.end());
                for (uint32_t v : aroots) {
                    take(v);
                    if (disabled_) return;
                }
                return;
            }
        }
        if (E_.pos[p].labels.empty()) {
            for (uint32_t v = 0; v < g_.num_vertices() && !disabled_; ++v) take(v);
        } else {
            for (const auto& lab : E_.pos[p].labels) {
                const int32_t id = g_.schema().label_of(lab);
                if (id < 0) continue;
                for (uint32_t v : g_.vertices_of((uint32_t)id)) {
                    take(v);
                    if (disabled_) return;
                }
            }
        }
        full_[p] = 1;
        if (prof_root)
            std::fprintf(stderr, "[mcs-root] p=%zu lev=%d card=%.0f verts=%zu sec=%.6f\n", p,
                         (int)lv, card[p], pos_[p].verts.size(), mcs_now() - t_root);
    };
    // One root per connected component of the skeleton: the most selective anchor, else the
    // smallest class that fits the build cap.  Scanning every anchored position (the old code)
    // repeated a whole label domain per anchor and dominated the build on SF10.
    std::vector<int32_t> uf(NP);
    for (size_t p = 0; p < NP; ++p) uf[p] = (int32_t)p;
    std::function<int32_t(int32_t)> find = [&](int32_t x) {
        while (uf[(size_t)x] != x) {
            uf[(size_t)x] = uf[(size_t)uf[(size_t)x]];
            x = uf[(size_t)x];
        }
        return x;
    };
    auto join = [&](int32_t a, int32_t b) {
        a = find(a); b = find(b);
        if (a != b) uf[(size_t)a] = b;
    };
    for (const auto& e : E_.edges)
        if (e.u >= 0 && e.w >= 0 && (size_t)e.u < NP && (size_t)e.w < NP) join(e.u, e.w);
    std::vector<int32_t> root(NP, -1);
    std::vector<double> score(NP, 1e300);
    for (size_t p = 0; p < NP; ++p) {
        const int32_t c = find((int32_t)p);
        const double sc = anchored[p] ? 1.0 + card[p] * 1e-9
                                      : (card[p] <= (double)kClassMax ? card[p] : 1e300);
        if (sc < score[(size_t)c]) {
            score[(size_t)c] = sc;
            root[(size_t)c] = (int32_t)p;
        }
    }
    {
        const MS mr{"rootscan", mcs_now(), mprof};
        for (size_t p = 0; p < NP; ++p)
            if (uf[p] == (int32_t)p && root[p] >= 0) scan_root((size_t)root[p]);
    }
    // The walk roots level 0 at the most selective position; when that class was not materialised
    // the root scan would pay the domain again, so the MCS only adds work.
    {
        size_t best = NP;
        for (size_t p = 0; p < NP; ++p) {
            if (E_.pos_lev[p] != 0) continue;
            const double sc = anchored[p] ? card[p] * 1e-6 : card[p];
            const double bs = best == NP ? 1e300 : (anchored[best] ? card[best] * 1e-6 : card[best]);
            if (sc < bs) best = p;
        }
        if (best != NP && !built_[best]) disabled_ = true;
    }
    if (disabled_) return;
    edges_.assign(NE, McsEdge());
    auto propagate = [&]() {
        bool changed = true;
        while (changed && !disabled_) {
            changed = false;
            for (size_t se = 0; se < NE; ++se) {
                const SkeletonEdge& X = E_.edges[se];
                if (X.u < 0 || X.w < 0) continue;
                if (built_[(size_t)X.u] && !built_[(size_t)X.w]) {
                    build_edge((int)se, cnt, accept);
                    changed = true;
                }
            }
        }
    };
    {
        const MS mp{"propagate", mcs_now(), mprof};
        propagate();
    }
    {
        const MS pc{"classes", mcs_now(), mprof};
        // A class is materialised only when the walk reaches it from a built root.  Scanning an
        // unreached class charged the build a full label-class scan (25k-548k vertices on the
        // text-to-Cypher graphs) whose contents the walk then read from the data graph anyway;
        // V3_MCS_CLASS_SCAN_ON restores the old full build for an ablation.
        if (std::getenv("V3_MCS_CLASS_SCAN_ON"))
            for (size_t p = 0; p < NP && !disabled_; ++p)
                if (!built_[p] && card[p] <= (double)kClassMax) { scan_root(p); propagate(); }
    }
    {
        const MS pe{"edges", mcs_now(), mprof};
        for (size_t se = 0; se < NE && !disabled_; ++se) {
            const SkeletonEdge& X = E_.edges[se];
            if (X.u >= 0 && X.w >= 0 && built_[(size_t)X.u]) build_edge((int)se, cnt, accept);
        }
    }
    // A variable-length edge only materialises its one-hop targets; full_ stays 0 for that
    // class, so the walk tests the data graph at depth >= 2 instead of using the one-hop mask
    // (scanning the whole class here would charge the target scan to B_S and abort the plan).
    {
        const MS pr{"refine", mcs_now(), mprof};
        refine(cnt);
    }
    {
        const MS pd{"depth", mcs_now(), mprof};
        if (!std::getenv("V3_MCS_DEPTH_OFF"))
            for (size_t se = 0; se < NE; ++se) depth_bound((int)se);
    }
    {
        const MS pf{"finalize", mcs_now(), mprof};
        finalize();
    }
}

void Mcs::finalize() {
    if (finalized_) return;
    sorted_.assign(pos_.size(), {});
    for (size_t p = 0; p < pos_.size(); ++p) {
        auto& sv = sorted_[p];
        sv.reserve(pos_[p].verts.size());
        for (uint32_t i = 0; i < pos_[p].verts.size(); ++i)
            sv.emplace_back(pos_[p].verts[i], i);
        std::sort(sv.begin(), sv.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        bytes_ += (uint64_t)sv.size() * (sizeof(uint32_t) * 2);
        // The hash index is only needed while the layers are being built.  Dropping it keeps
        // the round-local structure small and makes every walk lookup a binary search.
        index_[p].clear();
        index_[p].rehash(0);
    }
    finalized_ = true;
}

void Mcs::build_edge(int se, Counters* cnt, const Accept& accept) {
    if (disabled_) return;
    const SkeletonEdge& X = E_.edges[(size_t)se];
    if (X.u < 0 || X.w < 0 || !built_[(size_t)X.u]) return;
    const size_t up = (size_t)X.u, wp = (size_t)X.w;
    const int32_t lw = E_.pos_lev[wp];
    const int32_t level = X.level;
    const Level& L = E_.levels[(size_t)level];
    const bool wbuilt = built_[wp];
    std::vector<uint64_t> tmask(g_.schema().types.size(), 0ULL);
    std::vector<uint64_t> fwd(g_.schema().types.size(), 0ULL);
    std::vector<uint64_t> rev(g_.schema().types.size(), 0ULL);
    for (int m = 0; m < K_; ++m) {
        if ((size_t)se >= L.types[(size_t)m].size()) continue;
        const std::vector<std::string>& mt = L.types[(size_t)m][(size_t)se];
        if (mt.empty()) continue;
        const uint64_t bit = 1ULL << m;
        for (const auto& ty : mt) {
            const int32_t t = g_.schema().type_of(ty);
            if (t < 0) continue;
            tmask[(size_t)t] |= bit;
            if (L.any_dir[(size_t)m][(size_t)se]) fwd[(size_t)t] |= bit, rev[(size_t)t] |= bit;
            else if (L.dir[(size_t)m][(size_t)se]) fwd[(size_t)t] |= bit;
            else rev[(size_t)t] |= bit;
        }
    }
    std::vector<std::string> etypes = X.types;
    if (etypes.empty())
        for (const auto& td : g_.schema().types) etypes.push_back(td.name);
    if (!wbuilt) built_[wp] = 1;
    McsEdge& me = edges_[(size_t)se];
    me = McsEdge();
    McsPos& U = pos_[up];
    me.off.assign(U.verts.size() + 1, 0);
    struct Rec { uint64_t m; uint32_t child; uint32_t rel; };
    std::vector<Rec> recs;
    for (uint32_t pi = 0; pi < U.verts.size() && !disabled_; ++pi) {
        me.off[pi] = (uint32_t)me.segs.size();
        const uint32_t pv = U.verts[pi];
        const uint64_t pm = U.masks[pi];
        if (!pm) continue;
        recs.clear();
        for (const auto& ty : etypes) {
            const int32_t t = g_.schema().type_of(ty);
            if (t < 0) continue;
            const AdjView a = g_.typed((uint32_t)t);
            const auto& gr = g_.typed_rels((uint32_t)t);
            for (uint32_t j = (*a.off)[pv]; j < (*a.off)[pv + 1]; ++j) {
                if (++scanned_edges_ > max_scanned_edges_) { disabled_ = true; return; }
                const uint32_t child = (*a.nbr)[j];
                int32_t cp = position((int)wp, child);
                uint64_t cm = 0;
                if (cp >= 0) {
                    cm = pos_[wp].masks[(size_t)cp];
                } else if (!wbuilt || !full_[wp]) {
                    // A class that was only materialised as a one-hop target is not complete:
                    // a fixed edge that also reaches it must be allowed to add its own children.
                    accept(lw, (int32_t)wp, child, &cm);
                    if (cm) {
                        add((int)wp, child, cm, cnt);
                        if (disabled_) return;
                        cp = position((int)wp, child);
                    }
                }
                if (!cm || cp < 0) continue;
                const uint64_t em = tmask[(size_t)t] &
                                    ((*a.dir)[j] ? fwd[(size_t)t] : rev[(size_t)t]);
                const uint64_t m = pm & em & cm;
                if (!m) continue;
                recs.push_back(Rec{m, (uint32_t)cp, gr[j]});
                if (cnt) ++cnt->mcs_edges;
            }
        }
        if (recs.empty()) continue;
        std::sort(recs.begin(), recs.end(), [](const auto& a, const auto& b) {
            return a.m != b.m ? a.m < b.m : a.child < b.child;
        });
        size_t i = 0;
        while (i < recs.size()) {
            size_t j = i + 1;
            while (j < recs.size() && recs[j].m == recs[i].m) ++j;
            McsSeg s;
            s.begin = (uint32_t)me.child.size();
            s.mask = recs[i].m;
            for (size_t k = i; k < j; ++k) {
                me.child.push_back(recs[k].child);
                me.rel.push_back(recs[k].rel);
            }
            s.end = (uint32_t)me.child.size();
            me.segs.push_back(s);
            i = j;
        }
        bytes_ += (uint64_t)(me.segs.size() * sizeof(McsSeg) + me.child.size() * sizeof(uint32_t) +
                             me.rel.size() * sizeof(uint32_t));
    }
    me.off[U.verts.size()] = (uint32_t)me.segs.size();
}

void Mcs::refine(Counters* cnt) {
    for (size_t se = 0; se < edges_.size() && !disabled_; ++se) {
        const SkeletonEdge& X = E_.edges[se];
        if (X.u < 0 || X.w < 0) continue;
        if (!built_[(size_t)X.u] || !built_[(size_t)X.w]) continue;
        if (X.lo != 1 || X.hi != 1) continue;
        // Refinement clears bits from a complete candidate set; a one-hop target subset would
        // wrongly clear a member whose fixed edge reaches a vertex outside that subset.
        if (!full_[(size_t)X.u] || !full_[(size_t)X.w]) continue;
        const int32_t level = X.level;
        const Level& L = E_.levels[(size_t)level];
        const McsEdge& me = edges_[se];
        McsPos& U = pos_[(size_t)X.u];
        McsPos& W = pos_[(size_t)X.w];
        uint64_t optbits = 0;
        for (int m = 0; m < K_; ++m)
            if (L.opt[(size_t)m]) optbits |= 1ULL << m;
        for (uint32_t pi = 0; pi < U.verts.size(); ++pi) {
            const uint64_t pm = U.masks[pi];
            if (!pm) continue;
            uint64_t reach = 0;
            for (uint32_t s = me.off[pi]; s < me.off[pi + 1]; ++s) reach |= me.segs[s].mask;
            U.masks[pi] = (pm & reach) | (pm & optbits);
            if (cnt) ++cnt->mcs_vertices;
        }
        std::vector<uint64_t> in(W.verts.size(), 0ULL);
        for (uint32_t pi = 0; pi < U.verts.size(); ++pi) {
            for (uint32_t s = me.off[pi]; s < me.off[pi + 1]; ++s) {
                const uint64_t mm = me.segs[s].mask & ~optbits;
                if (!mm) continue;
                for (uint32_t j = me.segs[s].begin; j < me.segs[s].end; ++j)
                    in[me.child[j]] |= mm;
            }
        }
        for (uint32_t wi = 0; wi < W.verts.size(); ++wi) {
            const uint64_t wm = W.masks[wi];
            if (!wm) continue;
            W.masks[wi] = (wm & in[wi]) | (wm & optbits);
            if (cnt) ++cnt->mcs_vertices;
        }
    }
}

// Reverse-BFS lower bound from each target candidate of a variable-length edge.  Only selective
// targets are worth the walk (a whole Message class would visit most of the graph); the visit
// count is capped and the layer is simply left empty when the cap is exceeded, which disables
// only the pruning, never the correctness of the expansion.
void Mcs::depth_bound(int se) {
    const SkeletonEdge& X = E_.edges[(size_t)se];
    if (X.u < 0 || X.w < 0) return;
    if (!built_[(size_t)X.u] || !built_[(size_t)X.w]) return;
    if (X.lo != 1 || X.hi < 0 || X.hi > 8 || (X.lo == 1 && X.hi == 1)) return;
    const int32_t level = X.level;
    const Level& L = E_.levels[(size_t)level];
    const McsPos& U = pos_[(size_t)X.u];
    const McsPos& W = pos_[(size_t)X.w];
    if (W.verts.empty() || W.verts.size() > 200) return;
    const size_t visit_max = (size_t)std::max(1.0, std::atof(std::getenv("V3_MCS_DEPTH_VISIT_MAX") ?: "10000"));
    McsEdge& me = edges_[(size_t)se];
    me.dist.assign(W.verts.size(), -1);
    std::vector<uint64_t> pred[2];
    pred[0].assign(g_.schema().types.size(), 0ULL);
    pred[1].assign(g_.schema().types.size(), 0ULL);
    uint64_t optbits = 0;
    for (int m = 0; m < K_; ++m)
        if (L.opt[(size_t)m]) optbits |= 1ULL << m;
    for (int m = 0; m < K_; ++m) {
        if ((size_t)se >= L.types[(size_t)m].size()) continue;
        const uint64_t bit = 1ULL << m;
        for (const auto& ty : L.types[(size_t)m][(size_t)se]) {
            const int32_t t = g_.schema().type_of(ty);
            if (t < 0) continue;
            if (L.any_dir[(size_t)m][(size_t)se]) pred[0][(size_t)t] |= bit, pred[1][(size_t)t] |= bit;
            else if (L.dir[(size_t)m][(size_t)se]) pred[0][(size_t)t] |= bit;
            else pred[1][(size_t)t] |= bit;
        }
    }
    std::unordered_map<uint32_t, int8_t> seen;
    std::vector<uint32_t> q;
    seen.reserve(W.verts.size() * 4 + 64);
    for (uint32_t wi = 0; wi < W.verts.size(); ++wi) {
        const uint64_t wm = W.masks[wi];
        if (!wm) continue;
        seen.emplace(W.verts[wi], 0);
        me.dist[wi] = 0;
        q.push_back(W.verts[wi]);
    }
    if (q.empty()) { me.dist.clear(); return; }
    std::vector<uint32_t> next;
    for (int32_t d = 0; d < X.hi && !q.empty(); ++d) {
        next.clear();
        for (uint32_t v : q) {
            for (const auto& ty : X.types) {
                const int32_t t = g_.schema().type_of(ty);
                if (t < 0) continue;
                const AdjView a = g_.typed((uint32_t)t);
                for (uint32_t j = (*a.off)[v]; j < (*a.off)[v + 1]; ++j) {
                    const uint32_t u = (*a.nbr)[j];
                    if (seen.count(u)) continue;
                    const uint64_t pm = pred[(*a.dir)[j]][(size_t)t];
                    const int32_t upos = position((int)X.u, u);
                    if (upos < 0 || !(U.masks[(size_t)upos] & pm)) continue;
                    seen.emplace(u, (int8_t)(d + 1));
                    next.push_back(u);
                    if (seen.size() > visit_max) { me.dist.clear(); return; }
                    const int32_t wpos = position((int)X.w, u);
                    if (wpos >= 0) me.dist[(size_t)wpos] = (int8_t)(d + 1);
                }
            }
        }
        q.swap(next);
    }
    (void)optbits;
}

}  // namespace v3
