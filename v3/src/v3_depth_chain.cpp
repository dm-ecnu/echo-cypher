#include "v3_depth_chain.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <numeric>
#include <set>
#include <string>
#include <vector>

#include "v3_graph.h"

namespace v3 {
namespace {

struct Chain {
    int32_t len = 0;
    std::string type;
    std::vector<int32_t> verts;   // query variable ids in walk order
    std::vector<int32_t> edges;   // query edge ids in walk order
    std::vector<std::string> names;
};

// One candidate: a single MATCH-level directed path, all hops fixed to one, one label and one
// relationship type on every position, and count(*) as the only projection.  The pattern is
// walked from its unique source so the order is the direction of the data edges.
bool chain_of(const Query& q, Chain* out) {
    if (q.sem != Semantics::WALK) return false;
    if (q.first_match < 0 || (size_t)q.first_match >= q.clauses.size()) return false;
    if (q.clauses[(size_t)q.first_match].kind != ClauseKind::MATCH) return false;
    if (!q.atoms.empty()) return false;
    if (q.order_by >= 0 || q.skip >= 0 || q.limit >= 0) return false;
    if (q.return_clause < 0 || (size_t)q.return_clause >= q.clauses.size()) return false;
    if (q.items.size() != 1) return false;
    const ProjectionItem& it = q.items[0];
    if (it.kind != ProjectionItem::AGG || it.func != AggFunc::COUNT_STAR || it.distinct)
        return false;

    int nret = 0, nwith = 0;
    for (size_t c = 0; c < q.clauses.size(); ++c) {
        const Clause& cl = q.clauses[c];
        if (cl.kind == ClauseKind::RETURN) ++nret;
        if (cl.kind == ClauseKind::WITH) ++nwith;
        if (cl.kind == ClauseKind::OPTIONAL_MATCH) return false;
        if (cl.kind == ClauseKind::MATCH && (int32_t)c != q.first_match) return false;
        if (!cl.where.empty() || !cl.pre_where.empty()) return false;
    }
    if (nret != 1 || nwith != 0) return false;
    if (q.edges.empty() || q.edges.size() > 63) return false;

    std::vector<int32_t> deg(q.vars.size(), 0), indeg(q.vars.size(), 0), outdeg(q.vars.size(), 0);
    std::string type;
    for (const PatternEdge& e : q.edges) {
        if (e.clause != q.first_match) return false;
        if (e.lo != 1 || e.hi != 1 || e.any_dir) return false;
        if (!e.props.empty() || e.types.size() != 1) return false;
        if (!e.path.empty() || !e.scoped.empty()) return false;
        if (e.from < 0 || e.to < 0 || e.from == e.to) return false;
        if ((size_t)e.from >= q.vars.size() || (size_t)e.to >= q.vars.size()) return false;
        if (type.empty()) type = e.types[0];
        else if (type != e.types[0]) return false;
        ++deg[(size_t)e.from];
        ++deg[(size_t)e.to];
        ++outdeg[(size_t)e.from];
        ++indeg[(size_t)e.to];
    }

    std::vector<int32_t> verts;
    for (size_t v = 0; v < q.vars.size(); ++v)
        if (deg[v] > 0) verts.push_back((int32_t)v);
    if (verts.size() != q.edges.size() + 1) return false;

    int32_t start = -1, end = -1;
    for (int32_t v : verts) {
        if (q.vars[(size_t)v].kind != PatternVar::VERTEX) return false;
        if (!q.vars[(size_t)v].props.empty()) return false;
        if (deg[(size_t)v] > 2) return false;
        if (indeg[(size_t)v] == 0 && outdeg[(size_t)v] == 1) {
            if (start >= 0) return false;
            start = v;
        } else if (outdeg[(size_t)v] == 0 && indeg[(size_t)v] == 1) {
            if (end >= 0) return false;
            end = v;
        } else if (!(indeg[(size_t)v] == 1 && outdeg[(size_t)v] == 1)) {
            return false;
        }
    }
    if (start < 0 || end < 0) return false;

    const std::vector<std::string>& labels = q.vars[(size_t)start].labels;
    if (labels.size() != 1) return false;
    for (int32_t v : verts)
        if (q.vars[(size_t)v].labels != labels) return false;

    std::vector<char> used(q.edges.size(), 0);
    std::vector<int32_t> order{start}, edge_order;
    int32_t cur = start;
    for (size_t step = 0; step < q.edges.size(); ++step) {
        int32_t ei = -1;
        for (size_t e = 0; e < q.edges.size(); ++e) {
            if (used[e] || q.edges[e].from != cur) continue;
            if (ei >= 0) return false;   // two outgoing edges: a branch, not a chain
            ei = (int32_t)e;
        }
        if (ei < 0) return false;
        used[(size_t)ei] = 1;
        edge_order.push_back(ei);
        cur = q.edges[(size_t)ei].to;
        if (std::find(order.begin(), order.end(), cur) != order.end()) return false;
        order.push_back(cur);
    }
    if (cur != end) return false;
    for (char x : used)
        if (!x) return false;

    out->len = (int32_t)q.edges.size();
    out->type = type;
    out->verts = std::move(order);
    out->edges = std::move(edge_order);
    out->names.clear();
    for (int32_t v : out->verts) out->names.push_back(q.vars[(size_t)v].name);
    return true;
}

}  // namespace

bool detect_depth_chain(const GraphStore& g, const Round& r, DepthChainSpec* out) {
    out->ok = false;
    if (r.queries.size() > 63) return false;
    // The normal gate is a batch (>=2 lengths).  V3_DEPTH_CHAIN_FORCE is a main40e-only
    // attribution switch: it lets one fixed-length chain use the same count expansion, so
    // five K=1 rounds can be timed against the K=5 round with one implementation.
    if (r.queries.size() < 2 && !std::getenv("V3_DEPTH_CHAIN_FORCE")) return false;
    std::vector<Chain> cs(r.queries.size());
    for (size_t i = 0; i < r.queries.size(); ++i)
        if (!chain_of(r.queries[i], &cs[i])) return false;

    std::set<int32_t> lengths;
    for (const Chain& c : cs)
        if (!lengths.insert(c.len).second) return false;

    std::vector<int32_t> order(cs.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(),
                     [&](int32_t a, int32_t b) { return cs[(size_t)a].len < cs[(size_t)b].len; });
    const Chain& longest = cs[(size_t)order.back()];
    if (longest.len <= 0) return false;
    for (int32_t ci : order) {
        const Chain& c = cs[(size_t)ci];
        if (c.type != longest.type) return false;
        if (c.names.size() > longest.names.size()) return false;
        // The variable names fix the direction: a reversed chain would not be a prefix.
        for (size_t i = 0; i < c.names.size(); ++i)
            if (c.names[i] != longest.names[i]) return false;
        for (size_t i = 0; i < c.verts.size(); ++i)
            if (r.queries[(size_t)ci].vars[(size_t)c.verts[i]].labels !=
                r.queries[(size_t)order.back()].vars[(size_t)longest.verts[i]].labels)
                return false;
    }

    const Query& first = r.queries[(size_t)order.front()];
    const std::string& label = first.vars[(size_t)cs[(size_t)order.front()].verts[0]].labels[0];
    const int32_t label_id = g.schema().label_of(label);
    const int32_t type_id = g.schema().type_of(longest.type);
    if (label_id < 0 || type_id < 0) return false;

    out->ok = true;
    out->label_id = label_id;
    out->type_id = type_id;
    out->max_len = longest.len;
    out->members.clear();
    out->lengths.clear();
    for (int32_t ci : order) {
        out->members.push_back(ci);
        out->lengths.push_back(cs[(size_t)ci].len);
    }
    return true;
}

}  // namespace v3
