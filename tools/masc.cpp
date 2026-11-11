// MASC (SIGMOD 2026) runner on the shared native base.
// Usage: masc --graph G --workload DIR [--batch B] [--batch-sec 3600] [--collect] [--query I]
//
// The reporting shape is the one tools/perquery.cpp uses (embedding lines in query order,
// one ECHO_STATS record per query, then the batch record between ECHO_STATS_BEGIN/END), so the
// same runner and the same equivalence assertion consume both.
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include "masc.h"

using namespace echo;

int main(int argc, char** argv) {
    Args a = parse_args(argc, argv);
    std::string graph = a.get("graph");
    std::string wdir = a.get("workload");
    if (graph.empty() || wdir.empty()) {
        std::fprintf(stderr,
                     "usage: masc --graph G --workload DIR [--batch B] [--batch-sec SEC] "
                     "[--collect] [--query I]\n");
        return 2;
    }
    // --batch_deadline is the name the mainline cell runner passes; --batch-sec is the same knob.
    const double batch_limit = a.getd("batch-sec", a.getd("batch_deadline", 3600.0));
    const double per_query_limit = a.getd("timeout", 120.0);  // MASC is bounded per batch, see opt
    const int batch = (int)a.geti("batch", -1);
    const int only = (int)a.geti("query", -1);
    const bool collect = !a.get("collect").empty();

    Stats st("masc");
    Stopwatch load_sw;
    Graph* data = load_data_graph(graph);
    const double load_sec = load_sw.seconds();
    Workload w = read_workload_manifest(wdir);
    const int first = (int)a.geti("b0", batch < 0 ? 0 : (int)w.batch_start(batch));
    const int count = (int)a.geti("bn", batch < 0 ? (int)w.queries : (int)w.batch_count(batch));
    int b0 = first, b1 = first + count;

    std::vector<QueryGraph> queries;
    std::vector<int> index;
    for (int i = b0; i < b1; ++i) {
        if (only >= 0 && i != only) continue;
        if (i >= (int)w.queries) break;
        queries.push_back(w.read(i));
        index.push_back(i);
    }

    // Setup shared by every method and excluded from the comparison clocks, like the graph load:
    // the label/degree index is built once for the whole batch.
    Stopwatch setup_sw;
    LabelIndex label_index;
    label_index.build(data);
    const double index_sec = setup_sw.seconds();

    std::vector<std::vector<std::vector<uint32_t>>> cand;
    SigFilterStats fst;
    Stopwatch total_sw;
    signature_filter(data, label_index, queries, cand, &fst);

    MascOptions opt;
    opt.collect = collect;
    opt.batch_deadline_sec = batch_limit;
    opt.timeout_sec = per_query_limit;
    const Clock::time_point deadline =
        Clock::now() + std::chrono::milliseconds((long long)(batch_limit * 1000.0));
    std::atomic<bool> abort{false};
    MascResult r = masc_run(data, queries, cand, opt, deadline, &abort, &label_index);
    const double total = total_sw.seconds();

    long long lines_total = 0;
    long long timeouts = 0;
    for (size_t s = 0; s < queries.size(); ++s) {
        long long lines = 0;
        if (collect) {
            for (const auto& sol : r.per_query[s]) {
                for (size_t k = 0; k < sol.size(); ++k) std::printf(k ? " %u" : "%u", sol[k]);
                std::printf("\n");
                ++lines;
            }
            lines_total += lines;
        }
        st.seti("query_index", index[s]);
        st.seti("embeddings", (long long)r.embeddings[s]);
        st.set("filter_sec", s == 0 ? fst.sec : 0.0);
        st.set("order_sec", s == 0 ? r.stats.order_sec : 0.0);
        st.set("enum_sec", s == 0 ? r.stats.enum_sec : 0.0);
        st.set("query_sec", s == 0 ? fst.sec + r.stats.order_sec + r.stats.enum_sec : 0.0);
        st.seti("filter_empty", r.filter_empty[s] ? 1 : 0);
        st.seti("timeout", (r.query_timeout[s] || r.batch_timeout) ? 1 : 0);
        if (collect) st.seti("solution_lines", lines);
        st.sets("record_end", "1");
        if (r.query_timeout[s] || r.batch_timeout) ++timeouts;
    }
    st.seti("queries_done", (long long)queries.size());
    st.seti("timeouts", timeouts);
    st.seti("embeddings_total", (long long)r.stats.embeddings);
    st.set("filter_sec", fst.sec);
    st.set("order_sec", r.stats.order_sec);
    st.set("block_sec", r.stats.block_sec);
    st.set("enum_sec", r.stats.enum_sec);
    st.set("total_sec", total);
    st.set("batch_sec", total);
    st.set("per_query_timeout_arg", per_query_limit);
    st.set("load_sec", load_sec);
    st.set("index_sec", index_sec);
    st.seti("batch_timeout", r.batch_timeout ? 1 : 0);
    st.seti("batch_size", (long long)queries.size());
    st.seti("sig_cells", (long long)fst.cells);
    st.seti("sig_cell_evals", (long long)fst.cell_evals);
    st.seti("sig_vertex_evals", (long long)fst.vertex_evals);
    st.seti("filter_probes", (long long)fst.probes);
    st.seti("search_nodes", (long long)r.stats.nodes);
    st.seti("cand_visits", (long long)r.stats.cand_visits);
    st.seti("shared_visits", (long long)r.stats.shared_visits);
    st.seti("memo_hits", (long long)r.stats.memb_lock_hits);
    st.seti("memo_misses", (long long)r.stats.memb_lock_misses);
    st.seti("memo_stored", (long long)r.stats.memb_lock_stored);
    st.seti("memo_skipped", (long long)r.stats.memb_lock_skipped);
    st.seti("peak_rss_kb", peak_rss_kb());
    st.flush();
    delete data;
    return 0;
}
