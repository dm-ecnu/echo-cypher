// ECHO v3 reference evaluation on the shared base (see v3_base.h).
//
// The reference of one candidate is the base's own per-query pipeline on its first MATCH: the
// base's filter-vertex candidate lists (the DPiso filter refines at most three rounds and keeps
// the nodes that survive, so any graph whose first-round lists are the pattern's structural
// candidates has the same lists), the base's GQL order and the base's LFTJ enumerator, which
// walks those lists and tests every pattern edge in the edge tables built from them.  The graph
// view is the one of the v2 export:
//
//   v = LSHIFT + 2^(d(e)-2) for a level-1 connector bound to one endpoint of pattern edge e,
//   v + n, v + 2n, ... one copy per vertex, one level per link along a pattern path,
//
// so the relationship (s) -[:T]-> (d) of edge e is the chain s[0] - o[0] - i[0] - d[0] at level 1,
// and the other edges of the same bushy vertex move from d to the next source at level 2
// (d[1] - o'[1] - i'[1] - s'[1], then s'[2] ...).  A node's first-round candidates (its label,
// degree and typed-neighbour labels) make the filter keep exactly the nodes that repeat
// correctly; the candidate's atoms between two pattern variables are structural candidates as
// well (a node keeps a position when some binding satisfies the atom).  Matching counts are
// bucketed by the chain (a vertex covered by several level-1 copies is only listed once: in
// enumeration order every row 1 expresses exactly two buses, so a bushy vertex is always the
// root of its chain).
//
// Supported shapes: connected, acyclic patterns over single-labelled vertices with single-hop,
// single-type relationships, no edge property or edge variable in the pattern.  Anything else is
// reported unsupported (and counted by the caller).
#include "v3_base.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

#include "echo_dpiso.h"
#include "graph/graph.h"

#include "v3_exec.h"

