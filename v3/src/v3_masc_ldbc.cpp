#include "v3_masc_ldbc.h"

#include "v3_exec.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace v3 {
namespace {

double now_sec() {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct VarInfo {
    std::vector<int> labels;   // :Message is the union of Comment and Post
    bool has_id = false;
    int64_t id = 0;
};

bool has_label(const std::vector<int>& labels, int l) {
    return std::find(labels.begin(), labels.end(), l) != labels.end();
}

std::string label_key(const std::vector<int>& labels) {
    std::string s;
    for (int l : labels) { s += std::to_string(l); s += ','; }
    return s;
}

// One fixed single-hop relationship.  `from`/`to` are the query variables as written;
// `any` is true for `-` / `both`, otherwise the relationship is directed from -> to.
struct EdgeInfo {
    int from = -1, to = -1;
    int type = -1;
    bool any = false;
    bool ok = false;
    std::string why;
};

// Per covered candidate: how a matching is written into a full Env for the RETURN pipeline.
struct QueryShape {
    int from = -1, to = -1, edge_var = -1;
    bool root_from = true;
    bool other_has_id = false;
    int64_t other_id = 0;
};

bool var_info(const Query& q, int vi, const Schema& s, VarInfo* out, std::string* why) {
    const PatternVar& pv = q.vars[(size_t)vi];
    if (pv.kind != PatternVar::VERTEX) { *why = "edge-variable"; return false; }
    if (pv.labels.empty()) { *why = "label-count"; return false; }
    for (const std::string& ln : pv.labels) {
        const int l = s.label_of(ln);
        if (l < 0) { *why = "unknown-label"; return false; }
        out->labels.push_back(l);
    }
    std::sort(out->labels.begin(), out->labels.end());
    out->labels.erase(std::unique(out->labels.begin(), out->labels.end()), out->labels.end());
    for (const auto& pr : pv.props) {
        if (pr.first != "id" || pr.second.is_str || pr.second.is_null) {
            *why = "non-id-property";
            return false;
        }
        if (out->has_id) { *why = "multiple-constants"; return false; }
        out->has_id = true;
        out->id = pr.second.i;
    }
    return true;
}

bool edge_info(const Query& q, const Schema& s, EdgeInfo* out) {
    if (q.edges.size() != 1) { out->why = "edge-count"; return false; }
    const PatternEdge& e = q.edges[0];
    if (e.lo != 1 || e.hi != 1) { out->why = "variable-length"; return false; }
    if (e.types.size() != 1) { out->why = "type-count"; return false; }
    out->type = s.type_of(e.types[0]);
    if (out->type < 0) { out->why = "unknown-type"; return false; }
    if (!e.props.empty()) { out->why = "edge-property"; return false; }
    if (e.path_nodes || !e.path.empty()) { out->why = "path-variable"; return false; }
    if (e.from == e.to) { out->why = "self-loop-pattern"; return false; }
    out->from = e.from;
    out->to = e.to;
    out->any = e.any_dir;
    out->ok = true;
    return true;
}

struct Group {
    int type = -1;
    bool any = false;
    std::vector<int> root_labels, other_labels;
    bool root_is_from = true;
    std::map<int64_t, std::vector<int>> by_root;   // constant roots -> candidate indices
    std::vector<int> no_root;                      // root not anchored
};

}  // namespace

MascLdbcRound masc_ldbc_run(const GraphStore& g, const Round& r, double cap_sec) {
    (void)cap_sec;
    MascLdbcRound res;
    res.attempted = r.queries.size();
    res.per_query.resize(r.queries.size());

    const size_t nq = r.queries.size();
    std::vector<VarInfo> vars(nq);
    std::vector<EdgeInfo> edges(nq);
    std::vector<QueryShape> shape(nq);
    std::vector<std::vector<Env>> envs(nq);   // one entry per matching of a covered candidate
    std::map<std::string, Group> groups;
    const Schema& s = g.schema();

    for (size_t i = 0; i < nq; ++i) {
        const Query& q = r.queries[i];
        MascLdbcCandidate& c = res.per_query[i];
        if (!q.is_plain()) { c.why = "not-plain"; continue; }
        if (q.sem != Semantics::CYPHER) { c.why = "semantics"; continue; }
        if (!q.atoms.empty()) { c.why = "where-atom"; continue; }

        if (q.edges.empty()) {
            // A single vertex lookup (IS4) or a label-class scan: one matching per vertex.
            int nverts = 0, only = -1;
            for (size_t v = 0; v < q.vars.size(); ++v)
                if (q.vars[v].kind == PatternVar::VERTEX) { ++nverts; only = (int)v; }
            if (nverts != 1) { c.why = "multi-vertex-no-edge"; continue; }
            VarInfo vi;
            std::string why;
            if (!var_info(q, only, s, &vi, &why)) { c.why = why; continue; }
            const double t0 = now_sec();
            std::vector<uint32_t> found;
            if (vi.has_id) {
                // The same per-label id index the serial path uses; collect every label, not
                // only the first hit (Message is Comment + Post).
                for (int l : vi.labels) {
                    uint32_t v = 0;
                    if (g.lookup_vertex((uint32_t)l, vi.id, &v)) found.push_back(v);
                }
            } else {
                for (int l : vi.labels)
                    for (uint32_t v : g.vertices_of((uint32_t)l)) found.push_back(v);
            }
            std::sort(found.begin(), found.end());
            found.erase(std::unique(found.begin(), found.end()), found.end());
            for (uint32_t v : found) {
                Env e;
                e.v.assign(q.vars.size(), kNoVal);
                e.v[(size_t)only] = v;
                envs[i].push_back(std::move(e));
            }
            c.tuples = vi.has_id ? (1 + found.size()) : (2 * found.size());
            c.solutions = found.size();
            c.covered = true;
            c.sec = now_sec() - t0;
            res.covered++;
            res.edges += c.edges;
            res.tuples += c.tuples;
            res.sec += c.sec;
            continue;
        }

        if (!edge_info(q, s, &edges[i])) { c.why = edges[i].why; continue; }
        int nverts = 0;
        for (const auto& pv : q.vars)
            if (pv.kind == PatternVar::VERTEX) ++nverts;
        if (nverts != 2) { c.why = "multi-vertex"; continue; }
        const int from = edges[i].from, to = edges[i].to;
        VarInfo vf, vt;
        std::string why;
        if (!var_info(q, from, s, &vf, &why)) { c.why = "from-" + why; continue; }
        if (!var_info(q, to, s, &vt, &why)) { c.why = "to-" + why; continue; }

        // Root at the anchored side; if both (or neither) are anchored, prefer `from`.
        const bool root_from = vf.has_id || !vt.has_id;
        const VarInfo& vr = root_from ? vf : vt;
        const VarInfo& vo = root_from ? vt : vf;
        // The group key includes which side is the root: rounds can contain both
        // `(m:Message {id})->(p:Person)` and `(p:Person)<-(m:Message {id})`, and a
        // shared `root_is_from` bit for the two directions would reject every edge of
        // one kind.
        Group& gr = groups[std::to_string(edges[i].type) + "|" +
                          (edges[i].any ? "any" : "dir") + "|" +
                          (root_from ? "f" : "t") + "|" +
                          label_key(vr.labels) + "|" + label_key(vo.labels)];
        gr.type = edges[i].type;
        gr.any = edges[i].any;
        gr.root_labels = vr.labels;
        gr.other_labels = vo.labels;
        gr.root_is_from = root_from;
        if (vr.has_id) gr.by_root[vr.id].push_back((int)i);
        else gr.no_root.push_back((int)i);
        shape[i].from = from;
        shape[i].to = to;
        shape[i].edge_var = q.edges[0].var_name.empty() ? -1 : q.var_index(q.edges[0].var_name);
        shape[i].root_from = root_from;
        shape[i].other_has_id = vo.has_id;
        shape[i].other_id = vo.id;
        c.covered = true;   // counted by the group pass below
        res.covered++;
    }

    // One matching -> one Env in the covered candidate's own variable order.  The RETURN
    // pipeline later projects it and charges the output rows to the adapter's work.
    auto bind = [&](int qi, uint32_t root, uint32_t nb, uint32_t rel) {
        const Query& qq = r.queries[(size_t)qi];
        Env e;
        e.v.assign(qq.vars.size(), kNoVal);
        if (shape[(size_t)qi].root_from) {
            e.v[(size_t)shape[(size_t)qi].from] = root;
            e.v[(size_t)shape[(size_t)qi].to] = nb;
        } else {
            e.v[(size_t)shape[(size_t)qi].from] = nb;
            e.v[(size_t)shape[(size_t)qi].to] = root;
        }
        if (shape[(size_t)qi].edge_var >= 0)
            e.v[(size_t)shape[(size_t)qi].edge_var] = rel;
        envs[(size_t)qi].push_back(std::move(e));
        res.per_query[(size_t)qi].solutions++;
        res.tuples += 1;
    };

    for (auto& kv : groups) {
        Group& gr = kv.second;
        const AdjView av = g.typed((uint32_t)gr.type);
        if (!gr.by_root.empty()) {
            // The anchored roots come from the same per-label id index the serial path
            // uses: one lookup per distinct id, then MASC's shared typed-adjacency probe.
            // The unanchored group below keeps the label-class scan.
            const double t0 = now_sec();
            for (const auto& anchor : gr.by_root) {
                for (int rl : gr.root_labels) {
                    uint32_t root = 0;
                    if (!g.lookup_vertex((uint32_t)rl, anchor.first, &root)) continue;
                    ++res.tuples;   // one id lookup
                    const std::vector<int>& qs = anchor.second;
                    const uint32_t b = (*av.off)[root], e = (*av.off)[root + 1];
                    for (uint32_t k = b; k < e; ++k) {
                        ++res.edges;
                        const uint32_t rel = g.typed_rel((uint32_t)gr.type, root, k - b);
                        if (!gr.any) {
                            const uint8_t d = g.edge_dir(rel);
                            const uint8_t want = gr.root_is_from ? 1 : 0;
                            if (d != want) continue;
                        }
                        const uint32_t nb = (*av.nbr)[k];
                        if (!has_label(gr.other_labels, (int)g.label(nb))) continue;
                        for (int qi : qs) {
                            if (shape[(size_t)qi].other_has_id &&
                                (int64_t)nb != shape[(size_t)qi].other_id)
                                continue;
                            bind(qi, root, nb, rel);
                        }
                    }
                }
            }
            res.sec += now_sec() - t0;
        }
        for (int qi : gr.no_root) {
            const double t0 = now_sec();
            for (int rl : gr.root_labels) {
                const GraphArray<uint32_t>& roots = g.vertices_of((uint32_t)rl);
                for (uint32_t root : roots) {
                    ++res.tuples;
                    const uint32_t b = (*av.off)[root], e = (*av.off)[root + 1];
                    for (uint32_t k = b; k < e; ++k) {
                        ++res.edges;
                        const uint32_t rel = g.typed_rel((uint32_t)gr.type, root, k - b);
                        if (!gr.any) {
                            const uint8_t d = g.edge_dir(rel);
                            const uint8_t want = gr.root_is_from ? 1 : 0;
                            if (d != want) continue;
                        }
                        const uint32_t nb = (*av.nbr)[k];
                        if (!has_label(gr.other_labels, (int)g.label(nb))) continue;
                        if (shape[(size_t)qi].other_has_id &&
                            (int64_t)nb != shape[(size_t)qi].other_id)
                            continue;
                        bind(qi, root, nb, rel);
                    }
                }
            }
            res.sec += now_sec() - t0;
        }
    }

    // Same-contract projection: every covered candidate runs the serial RETURN / grouping /
    // aggregation pipeline over its matchings.  The rows are kept for v3_check and charged to
    // the adapter's work ledger; the wall time includes the projection.
    for (size_t i = 0; i < nq; ++i) {
        MascLdbcCandidate& c = res.per_query[i];
        if (!c.covered) continue;
        const double p0 = now_sec();
        CandidateResult out;
        Counters pc;
        QueryExec::project_only(g, r.queries[i], envs[i], &out, true, true, true, -1, &pc);
        const uint64_t rows = out.ncols ? (uint64_t)(out.data.size() / out.ncols) : 0;
        c.out_rows = rows;
        c.result = std::move(out);
        const double dt = now_sec() - p0;
        c.sec += dt;
        res.tuples += rows;
        res.sec += dt;
    }

    for (size_t i = 0; i < res.per_query.size(); ++i) {
        if (res.per_query[i].covered) res.solutions += res.per_query[i].solutions;
    }
    return res;
}

}  // namespace v3
