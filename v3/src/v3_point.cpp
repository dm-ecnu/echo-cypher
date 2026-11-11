#include "v3_point.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "v3_graph.h"
#include "v3_executor.h"

namespace v3 {
namespace {

double now_sec() {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

int vertex_count(const Query& q) {
    int n = 0;
    for (const auto& v : q.vars)
        if (v.kind == PatternVar::VERTEX) ++n;
    return n;
}

bool has_id_prop(const PatternVar& v, int64_t* id) {
    for (const auto& p : v.props) {
        if (p.first != "id" || p.second.is_str || p.second.is_null) return false;
        *id = p.second.i;
    }
    return true;
}

bool id_atom(const Query& q, int var, int64_t* id) {
    for (const Atom& a : q.atoms) {
        if (a.var != var || a.prop != "id" || a.cmp.op != "=" ||
            a.cmp.rhs != Comparison::CONST || a.cmp.consts.size() != 1)
            continue;
        const Value& v = a.cmp.consts[0];
        if (v.is_str || v.is_null) continue;
        *id = v.i;
        return true;
    }
    return false;
}

bool labels_of(const Query& q, const Schema& s, int var, std::vector<int>* out) {
    if (var < 0 || (size_t)var >= q.vars.size()) return false;
    const PatternVar& pv = q.vars[(size_t)var];
    if (pv.labels.empty()) return false;
    out->clear();
    for (const auto& name : pv.labels) {
        const int32_t l = s.label_of(name);
        if (l < 0) return false;
        out->push_back(l);
    }
    std::sort(out->begin(), out->end());
    out->erase(std::unique(out->begin(), out->end()), out->end());
    return true;
}

bool compile_query(const Query& q, const Schema& s, PointQuery* out) {
    // Only one constant-anchored vertex or one fixed typed hop.  Everything else (multi-hop,
    // variable length, OPTIONAL/WITH, WHERE atoms or edge constants) keeps the general path.
    if (!q.is_plain() || q.edges.size() > 1) return false;
    if (q.first_match < 0 || q.return_clause < 0) return false;
    for (const auto& c : q.clauses)
        if (!c.pre_where.empty()) return false;

    bool direct = false;
    if (q.return_clause >= 0 && (size_t)q.return_clause < q.clauses.size()) {
        const Clause& ret = q.clauses[(size_t)q.return_clause];
        direct = !ret.distinct && ret.pre_where.empty() && q.skip < 0 && q.limit < 0 &&
                 !ret.items.empty();
        for (int32_t it : ret.items) {
            if (it < 0 || (size_t)it >= q.items.size()) { direct = false; break; }
            const ProjectionItem::Kind kind = q.items[(size_t)it].kind;
            if (kind != ProjectionItem::KEY && kind != ProjectionItem::COL) {
                direct = false;
                break;
            }
        }
    }
    out->direct_project = direct;

    if (q.edges.empty()) {
        if (vertex_count(q) != 1) return false;
        int v = -1;
        for (size_t i = 0; i < q.vars.size(); ++i)
            if (q.vars[i].kind == PatternVar::VERTEX) v = (int)i;
        int64_t id = 0;
        bool found = false;
        for (const auto& p : q.vars[(size_t)v].props)
            if (p.first == "id" && !p.second.is_str && !p.second.is_null) { found = true; id = p.second.i; }
        if (!found && !id_atom(q, v, &id)) return false;
        // The only atom this path may consume is the anchor equality itself.
        if (q.atoms.size() > (found ? 0u : 1u)) return false;
        out->root_id = id;
        out->edge = false;
        out->root = v;
        if (!labels_of(q, s, v, &out->root_labels)) return false;
        return true;
    }

    if (vertex_count(q) != 2 || !q.atoms.empty()) return false;
    for (const auto& c : q.clauses)
        if (!c.where.empty()) return false;
    const PatternEdge& e = q.edges[0];
    if (e.lo != 1 || e.hi != 1 || e.types.size() != 1) return false;
    if (!e.props.empty() || e.path_nodes || !e.path.empty() || !e.scoped.empty()) return false;
    const int32_t type = s.type_of(e.types[0]);
    if (type < 0) return false;
    if (e.from < 0 || e.to < 0 || e.from == e.to) return false;
    if (q.vars[(size_t)e.from].kind != PatternVar::VERTEX ||
        q.vars[(size_t)e.to].kind != PatternVar::VERTEX)
        return false;

    const bool from_has = !q.vars[(size_t)e.from].props.empty();
    const bool to_has = !q.vars[(size_t)e.to].props.empty();
    if (from_has == to_has) return false;   // exactly one correctly anchored end
    const bool root_from = from_has;
    const int root = root_from ? e.from : e.to;
    const int other = root_from ? e.to : e.from;

    int64_t rid = 0;
    if (!has_id_prop(q.vars[(size_t)root], &rid)) return false;
    bool root_has_id = false;
    for (const auto& p : q.vars[(size_t)root].props)
        if (p.first == "id" && !p.second.is_str && !p.second.is_null) root_has_id = true;
    if (!root_has_id) return false;
    if (!labels_of(q, s, root, &out->root_labels)) return false;
    if (!labels_of(q, s, other, &out->other_labels)) return false;

    int64_t oid = 0;
    bool other_has_id = false;
    for (const auto& p : q.vars[(size_t)other].props) {
        if (p.first != "id" || p.second.is_str || p.second.is_null) return false;
        other_has_id = true;
        oid = p.second.i;
    }

    out->edge = true;
    out->root = root;
    out->other = other;
    out->edge_var = e.var_name.empty() ? -1 : q.var_index(e.var_name);
    out->type = type;
    out->any_dir = e.any_dir;
    out->root_from = root_from;
    out->root_id = rid;
    out->other_has_id = other_has_id;
    out->other_id = oid;
    return true;
}

bool same_group(const PointQuery& a, const PointQuery& b) {
    return a.type == b.type && a.any_dir == b.any_dir && a.root_from == b.root_from &&
           a.root_labels == b.root_labels && a.other_labels == b.other_labels;
}

void project_direct(const GraphStore& g, const Query& q, const std::vector<Env>& envs,
                    CandidateResult* res) {
    const Clause& ret = q.clauses[(size_t)q.return_clause];
    res->ncols = (uint32_t)ret.items.size();
    res->labels.clear();
    res->data.clear();
    res->data.reserve(envs.size() * ret.items.size());
    std::vector<Row> rows;
    rows.reserve(envs.size());
    for (const Env& e : envs) {
        Row r;
        r.v.reserve(ret.items.size());
        for (int32_t it : ret.items) {
            const ProjectionItem& pi = q.items[(size_t)it];
            if (pi.kind == ProjectionItem::COL) {
                // A RETURN-level COL in the frozen builder is an alias slot with no carried
                // WITH column; the reference projection reads it as NULL.  Keep that value so
                // the result multiset matches the per-query executor.
                r.v.push_back(Value::nul());
                continue;
            }
            Value v;
            if (!env_value(g, q, e, pi.var, pi.prop, &v)) v = Value::nul();
            r.v.push_back(std::move(v));
        }
        rows.push_back(std::move(r));
    }
    if (q.order_by >= 0 && (size_t)q.order_by < q.clauses.size()) {
        const Clause& ob = q.clauses[(size_t)q.order_by];
        std::stable_sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
            for (size_t k = 0; k < ob.order_items.size(); ++k) {
                const size_t col = (size_t)ob.order_items[k];
                if (col >= a.v.size()) continue;
                const Value& x = a.v[col];
                const Value& y = b.v[col];
                if (x == y) continue;
                const bool less = x < y;
                return k < ob.order_desc.size() && ob.order_desc[k] ? !less : less;
            }
            return false;
        });
    }
    for (const auto& r : rows)
        for (const auto& v : r.v) res->data.push_back(v);
}

}  // namespace

