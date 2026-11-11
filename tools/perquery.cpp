// Per-query reference method: DPiso filter, GQL order, LFTJ enumeration of every embedding.
// Usage: perquery --graph G --workload DIR [--batch B] [--timeout SEC] [--collect] [--query I]
#include "echo_common.h"
#include "echo_dpiso.h"

using namespace echo;

int main(int argc, char** argv) {
    Args a = parse_args(argc, argv);
    std::string graph = a.get("graph");
    std::string wdir = a.get("workload");
    if (graph.empty() || wdir.empty()) {
        std::fprintf(stderr, "usage: perquery --graph G --workload DIR [--batch B] [--timeout SEC] [--collect] [--query I]\n");
        return 2;
    }
    double timeout = a.getd("timeout", 120.0);
    int batch = (int)a.geti("batch", -1);
    int only = (int)a.geti("query", -1);
    bool collect = !a.get("collect").empty();
    Stats st("perquery");
    Stopwatch load_sw;
    Graph* data = load_data_graph(graph);
    double load_sec = load_sw.seconds();
    Workload w = read_workload_manifest(wdir);
    int first = (int)a.geti("b0", batch < 0 ? 0 : (int)w.batch_start(batch));
    int count = (int)a.geti("bn", batch < 0 ? (int)w.queries : (int)w.batch_count(batch));
    int b0 = first, b1 = first + count;
    double batch_deadline = a.getd("batch_deadline", 3600.0);
    long long done = 0, timeout_count = 0;
    double batch_sec = 0;
    // Query files are read before the timer: loading the workload is not algorithm time and the
    // ECHO side does not pay it either. Base-query construction stays inside the timer, as it does
    // for ECHO (run_envelope builds the base query of every member).
    std::vector<QueryGraph> pre;
    pre.reserve((size_t)std::max(0, b1 - b0));
    for (int i = b0; i < b1; ++i) pre.push_back(w.read(i));
    {
        Stopwatch bs;
        long long b_done = 0;
        for (int i = b0; i < b1; ++i) {
            if (only >= 0 && i != only) continue;
            double used = bs.seconds();
            double rem = std::min(timeout, batch_deadline - used);
            if (rem <= 0) { ++timeout_count; continue; }
            const QueryGraph& g = pre[(size_t)(i - b0)];
            Graph* qg = to_base_query(g);
            LfrtControl ctl;
            ctl.deadline = Clock::now() + std::chrono::milliseconds((long long)(rem * 1000));
            std::vector<std::vector<uint32_t>> sols;
            PerQueryResult r = dpiso_run(data, qg, collect ? &sols : nullptr, &ctl);
            delete qg;
            ++done;
            ++b_done;
            uint64_t lines = 0;
            if (collect) {
                for (auto& s : sols) {
                    for (size_t k = 0; k < s.size(); ++k) std::printf(k ? " %u" : "%u", s[k]);
                    std::printf("\n");
                    ++lines;
                }
            }
            st.seti("query_index", i);
            st.seti("timeout", r.timed_out ? 1 : 0);
            st.seti("filter_empty", r.ok ? 0 : 1);
            st.set("filter_sec", r.filter_sec);
            st.set("order_sec", r.order_sec);
            st.set("enum_sec", r.enum_sec);
            st.seti("embeddings", (long long)r.embeddings);
            st.set("query_sec", r.filter_sec + r.order_sec + r.enum_sec);
            if (collect) st.seti("solution_lines", (long long)lines);
            st.sets("record_end", "1");
            if (r.timed_out) ++timeout_count;
        }
        double bt = bs.seconds();
        batch_sec += bt;
        st.set("batch_sec", bt);
        st.seti("batch_done", b_done);
    }
    st.seti("queries_done", done);
    st.seti("timeouts", timeout_count);
    st.set("total_sec", batch_sec);
    st.set("load_sec", load_sec);
    st.seti("peak_rss_kb", peak_rss_kb());
    st.flush();
    delete data;
    return 0;
}
