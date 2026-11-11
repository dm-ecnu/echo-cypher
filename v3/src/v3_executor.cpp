// ECHO v3 per-query executor: enumeration of one candidate over the typed graph and the
// projection of its result (RETURN over the matching, with aggregation and DISTINCT / ORDER /
// SKIP / LIMIT).  This is the fallback path (M6) and the reference the masked evaluation equals.
#include "v3_executor.h"

#include <algorithm>
#include <cstring>

#include "v3_exec.h"

namespace v3 {

bool vertex_matches(const GraphStore& g, const PatternVar& pv, uint32_t v) {
    if (!pv.labels.empty()) {
        const uint32_t lab = g.label(v);
        bool ok = false;
        for (const auto& L : pv.labels) {
            const int32_t id = g.schema().label_of(L);
            if (id >= 0 && (uint32_t)id == lab) {
                ok = true;
                break;
            }
        }
        if (!ok) return false;
    }
    for (const auto& p : pv.props) {
        const uint32_t lab = g.label(v);
        const int32_t col = g.label_col(lab, p.first);
        if (col < 0) return false;
        if (!g.vertex_prop_eq(v, lab, (uint32_t)col, p.second)) return false;
    }
    return true;
}

bool var_const_anchored(const Query& q, int32_t var) {
    if (var < 0 || (size_t)var >= q.vars.size()) return false;
    if (q.vars[(size_t)var].kind != PatternVar::VERTEX) return false;
    if (!q.vars[(size_t)var].props.empty()) return true;
    for (const auto& a : q.atoms) {
        if (a.col >= 0 || a.var != var || a.post || a.or_group >= 0 || a.negated) continue;
        if (a.cmp.rhs == Comparison::VAR || a.cmp.op != "=") continue;
        if (a.cmp.consts.size() != 1 || a.cmp.consts[0].is_null) continue;
        return true;
    }
    return false;
}

bool const_id_anchor(const Query& q, int32_t var, int64_t* id) {
    if (var < 0 || (size_t)var >= q.vars.size()) return false;
    const PatternVar& pv = q.vars[(size_t)var];
    if (pv.kind != PatternVar::VERTEX || pv.labels.empty()) return false;
    for (const auto& pr : pv.props) {
        if (pr.first != "id" || pr.second.is_null || pr.second.is_str) continue;
        *id = pr.second.i;
        return true;
    }
    for (const auto& a : q.atoms) {
        if (a.col >= 0 || a.var != var || a.post || a.negated || a.or_group >= 0) continue;
        if (a.prop != "id" || a.cmp.rhs == Comparison::VAR || a.cmp.op != "=") continue;
        if (a.cmp.consts.size() != 1 || a.cmp.consts[0].is_null || a.cmp.consts[0].is_str) continue;
        *id = a.cmp.consts[0].i;
        return true;
    }
    return false;
}

bool id_anchored_vertices(const GraphStore& g, const Query& q, int32_t var,
                          std::vector<uint32_t>* out) {
    out->clear();
    int64_t id = 0;
    if (!const_id_anchor(q, var, &id)) return false;
    const PatternVar& pv = q.vars[(size_t)var];
    bool any_label = false;
    for (const std::string& ln : pv.labels) {
        const int32_t lid = g.schema().label_of(ln);
        if (lid < 0) continue;
        any_label = true;
        uint32_t v = 0;
        if (g.lookup_vertex((uint32_t)lid, id, &v)) out->push_back(v);
    }
    if (!any_label) {
        out->clear();
        return false;
    }
    std::sort(out->begin(), out->end());
    out->erase(std::unique(out->begin(), out->end()), out->end());
    return true;
}

// Collect the single-column equality anchors of one vertex variable.  A property map entry and
// a WHERE equality atom are the same constraint; both are usable as index probes.
static void collect_anchors(const Query& q, int32_t var,
                            std::vector<std::pair<std::string, Value>>* out) {
    out->clear();
    if (var < 0 || (size_t)var >= q.vars.size()) return;
    if (q.vars[(size_t)var].kind != PatternVar::VERTEX) return;
    for (const auto& pr : q.vars[(size_t)var].props) {
        // The id column already has its own index (main33); indexing it again would build a
        // second copy of every vertex id on the large LDBC graphs.
        if (pr.first == "id" || pr.second.is_null) continue;
        out->emplace_back(pr.first, pr.second);
    }
    for (const auto& c : q.clauses) {
        for (int32_t ai : c.where) {
            if (ai < 0 || (size_t)ai >= q.atoms.size()) continue;
            const Atom& a = q.atoms[(size_t)ai];
            if (a.col >= 0 || a.var != var || a.post || a.negated || a.or_group >= 0) continue;
            if (a.prop == "id") continue;
            if (a.cmp.rhs != Comparison::CONST || a.cmp.op != "=") continue;
            if (a.cmp.consts.size() != 1 || a.cmp.consts[0].is_null) continue;
            out->emplace_back(a.prop, a.cmp.consts[0]);
        }
    }
}

void warm_anchors(const GraphStore& g, const Query& q) {
    std::vector<std::pair<std::string, Value>> anchors;
    for (int32_t var = 0; var < (int32_t)q.vars.size(); ++var) {
        collect_anchors(q, var, &anchors);
        if (anchors.empty()) continue;
        for (const auto& L : q.vars[(size_t)var].labels) {
            const int32_t lid = g.schema().label_of(L);
            if (lid < 0) continue;
            for (const auto& a : anchors) {
                const int32_t col = g.label_col((uint32_t)lid, a.first);
                if (col >= 0) g.warm_prop_index((uint32_t)lid, (uint32_t)col);
            }
        }
    }
}

bool anchored_vertices(const GraphStore& g, const Query& q, int32_t var,
                       std::vector<uint32_t>* out) {
    out->clear();
    if (var < 0 || (size_t)var >= q.vars.size()) return false;
    const PatternVar& pv = q.vars[(size_t)var];
    if (pv.kind != PatternVar::VERTEX || pv.labels.empty()) return false;
    std::vector<std::pair<std::string, Value>> anchors;
    collect_anchors(q, var, &anchors);
    if (anchors.empty()) return false;
    bool any_col = false;
    for (const auto& L : pv.labels) {
        const int32_t lid = g.schema().label_of(L);
        if (lid < 0) continue;
        const int32_t col = g.label_col((uint32_t)lid, anchors[0].first);
        if (col < 0) continue;
        any_col = true;
        g.prop_lookup((uint32_t)lid, (uint32_t)col, anchors[0].second, out);
    }
    if (!any_col) return true;   // the anchor column does not exist on any label: no root
    for (size_t ai = 1; ai < anchors.size(); ++ai) {
        std::vector<uint32_t> keep;
        keep.reserve(out->size());
        for (uint32_t v : *out) {
            const uint32_t lab = g.label(v);
            const int32_t col = g.label_col(lab, anchors[ai].first);
            if (col < 0) continue;
            if (g.vertex_prop_eq(v, lab, (uint32_t)col, anchors[ai].second)) keep.push_back(v);
        }
        out->swap(keep);
    }
    std::sort(out->begin(), out->end());
    out->erase(std::unique(out->begin(), out->end()), out->end());
    return true;
}

double var_root_card(const GraphStore& g, const Query& q, int32_t var) {
    if (var < 0 || (size_t)var >= q.vars.size()) return (double)g.num_vertices();
    const PatternVar& pv = q.vars[(size_t)var];
    if (pv.labels.empty()) return (double)g.num_vertices();
    double n = 0.0;
    for (const auto& L : pv.labels) {
        const int32_t id = g.schema().label_of(L);
        if (id >= 0) n += (double)g.vertices_of((uint32_t)id).size();
    }
    return std::max(1.0, n);
}

std::vector<int32_t> const_atoms_of(const Query& q, int32_t var, int32_t clause) {
    std::vector<int32_t> out;
    if (clause < 0 || (size_t)clause >= q.clauses.size()) return out;
    if (var < 0 || (size_t)var >= q.vars.size() ||
        q.vars[(size_t)var].kind != PatternVar::VERTEX) return out;
    for (int32_t a : q.clauses[(size_t)clause].where) {
        if (a < 0 || (size_t)a >= q.atoms.size()) continue;
        const Atom& x = q.atoms[(size_t)a];
        if (x.col >= 0 || x.var != var || x.post || x.or_group >= 0 || x.negated) continue;
        if (x.cmp.rhs == Comparison::VAR || x.cmp.op != "=") continue;
        if (x.cmp.consts.size() != 1 || x.cmp.consts[0].is_null) continue;
        out.push_back(a);
    }
    return out;
}

Value id_value(const GraphStore& g, bool is_edge, uint32_t x) {
    Value v;
    const int64_t id = is_edge ? g.edge_id(x) : g.vertex_id(x);
    if (id == kNull) v.is_null = true;
    v.i = id;
    return v;
}

bool env_value(const GraphStore& g, const Query& q, const Env& env, int32_t var,
               const std::string& prop, Value* out) {
    out->is_null = true;
    out->is_str = false;
    out->i = 0;
    out->s.clear();
    if (var < 0 || var >= (int32_t)env.v.size() || env.v[var] == kNoVal) return true;
    const PatternVar& pv = q.vars[var];
    const uint32_t x = env.v[var];
    if (pv.kind == PatternVar::EDGE) {
        if (prop.empty()) { *out = id_value(g, true, x); return true; }
        const uint32_t t = g.edge_type(x);
        const int32_t col = g.type_col(t, prop);
        if (col < 0) return true;
        bool is_null = false;
        *out = g.edge_prop(x, t, (uint32_t)col, &is_null);
        if (is_null) out->is_null = true;
        return true;
    }
    const uint32_t lab = g.label(x);
    if (prop.empty()) { *out = id_value(g, false, x); return true; }
    const int32_t col = g.label_col(lab, prop);
    if (col < 0) return true;
    bool is_null = false;
    *out = g.vertex_prop(x, lab, (uint32_t)col, &is_null);
    if (is_null) out->is_null = true;
    return true;
}

bool QueryExec::atom_evaluable(const Query& q, const Env& env, const Atom& a, const Row* cols) {
    (void)q;
    auto bound = [&](int32_t v) { return v >= 0 && v < (int32_t)env.v.size() && env.v[v] != kNoVal; };
    if (a.col >= 0) return cols && (size_t)a.col < cols->v.size();
    if (!bound(a.var)) return false;
    if (a.cmp.rhs == Comparison::VAR && !bound(a.cmp.var)) return false;
    return true;
}

int QueryExec::eval_atom(const GraphStore& g, const Query& q, const Env& env, const Atom& a,
                         const Row* cols) {
    if (a.cmp.op == "pattern_edge_type") return -1;   // handled by the caller (edge scoping)
    if (a.cmp.op == "LABEL" || a.cmp.op == "LABEL_ANY") {
        if (a.var < 0 || (size_t)a.var >= env.v.size() || env.v[(size_t)a.var] == kNoVal) return -1;
        const uint32_t lid = g.label(env.v[(size_t)a.var]);
        bool res = false;
        for (const auto& c : a.cmp.consts) {
            if (!c.is_str) continue;
            const int32_t id = g.schema().label_of(c.s);
            if (id >= 0 && (uint32_t)id == lid) { res = true; break; }
        }
        if (a.negated) res = !res;
        return res ? 1 : 0;
    }
    Value l;
    if (a.col >= 0) {
        if (!cols || (size_t)a.col >= cols->v.size()) return -1;
        l = cols->v[(size_t)a.col];
    } else {
        env_value(g, q, env, a.var, a.prop, &l);
    }
    if (a.cmp.op == "IS NULL") return (l.is_null ? 1 : 0) ^ (a.negated ? 1 : 0);
    if (a.cmp.op == "IS NOT NULL") return (l.is_null ? 0 : 1) ^ (a.negated ? 1 : 0);
    if (l.is_null) return -1;
    bool res = false;
    if (a.cmp.rhs == Comparison::VAR) {
        Value r;
        if (a.prop.empty()) {
            // entity comparison: identity (the vertices' / relationships' ids)
            if (env.v[a.cmp.var] == kNoVal) return -1;
            r = id_value(g, q.vars[a.cmp.var].kind == PatternVar::EDGE, env.v[a.cmp.var]);
        } else {
            env_value(g, q, env, a.cmp.var, a.rhs_prop, &r);
        }
        if (r.is_null) return -1;
        Value lv = a.prop.empty()
                       ? id_value(g, q.vars[a.var].kind == PatternVar::EDGE, env.v[a.var])
                       : l;
        res = cmp_values(lv, r, a.cmp.op);
    } else if (a.cmp.op == "IN") {
        for (const auto& c : a.cmp.consts)
            if (!c.is_null && cmp_values(l, c, "=")) {
                res = true;
                break;
            }
    } else {
        if (a.cmp.consts.empty()) return -1;
        const Value& c = a.cmp.consts[0];
        if (c.is_null) return -1;
        res = cmp_values(l, c, a.cmp.op);
    }
    if (a.negated) res = !res;
    return res ? 1 : 0;
}

uint32_t result_columns(const Query& q) {
    const Clause& ret = q.clauses[q.return_clause];
    uint32_t n = 0;
    for (int32_t it : ret.items) {
        const ProjectionItem& pi = q.items[it];
        n += pi.kind == ProjectionItem::AGG && pi.func == AggFunc::COLLECT && !pi.distinct ? 1u : 1u;
    }
    return n;
}

}  // namespace v3