namespace v3 {

namespace {

constexpr int64_t kShift = 13;
constexpr int64_t kUnit = 1LL << kShift;
constexpr int64_t kMaxT = (1LL << (kShift - 1)) - 2;   // the encoding keeps types below 8190

bool column_value(const GraphStore& g, const Query& q, int32_t var, const std::string& prop, uint32_t x, Value* out) {
    const PatternVar& pv = q.vars[var];
    out->is_null = true;
    out->is_str = false;
    out->i = 0;
    out->s.clear();
    if (x == UINT32_MAX) return true;
    if (pv.kind == PatternVar::EDGE) {
        if (x < g.num_vertices()) return true;
        const uint32_t rel = x - g.num_vertices();
        if (prop.empty()) {
            *out = Value::num(g.edge_id(rel));
            if (out->i == kNull) out->is_null = true;
            return true;
        }
        const int32_t col = g.type_col(g.edge_type(rel), prop);
        if (col < 0) return true;
        bool n2 = false;
        *out = g.edge_prop(rel, g.edge_type(rel), (uint32_t)col, &n2);
        if (n2) out->is_null = true;
        return true;
    }
    if (x >= g.num_vertices()) return true;
    if (prop.empty()) {
        *out = Value::num(g.vertex_id(x));
        if (out->i == kNull) out->is_null = true;
        return true;
    }
    const int32_t col = g.label_col(g.label(x), prop);
    if (col < 0) return true;
    bool n2 = false;
    *out = g.vertex_prop(x, g.label(x), (uint32_t)col, &n2);
    if (n2) out->is_null = true;
    return true;
}

int atom_eval(const GraphStore& g, const Query& q, const Atom& a, const std::vector<uint32_t>& vx, const std::vector<char>& vnull) {
    const uint32_t lx = vx[a.var];
    const bool ln = vnull[a.var];
    if (a.cmp.op == "IS NULL") return (ln ? 1 : 0) ^ (a.negated ? 1 : 0);
    if (a.cmp.op == "IS NOT NULL") return (!ln ? 1 : 0) ^ (a.negated ? 1 : 0);
    if (ln) return -1;
    Value l;
    column_value(g, q, a.var, a.cmp.rhs == Comparison::CONST && a.prop.empty() ? "" : a.prop, lx, &l);
    if (l.is_null) return -1;
    bool res = false;
    if (a.cmp.rhs == Comparison::VAR) {
        if (vnull[a.cmp.var]) return -1;
        Value r;
        column_value(g, q, a.cmp.var, a.prop.empty() ? "" : a.rhs_prop, vx[a.cmp.var], &r);
        if (r.is_null) return -1;
        res = cmp_values(l, r, a.cmp.op);
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

bool passes(const GraphStore& g, const Query& q, const std::vector<int32_t>& atoms, const std::vector<uint32_t>& vx,
            const std::vector<char>& vnull) {
    size_t k = 0;
    while (k < atoms.size()) {
        const Atom& a = q.atoms[atoms[k]];
        if (a.or_group < 0) {
            if (atom_eval(g, q, a, vx, vnull) != 1) return false;
            ++k;
            continue;
        }
        bool any = false;
        while (k < atoms.size() && q.atoms[atoms[k]].or_group == a.or_group) {
            if (atom_eval(g, q, q.atoms[atoms[k]], vx, vnull) == 1) any = true;
            ++k;
        }
        if (!any) return false;
    }
    return true;
}

bool acyclic(const Query& q, const std::vector<int32_t>& edges, std::vector<char>* used) {
    std::vector<std::vector<int32_t>> adj(q.vars.size());
    used->assign(q.vars.size(), 0);
    for (int32_t e : edges) {
        adj[q.edges[e].from].push_back(q.edges[e].to);
        adj[q.edges[e].to].push_back(q.edges[e].from);
        (*used)[q.edges[e].from] = (*used)[q.edges[e].to] = 1;
    }
    std::vector<int> color(q.vars.size(), 0);
    std::function<bool(int32_t, int32_t)> dfs = [&](int32_t u, int32_t parent) {
        color[u] = 1;
        for (int32_t w : adj[u]) {
            if (w == parent) continue;
            if (color[w] == 1) return false;
            if (color[w] == 0 && !dfs(w, u)) return false;
        }
        color[u] = 2;
        return true;
    };
    for (size_t v = 0; v < q.vars.size(); ++v)
        if ((*used)[v] && !color[v] && !dfs((int32_t)v, -1)) return false;
    return true;
}

}  // namespace

BaseQuery::Result BaseQuery::run(const GraphStore& g, const std::string&, const Query& q) {
    Result r;
    if (q.first_match < 0 || q.return_clause < 0) {
        r.why = "no_match";
        return r;
    }
    const std::vector<int32_t> edges = q.clauses[q.first_match].edges;
    if (edges.empty()) {
        r.why = "no_edges";
        return r;
    }
    for (size_t c = 0; c < q.clauses.size(); ++c) {
        if ((int32_t)c == q.first_match) continue;
        const ClauseKind k = q.clauses[c].kind;
        if ((k == ClauseKind::MATCH || k == ClauseKind::OPTIONAL_MATCH) && !q.clauses[c].edges.empty()) {
            r.why = "second_match";
            return r;
        }
        if (k == ClauseKind::WITH) {
            r.why = "with";
            return r;
        }
    }
    for (size_t a = 0; a < edges.size(); ++a) {
        const PatternEdge& pe = q.edges[edges[a]];
        if (pe.lo != 1 || pe.hi != 1) {
            r.why = "variable_length";
            return r;
        }
        if (!pe.var_name.empty() && q.var_index(pe.var_name) >= 0) {
            r.why = "edge_variable";
            return r;
        }
        if (pe.any_dir) {
            r.why = "undirected";
            return r;
        }
        if (!pe.props.empty()) {
            r.why = "edge_property";
            return r;
        }
        if (pe.types.size() != 1) {
            r.why = "type_set";
            return r;
        }
        for (size_t b = a + 1; b < edges.size(); ++b) {
            const PatternEdge& qe = q.edges[edges[b]];
            if ((qe.from == pe.from && qe.to == pe.to) || (qe.from == pe.to && qe.to == pe.from)) {
                r.why = "parallel_edges";
                return r;
            }
        }
    }
    std::vector<char> used;
    if (!acyclic(q, edges, &used)) {
        r.why = "cycle";
        return r;
    }
    for (size_t v = 0; v < q.vars.size(); ++v) {
        if (q.vars[v].kind != PatternVar::VERTEX) continue;
        if (!used[v]) {
            r.why = "variable_outside_pattern";
            return r;
        }
        if (q.vars[v].labels.size() != 1) {
            r.why = "label_set";
            return r;
        }
        if (!q.vars[v].props.empty()) {
            r.why = "vertex_map";
            return r;
        }
    }
    r.why = "todo";    return r;
}

}  // namespace v3