bool prepare_point_round(const GraphStore& g, Round* r) {
    const size_t k = r->queries.size();
    r->point_ready = false;
    r->point.assign(k, PointQuery());
    r->point_uniq.clear();
    r->point_replay.assign(k, -1);
    if (k == 0) return false;
    for (size_t i = 0; i < k; ++i) {
        if (!compile_query(r->queries[i], g.schema(), &r->point[i])) return false;
    }
    // Compile-time dedup: identical canonical candidates replay the first one's result.
    for (auto& q : r->queries)
        if (q.canon.empty()) q.canon = canonical_key(q);
    std::unordered_map<std::string_view, int32_t> first_of;
    for (size_t i = 0; i < k; ++i) {
        std::string_view key = r->queries[i].canon.empty()
                                   ? std::string_view(r->queries[i].text)
                                   : std::string_view(r->queries[i].canon);
        auto it = first_of.find(key);
        if (it == first_of.end()) {
            first_of.emplace(key, (int32_t)i);
            r->point_uniq.push_back((int32_t)i);
        } else {
            r->point_replay[i] = it->second;
        }
    }
    r->point_ready = true;
    return true;
}

bool run_point_round(const GraphStore& g, const Round& r, std::vector<CandidateOut>* out,
                     Counters* cnt, RoundPlan* plan, double* plan_sec, double* exec_sec,
                     std::string* err) {
    if (!r.point_ready) return false;
    const double t_start = now_sec();
    const size_t k = r.queries.size();
    const std::vector<PointQuery>& pq = r.point;
    const std::vector<int32_t>& uniq = r.point_uniq;
    const std::vector<int32_t>& replay = r.point_replay;

    if (out->size() != k) { out->clear(); out->resize(k); }
    plan->envelopes.clear();
    plan->prefix_envelopes.clear();
    plan->replay = replay;
    plan->fallback = uniq;
    plan->why.clear();
    if (plan_sec) *plan_sec = 0.0;

    std::vector<std::vector<Env>> envs(k);
    // Per-label id cache: one binary search per (label, constant), shared by all candidates
    // that use the same constant.  The work ledger still charges one lookup per unique
    // candidate, as the per-query executor did; the cache only removes the repeated search.
    struct RootEnt {
        int label;
        int64_t id;
        std::vector<uint32_t> found;
    };
    std::vector<RootEnt> root_cache;
    root_cache.reserve(k);
    auto roots_of = [&](const PointQuery& x) {
        std::vector<uint32_t> roots;
        for (int label : x.root_labels) {
            const RootEnt* hit = nullptr;
            for (const auto& r : root_cache)
                if (r.label == label && r.id == x.root_id) { hit = &r; break; }
            if (!hit) {
                RootEnt e;
                e.label = label;
                e.id = x.root_id;
                uint32_t v = 0;
                if (g.lookup_vertex((uint32_t)label, x.root_id, &v)) e.found.push_back(v);
                root_cache.push_back(std::move(e));
                hit = &root_cache.back();
            }
            roots.insert(roots.end(), hit->found.begin(), hit->found.end());
        }
        if (roots.size() > 1) {
            std::sort(roots.begin(), roots.end());
            roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
        }
        return roots;
    };

    // Vertex-only candidates: the lookup is the whole pattern.
    for (int32_t ci : uniq) {
        const PointQuery& x = pq[(size_t)ci];
        if (x.edge) continue;
        const std::vector<uint32_t> roots = roots_of(x);
        if (roots.empty()) {
            if (cnt) ++cnt->init_tuples;
        } else if (cnt) {
            cnt->init_tuples += roots.size();
        }
        for (uint32_t v : roots) {
            Env e;
            e.v.assign(r.queries[(size_t)ci].vars.size(), kNoVal);
            e.v[(size_t)x.root] = v;
            envs[(size_t)ci].push_back(std::move(e));
        }
        if (cnt) cnt->solutions += roots.size();
    }

    // Single-hop candidates: group by type, direction and endpoint labels.  One adjacency scan
    // per distinct root vertex serves every candidate in the group; the candidate's constant
    // decides which of the scanned neighbours become its matchings.  This is the small-round
    // form of the masked dispatch.
    std::vector<std::vector<int32_t>> groups;
    for (int32_t ci : uniq) {
        if (!pq[(size_t)ci].edge) continue;
        bool placed = false;
        for (auto& gr : groups) {
            if (same_group(pq[(size_t)gr[0]], pq[(size_t)ci])) {
                gr.push_back(ci);
                placed = true;
                break;
            }
        }
        if (!placed) groups.push_back({ci});
    }

    for (auto& gr : groups) {
        const PointQuery& model = pq[(size_t)gr[0]];
        const AdjView av = g.typed((uint32_t)model.type);
        std::vector<std::pair<uint32_t, std::vector<int32_t>>> by_root;
        for (int32_t ci : gr) {
            const PointQuery& x = pq[(size_t)ci];
            const std::vector<uint32_t> roots = roots_of(x);
            if (roots.empty()) {
                if (cnt) ++cnt->init_tuples;
            } else if (cnt) {
                cnt->init_tuples += roots.size();
            }
            for (uint32_t v : roots) {
                std::vector<int32_t>* bucket = nullptr;
                for (auto& kv : by_root)
                    if (kv.first == v) { bucket = &kv.second; break; }
                if (!bucket) {
                    by_root.emplace_back(v, std::vector<int32_t>());
                    bucket = &by_root.back().second;
                }
                bucket->push_back(ci);
            }
        }
        for (const auto& kv : by_root) {
            const uint32_t root = kv.first;
            const std::vector<int32_t>& cand = kv.second;
            const uint8_t want = model.root_from ? 1 : 0;
            const uint32_t b = (*av.off)[root], e = (*av.off)[root + 1];
            for (uint32_t j = b; j < e; ++j) {
                const uint32_t rel = g.typed_rel((uint32_t)model.type, root, j - b);
                if (!model.any_dir && g.edge_dir(rel) != want) continue;
                const uint32_t nb = (*av.nbr)[j];
                if (!std::binary_search(model.other_labels.begin(), model.other_labels.end(),
                                        (int)g.label(nb)))
                    continue;
                if (cnt) ++cnt->edges_expanded;
                for (int32_t ci : cand) {
                    const PointQuery& x = pq[(size_t)ci];
                    if (x.other_has_id && (int64_t)nb != x.other_id) continue;
                    Env row;
                    row.v.assign(r.queries[(size_t)ci].vars.size(), kNoVal);
                    row.v[(size_t)x.root] = root;
                    row.v[(size_t)x.other] = nb;
                    if (x.edge_var >= 0) row.v[(size_t)x.edge_var] = rel;
                    envs[(size_t)ci].push_back(std::move(row));
                }
            }
        }
    }
    for (int32_t ci : uniq) {
        if (cnt) {
            cnt->tuples += envs[(size_t)ci].size();
            cnt->solutions += envs[(size_t)ci].size();
        }
    }

    for (int32_t ci : uniq) {
        const Query& q = r.queries[(size_t)ci];
        if (envs[(size_t)ci].empty()) {
            // v3_batch consumes ncols/data only; column names are checked by v3_check, which
            // runs the generic per-query path.  Avoid eight small label vectors per cell.
            const Clause& ret = q.clauses[(size_t)q.return_clause];
            (*out)[(size_t)ci].ncols = (uint32_t)ret.items.size();
            (*out)[(size_t)ci].planned = false;
            continue;
        }
        CandidateResult res;
        if (pq[(size_t)ci].direct_project) {
            project_direct(g, q, envs[(size_t)ci], &res);
        } else {
            Counters pc;
            QueryExec::project_only(g, q, envs[(size_t)ci], &res, true, true, true, -1, &pc);
            if (cnt) cnt->add(pc);
        }
        (*out)[(size_t)ci].ncols = res.ncols;
        (*out)[(size_t)ci].data = std::move(res.data);
        (*out)[(size_t)ci].labels = std::move(res.labels);
        (*out)[(size_t)ci].planned = false;
    }
    for (size_t i = 0; i < k; ++i) {
        if (plan->replay[i] < 0) continue;
        (*out)[i] = (*out)[(size_t)plan->replay[i]];
        if (cnt) cnt->solutions += envs[(size_t)plan->replay[i]].size();
    }
    if (exec_sec) *exec_sec = now_sec() - t_start;
    if (err) err->clear();
    return true;
}

}  // namespace v3
