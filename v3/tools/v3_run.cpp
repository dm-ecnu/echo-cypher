// v3_run --sidecar DIR --round FILE [--stats FILE] [--no-collect] [--serial]
//
// Evaluates one round (all candidates of one intent family) and reports the operator ledger.
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>

#include "v3_exec.h"
#include "v3_json.h"
#include "v3_plan.h"

using namespace v3;

int main(int argc, char** argv) {
    std::string sidecar, round_path, stats;
    bool collect = true;
    bool serial = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--sidecar" && i + 1 < argc) sidecar = argv[++i];
        else if (a == "--round" && i + 1 < argc) round_path = argv[++i];
        else if (a == "--stats" && i + 1 < argc) stats = argv[++i];
        else if (a == "--no-collect") collect = false;
        else if (a == "--serial") serial = true;
        else if (a == "--no-mcs") setenv("V3_MCS_OFF", "1", 1);
        else if (a == "--no-depth-layer") setenv("V3_MCS_DEPTH_OFF", "1", 1);
    }
    if (sidecar.empty() || round_path.empty()) {
        std::fprintf(stderr, "usage: v3_run --sidecar DIR --round FILE [--stats FILE] [--no-collect]\n");
        return 2;
    }
    GraphStore g;
    std::string err;
    const auto load_t0 = std::chrono::steady_clock::now();
    if (!g.load(sidecar, &err)) {
        std::fprintf(stderr, "load: %s\n", err.c_str());
        return 1;
    }
    const double load_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - load_t0).count();
    Round r;
    if (!load_round(round_path, &r, &err)) {
        std::fprintf(stderr, "round: %s\n", err.c_str());
        return 1;
    }
    Executor::Options opt;
    opt.collect = collect;
    opt.serial_only = serial;
    Executor ex(g, g.schema(), opt);
    std::vector<CandidateOut> out;
    Counters cnt;
    RoundPlan plan;
    const auto batch_t0 = std::chrono::steady_clock::now();
    if (!ex.run(r, &out, &cnt, &plan, &err)) {
        std::fprintf(stderr, "run: %s\n", err.c_str());
        return 1;
    }
    const double batch_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - batch_t0).count();
    // The driver's W is edges_expanded + tuples.  `cnt.tuples` counts the rows at the leaves of
    // the growth (which includes the completed matchings on the masked path); the intermediate
    // tuples are the root scans plus those leaves that are not complete matchings.
    const uint64_t edges_expanded = cnt.edges_expanded + cnt.vl_steps + cnt.mcs_edges;
    const uint64_t intermediate = cnt.init_tuples + cnt.mcs_vertices +
        (cnt.tuples > cnt.solutions ? cnt.tuples - cnt.solutions : 0);
    const uint64_t work = cnt.work();
    if (std::getenv("V3_DEBUG_OUT")) {
        for (size_t i = 0; i < out.size(); ++i) {
            const CandidateOut& c = out[i];
            const size_t nc = c.ncols ? c.ncols : 1;
            std::fprintf(stderr, "[out] i=%zu %s planned=%d ncols=%u rows=%zu\n", i,
                         r.queries[i].id.c_str(), (int)c.planned, c.ncols, c.data.size() / nc);
            for (size_t k = 0; k < c.data.size() / nc && k < 12; ++k) {
                std::fprintf(stderr, "  ");
                for (size_t j = 0; j < nc; ++j) {
                    const Value& v = c.data[k * nc + j];
                    if (v.is_null) std::fprintf(stderr, " NULL");
                    else if (v.is_str) std::fprintf(stderr, " %s", v.s.c_str());
                    else std::fprintf(stderr, " %lld", (long long)v.i);
                }
                std::fprintf(stderr, "\n");
            }
        }
    }
    uint64_t result_rows = 0;
    for (const auto& c : out) result_rows += c.ncols ? (uint64_t)(c.data.size() / c.ncols) : 0;
    // The driver compares this with the base's `embeddings_total`, i.e. the rows an ORDER/LIMIT
    // actually emits.  `solutions` stays the M5 matching count.
    const uint64_t output_tuples = result_rows;
    std::printf("round=%s candidates=%zu fallback=%zu envelopes=%zu solutions=%llu rows=%llu "
                "edges=%llu tuples=%llu work=%llu\n",
                r.family.c_str(), r.queries.size(), plan.fallback.size(), plan.envelopes.size(),
                (unsigned long long)cnt.solutions, (unsigned long long)result_rows,
                (unsigned long long)edges_expanded, (unsigned long long)intermediate,
                (unsigned long long)work);
    if (!stats.empty()) {
        std::ofstream f(stats);
        f << "{\"family\":\"" << r.family << "\",\"candidates\":" << r.queries.size()
          << ",\"fallback\":" << plan.fallback.size()
          << ",\"envelopes\":" << plan.envelopes.size()
          << ",\"batch_sec\":" << batch_sec << ",\"load_sec\":" << load_sec
          << ",\"edges_expanded\":" << edges_expanded << ",\"tuples\":" << intermediate
          << ",\"mcs_edges\":" << cnt.mcs_edges << ",\"mcs_vertices\":" << cnt.mcs_vertices
          << ",\"mcs_bytes\":" << cnt.mcs_bytes
          << ",\"mcs_build_sec\":" << cnt.mcs_build_sec
          << ",\"plan_sec\":" << ex.stats.plan_sec
          << ",\"env_sec\":" << ex.stats.env_sec
          << ",\"fallback_sec\":" << ex.stats.fallback_sec
          << ",\"departures\":" << ex.stats.departures
          << ",\"prefix_envelopes\":" << ex.stats.prefix_envelopes
          << ",\"output_tuples\":" << output_tuples << ",\"result_rows\":" << result_rows
          << ",\"work\":" << work << ",\"solutions\":" << cnt.solutions
          << ",\"per_candidate\":[";
        for (size_t i = 0; i < out.size(); ++i) {
            const CandidateOut& c = out[i];
            const uint64_t rows = c.ncols ? (uint64_t)(c.data.size() / c.ncols) : 0;
            if (i) f << ",";
            f << "{\"id\":\"" << r.queries[i].id << "\",\"planned\":"
              << (c.planned ? "true" : "false") << ",\"rows\":" << rows
              << ",\"matchings\":" << c.matchings.size() << "}";
        }
        f << "]}\n";
    }
    return 0;
}
