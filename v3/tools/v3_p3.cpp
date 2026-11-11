// v3_p3: the P3 assertion.  A round whose candidates do not pay for sharing is planned
// serially (one candidate per envelope); its work and wall time must stay within 1.05x of
// evaluating every candidate alone with the same base enumerator.
//
//   v3_p3 --sidecar DIR --round FILE [--repeat N] [--factor 1.05]
//
// Work is the same ledger as v3_run --stats: initial tuples, edge expansions, variable-length
// steps/rows, intermediate tuples and aggregation rows.  Exit 0 iff both ratios are within the
// factor; prints one JSON line.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "v3_exec.h"
#include "v3_executor.h"
#include "v3_plan.h"

using namespace v3;

static double now_sec() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char** argv) {
    std::string sidecar, round_path;
    int repeat = 3;
    double factor = 1.05;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--sidecar" && i + 1 < argc) sidecar = argv[++i];
        else if (a == "--round" && i + 1 < argc) round_path = argv[++i];
        else if (a == "--repeat" && i + 1 < argc) repeat = std::atoi(argv[++i]);
        else if (a == "--factor" && i + 1 < argc) factor = std::atof(argv[++i]);
    }
    if (sidecar.empty() || round_path.empty()) {
        std::fprintf(stderr, "usage: v3_p3 --sidecar DIR --round FILE [--repeat N] [--factor F]\n");
        return 2;
    }
    GraphStore g;
    std::string err;
    if (!g.load(sidecar, &err)) {
        std::fprintf(stderr, "load: %s\n", err.c_str());
        return 2;
    }
    Round r;
    if (!load_round(round_path, &r, &err)) {
        std::fprintf(stderr, "round: %s\n", err.c_str());
        return 2;
    }
    QueryExec qe(g, &err);
    uint64_t per_work = 0, echo_work = 0;
    double per_wall = 0, echo_wall = 0;
    std::vector<uint64_t> pw, ew;
    std::vector<double> pt, et;
    for (int rep = 0; rep < std::max(1, repeat); ++rep) {
        Counters pc;
        const double t0 = now_sec();
        for (const auto& q : r.queries) {
            CandidateResult res;
            Counters c;
            if (!qe.run(q, &res, &c)) {
                std::fprintf(stderr, "per-query %s\n", q.id.c_str());
                return 2;
            }
            if (std::getenv("V3_DEBUG_P3"))
                std::fprintf(stderr, "per %s work %llu edges %llu tuples %llu vl %llu\n", q.id.c_str(),
                             (unsigned long long)c.work(), (unsigned long long)c.edges_expanded,
                             (unsigned long long)c.tuples, (unsigned long long)c.vl_steps);
            pc.add(c);
        }
        const double t1 = now_sec();
        Executor::Options opt;
        opt.serial_only = true;
        opt.masked_agg = false;
        opt.enforce_budget = false;
        Executor ex(g, g.schema(), opt);
        std::vector<CandidateOut> out;
        Counters ec;
        RoundPlan plan;
        const double t2 = now_sec();
        if (!ex.run(r, &out, &ec, &plan, &err)) {
            std::fprintf(stderr, "serial run: %s\n", err.c_str());
            return 2;
        }
        const double t3 = now_sec();
        pw.push_back(pc.work());
        ew.push_back(ec.work());
        pt.push_back(t1 - t0);
        et.push_back(t3 - t2);
    }
    auto median = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    per_work = (uint64_t)median(std::vector<double>(pw.begin(), pw.end()));
    echo_work = (uint64_t)median(std::vector<double>(ew.begin(), ew.end()));
    per_wall = median(pt);
    echo_wall = median(et);
    const double wr = per_work ? (double)echo_work / (double)per_work : 1.0;
    const double tr = per_wall > 0 ? echo_wall / per_wall : 1.0;
    const bool ok = wr <= factor && tr <= factor;
    std::printf("{\"round\":\"%s\",\"candidates\":%zu,\"per_work\":%llu,\"echo_work\":%llu,"
                "\"work_ratio\":%.6f,\"per_sec\":%.6f,\"echo_sec\":%.6f,\"wall_ratio\":%.6f,"
                "\"factor\":%.3f,\"p3\":\"%s\"}\n",
                r.family.c_str(), r.queries.size(), (unsigned long long)per_work,
                (unsigned long long)echo_work, wr, per_wall, echo_wall, tr, factor,
                ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
