// Count a pure pattern with the base's own per-query pipeline (DPiso filter -> GQL order ->
// LFTJ), the reference the paper's per-query baseline uses.  main28: the v3 walk was correct on
// these patterns but 40-100x the base on the large |V_q| size tests (DBLP k64 b4: 191M
// embeddings; Figeys k64 b2: 447M), because it visits every matching instead of running LFTJ's
// per-level intersection.  The fallback is specified to cost no more than the base, so the
// count of a shape the base supports is asked from the base itself.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

#include "graph/graph.h"

#include "v3_base.h"
#include "echo_dpiso.h"
#include "v3_exec.h"

namespace v3 {

namespace {

// "<name>" -> the integer label id of the v2 export ("L84" -> 84).
bool label_id(const std::string& name, int32_t* out) {
    if (name.size() < 2 || name[0] != 'L') return false;
    char* end = nullptr;
    const long x = std::strtol(name.c_str() + 1, &end, 10);
    if (!end || *end != '\0' || x < 0) return false;
    *out = (int32_t)x;
    return true;
}

}  // namespace

bool base_count_pure(const Graph* data, const Query& q, uint64_t* out, std::string* why) {
    if (data == nullptr || out == nullptr) return false;
    if (q.sem != Semantics::ISO) { if (why) *why = "semantics"; return false; }
    int match_clauses = 0, return_clauses = 0;
    for (const auto& cl : q.clauses) {
        if (cl.kind == ClauseKind::MATCH) ++match_clauses;
        else if (cl.kind == ClauseKind::RETURN) ++return_clauses;
        else { if (why) *why = "clause"; return false; }
        if (!cl.where.empty() || !cl.pre_where.empty()) { if (why) *why = "where"; return false; }
    }
    if (match_clauses != 1 || return_clauses != 1 || !q.atoms.empty()) {
        if (why) *why = "shape";
        return false;
    }
    if (q.items.size() != 1 || q.items[0].kind != ProjectionItem::AGG ||
        q.items[0].func != AggFunc::COUNT_STAR) {
        if (why) *why = "projection";
        return false;
    }
    const Clause& ret = q.clauses.empty() ? q.clauses[0] : q.clauses.back();
    (void)ret;
    std::vector<int32_t> lab(q.vars.size(), -1);
    for (size_t v = 0; v < q.vars.size(); ++v) {
        const PatternVar& pv = q.vars[v];
        if (pv.kind != PatternVar::VERTEX || pv.labels.size() != 1 || !pv.props.empty()) {
            if (why) *why = "vertex";
            return false;
        }
        if (!label_id(pv.labels[0], &lab[v])) { if (why) *why = "label"; return false; }
    }
    std::vector<std::pair<int32_t, int32_t>> edges;
    std::vector<int32_t> deg(q.vars.size(), 0);
    for (const auto& pe : q.edges) {
        if (pe.lo != 1 || pe.hi != 1 || pe.types.size() != 1 || !pe.var_name.empty() ||
            !pe.props.empty()) {
            if (why) *why = "edge";
            return false;
        }
        int32_t a = pe.from, b = pe.to;
        if (a == b) { if (why) *why = "self"; return false; }
        if (a > b) std::swap(a, b);
        edges.emplace_back(a, b);
        ++deg[(size_t)a];
        ++deg[(size_t)b];
    }
    if (edges.empty()) { if (why) *why = "no_edge"; return false; }

    std::ostringstream text;
    text << "t " << q.vars.size() << " " << edges.size() << "\n";
    for (size_t v = 0; v < q.vars.size(); ++v)
        text << "v " << v << " " << lab[v] << " " << deg[v] << "\n";
    for (const auto& e : edges) text << "e " << e.first << " " << e.second << "\n";
    const char* tmp = std::getenv("TMPDIR");
    const std::string path = std::string(tmp && *tmp ? tmp : "/tmp") + "/v3-q-" +
                             std::to_string((long long)::getpid()) + "-" +
                             std::to_string((long long)(uintptr_t)&q) + ".graph";
    {
        std::ofstream f(path);
        if (!f) { if (why) *why = "tmp"; return false; }
        f << text.str();
    }
    Graph* qg = new Graph(false);
    qg->loadGraphFromFile(path);
    std::remove(path.c_str());
    echo::LfrtControl ctl;
    ctl.deadline = echo::Clock::now() + std::chrono::seconds(1800);
    const echo::PerQueryResult r = echo::dpiso_run(data, qg, nullptr, &ctl);
    delete qg;
    if (r.timed_out) { if (why) *why = "timeout"; return false; }
    *out = (uint64_t)r.embeddings;
    return true;
}

}  // namespace v3
