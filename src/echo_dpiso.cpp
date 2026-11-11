// Per-query DPiso through the vendored base: FilterVertices::DPisoFilter,
// GenerateQueryPlan::generateGQLQueryPlan and EvaluateQuery::LFTJ, i.e. the code path of
// SubgraphMatching.out with -filter DPiso -order GQL -engine LFTJ. ECHO's N4 reuses the same
// DPisoFilter into a generator-backed graph view (see echo_mask.cpp).
//
// The only changes to the base are the LFTJ hook (patch/EvaluateQuery.LFTJ.patch): the current
// embedding is handed to echo_emit_embedding() whenever an embedding is counted, and LFTJ stops
// once the deadline passed when a deadline is armed (g_lfrt_abort != nullptr).
#include "echo_dpiso.h"

#include "matching/BuildTable.h"
#include "matching/EvaluateQuery.h"
#include "matching/FilterVertices.h"
#include "matching/GenerateQueryPlan.h"

namespace echo {
std::atomic<bool>* g_lfrt_abort = nullptr;
Clock::time_point g_lfrt_deadline = Clock::time_point::max();
VertexID* g_lfrt_embedding = nullptr;

namespace {
std::vector<std::vector<uint32_t>>* g_sink = nullptr;
uint32_t g_size = 0;

void free_tables(const Graph* query, Edges*** edge_matrix) {
    for (ui i = 0; i < query->getVerticesCount(); ++i) {
        for (ui j = 0; j < query->getVerticesCount(); ++j) {
            if (edge_matrix[i][j] != nullptr) {
                // Edges owns both buffers (see ~Edges): deleting the object frees them. Mixing
                // free() with new was undefined behaviour that ASan flags as a mismatch.
                delete edge_matrix[i][j];
            }
        }
        delete[] edge_matrix[i];
    }
    delete[] edge_matrix;
}
}  // namespace

// `embedding` is LFTJ's pv array indexed by query vertex id.
void emit_embedding(VertexID* embedding) {
    if (g_sink == nullptr) return;
    auto& sol = g_sink->emplace_back(g_size);
    for (uint32_t d = 0; d < g_size; ++d) sol[d] = embedding[d];
}

bool dpiso_filter(const Graph* data, Graph* query, ui**& candidates, ui*& candidates_count, ui*& order,
                  TreeNode*& tree) {
    return FilterVertices::DPisoFilter(data, query, candidates, candidates_count, order, tree);
}

// The base's filtered candidate lists for one query, handed out so a caller can finish a
// nearly-complete partial assignment without rebuilding edge tables.
bool dpiso_candidates(const Graph* data, Graph* query, std::vector<std::vector<uint32_t>>* out) {
    const ui qn = query->getVerticesCount();
    ui** candidates = nullptr;
    ui* candidates_count = nullptr;
    ui* order = new ui[qn + 1];
    TreeNode* tree = new TreeNode[qn];
    bool ok = FilterVertices::DPisoFilter(data, query, candidates, candidates_count, order, tree);
    out->assign(qn, {});
    if (ok)
        for (ui i = 0; i < qn; ++i) out->at(i).assign(candidates[i], candidates[i] + candidates_count[i]);
    for (ui i = 0; i < qn; ++i) delete[] candidates[i];
    delete[] candidates;
    delete[] candidates_count;
    delete[] order;
    delete[] tree;
    return ok;
}

// Complete a departing member from several prefixes at once: the base filter runs once for the
// member, and each prefix only pins its vertices, rebuilds the edge tables and enumerates. The
// filter dominates the cost, so sharing it across prefixes is what makes deep departures cheap.
PerQueryResult dpiso_run_pinned_multi(const Graph* data, Graph* query,
                                      const std::vector<std::vector<std::pair<int, uint32_t>>>& prefixes,
                                      std::vector<std::vector<std::vector<uint32_t>>>* sinks,
                                      LfrtControl* control) {
    PerQueryResult r;
    const ui qn = query->getVerticesCount();
    ui** candidates = nullptr;
    ui* candidates_count = nullptr;
    ui* order = new ui[qn + 1];
    TreeNode* tree = new TreeNode[qn];
    Stopwatch sw;
    if (!FilterVertices::DPisoFilter(data, query, candidates, candidates_count, order, tree)) {
        r.filter_sec = sw.seconds();
        r.ok = false;
        delete[] order;
        delete[] tree;
        return r;
    }
    r.filter_sec = sw.seconds();
    sinks->assign(prefixes.size(), {});

    for (size_t pi = 0; pi < prefixes.size(); ++pi) {
        sw.reset();
        // Fresh candidate lists for this prefix (the filter's arrays are reused).
        ui** cand = new ui*[qn];
        ui* cnt = new ui[qn];
        bool feasible = true;
        for (ui i = 0; i < qn; ++i) {
            cnt[i] = candidates_count[i];
            cand[i] = new ui[cnt[i]];
            for (ui k = 0; k < cnt[i]; ++k) cand[i][k] = candidates[i][k];
        }
        for (auto& pr : prefixes[pi]) {
            if (pr.first < 0 || (ui)pr.first >= qn) { feasible = false; break; }
            ui*& list = cand[pr.first];
            ui c = cnt[pr.first];
            bool found = false;
            for (ui k = 0; k < c; ++k) if (list[k] == pr.second) { found = true; break; }
            delete[] list;
            list = new ui[1];
            cnt[pr.first] = found ? 1 : 0;
            if (found) list[0] = pr.second;
            if (!found) { feasible = false; break; }
        }
        if (!feasible) {
            for (ui i = 0; i < qn; ++i) delete[] cand[i];
            delete[] cand;
            delete[] cnt;
            continue;
        }
        Edges*** em = new Edges**[qn];
        for (ui i = 0; i < qn; ++i) em[i] = new Edges*[qn];
        BuildTable::buildTables(data, query, cand, cnt, em);
        ui* ord_gql = new ui[qn];
        {
            ui* pivot = nullptr;
            GenerateQueryPlan::generateGQLQueryPlan(data, query, cnt, order, pivot);
            std::memcpy(ord_gql, order, sizeof(ui) * qn);
            delete[] pivot;
        }
        size_t call_count = 0;
        g_sink = &(*sinks)[pi];
        g_size = qn;
        if (control) { g_lfrt_abort = &control->abort; g_lfrt_deadline = control->deadline; }
        else g_lfrt_deadline = Clock::time_point::max();
        r.embeddings += EvaluateQuery::LFTJ(data, query, em, cand, cnt, ord_gql,
                                            std::numeric_limits<size_t>::max(), call_count);
        g_lfrt_abort = nullptr;
        g_sink = nullptr;
        r.enum_sec += sw.seconds();
        for (ui i = 0; i < qn; ++i) delete[] cand[i];
        delete[] cand;
        delete[] cnt;
        delete[] ord_gql;
        free_tables(query, em);
        if (control && control->abort.load()) { r.timed_out = true; break; }
    }
    for (ui i = 0; i < qn; ++i) delete[] candidates[i];
    delete[] candidates;
    delete[] candidates_count;
    delete[] order;
    delete[] tree;
    return r;
}

// Same pipeline, but with a partial assignment: `pins` lists (query vertex, data vertex) pairs
// that the match must take. The base's filter runs unchanged, then each pinned vertex's candidate
// list is restricted to its one data vertex, so the base enumerator only walks matches that agree
// with the prefix. This is how a member that left the envelope walk is completed from its prefix
// without a second, hand-written recursion and without enumerating the whole query first.
PerQueryResult dpiso_run_pinned(const Graph* data, Graph* query,
                               const std::vector<std::pair<int, uint32_t>>& pins,
                               std::vector<std::vector<uint32_t>>* solutions, LfrtControl* control) {
    PerQueryResult r;
    ui** candidates = nullptr;
    ui* candidates_count = nullptr;
    ui* order = new ui[query->getVerticesCount() + 1];
    TreeNode* tree = new TreeNode[query->getVerticesCount()];
    const ui qn = query->getVerticesCount();
    Stopwatch sw;
    if (!FilterVertices::DPisoFilter(data, query, candidates, candidates_count, order, tree)) {
        r.filter_sec = sw.seconds();
        r.ok = false;
        for (ui i = 0; i < qn; ++i) delete[] candidates[i];
        delete[] candidates;
        delete[] candidates_count;
        delete[] order;
        delete[] tree;
        return r;
    }
    // Restrict the pinned vertices to their fixed data vertex. compactCandidates() in the base
    // expects ascending lists, which a single element trivially satisfies.
    bool feasible = true;
    for (auto& pr : pins) {
        if (pr.first < 0 || (ui)pr.first >= qn) { feasible = false; break; }
        ui*& list = candidates[pr.first];
        ui cnt = candidates_count[pr.first];
        ui found = 0;
        for (ui k = 0; k < cnt; ++k)
            if (list[k] == pr.second) { found = 1; break; }
        delete[] list;
        list = new ui[1];
        candidates_count[pr.first] = found ? 1 : 0;
        if (found) list[0] = pr.second;
        if (!found) feasible = false;
    }
    r.filter_sec = sw.seconds();
    if (!feasible) {
        for (ui i = 0; i < qn; ++i) delete[] candidates[i];
        delete[] candidates;
        delete[] candidates_count;
        delete[] order;
        delete[] tree;
        r.ok = false;
        return r;
    }

    sw.reset();
    Edges*** edge_matrix = new Edges**[qn];
    for (ui i = 0; i < qn; ++i) edge_matrix[i] = new Edges*[qn];
    BuildTable::buildTables(data, query, candidates, candidates_count, edge_matrix);
    ui* order_gql = new ui[qn];
    {
        ui* pivot = nullptr;
        GenerateQueryPlan::generateGQLQueryPlan(data, query, candidates_count, order, pivot);
        std::memcpy(order_gql, order, sizeof(ui) * qn);
        delete[] pivot;
    }
    r.order_sec = sw.seconds();

    sw.reset();
    size_t call_count = 0;
    g_sink = solutions;
    g_size = qn;
    if (control) {
        g_lfrt_abort = &control->abort;
        g_lfrt_deadline = control->deadline;
    } else {
        g_lfrt_deadline = Clock::time_point::max();
    }
    r.embeddings = EvaluateQuery::LFTJ(data, query, edge_matrix, candidates, candidates_count, order_gql,
                                       std::numeric_limits<size_t>::max(), call_count);
    g_lfrt_abort = nullptr;
    g_sink = nullptr;
    r.timed_out = control && control->abort.load();
    r.enum_sec = sw.seconds();

    for (ui i = 0; i < qn; ++i) delete[] candidates[i];
    delete[] candidates;
    delete[] candidates_count;
    delete[] order;
    delete[] order_gql;
    delete[] tree;
    free_tables(query, edge_matrix);
    return r;
}

PerQueryResult dpiso_run(const Graph* data, Graph* query, std::vector<std::vector<uint32_t>>* solutions,
                         LfrtControl* control) {
    PerQueryResult r;
    ui** candidates = nullptr;
    ui* candidates_count = nullptr;
    ui* order = new ui[query->getVerticesCount() + 1];
    TreeNode* tree = new TreeNode[query->getVerticesCount()];
    const ui qn = query->getVerticesCount();
    Stopwatch sw;
    if (!FilterVertices::DPisoFilter(data, query, candidates, candidates_count, order, tree)) {
        r.filter_sec = sw.seconds();
        r.ok = false;
        for (ui i = 0; i < qn; ++i) delete[] candidates[i];
        delete[] candidates;
        delete[] candidates_count;
        delete[] order;
        delete[] tree;
        return r;
    }
    r.filter_sec = sw.seconds();

    sw.reset();
    Edges*** edge_matrix = new Edges**[qn];
    for (ui i = 0; i < qn; ++i) edge_matrix[i] = new Edges*[qn];
    BuildTable::buildTables(data, query, candidates, candidates_count, edge_matrix);
    ui* order_gql = new ui[qn];
    {
        ui* pivot = nullptr;
        GenerateQueryPlan::generateGQLQueryPlan(data, query, candidates_count, order, pivot);
        std::memcpy(order_gql, order, sizeof(ui) * qn);
        delete[] pivot;
    }
    r.order_sec = sw.seconds();

    sw.reset();
    size_t call_count = 0;
    g_sink = solutions;
    g_size = qn;
    if (control) {
        g_lfrt_abort = &control->abort;
        g_lfrt_deadline = control->deadline;
    } else {
        g_lfrt_deadline = Clock::time_point::max();
    }
    r.embeddings = EvaluateQuery::LFTJ(data, query, edge_matrix, candidates, candidates_count, order_gql,
                                       std::numeric_limits<size_t>::max(), call_count);
    g_lfrt_abort = nullptr;
    g_sink = nullptr;
    r.timed_out = control && control->abort.load();
    r.enum_sec = sw.seconds();

    for (ui i = 0; i < qn; ++i) delete[] candidates[i];
    delete[] candidates;
    delete[] candidates_count;
    delete[] order;
    delete[] order_gql;
    delete[] tree;
    free_tables(query, edge_matrix);
    return r;
}

}  // namespace echo
