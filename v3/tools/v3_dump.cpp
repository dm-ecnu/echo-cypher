// v3_dump: the reference rows of a candidate for the result assertions.
//
//   v3_dump --sidecar DIR --round FILE --out DIR [--limit N]
//
// Writes <out>/ref-<cand>.jsonl, one row per line:
//   {"c":"<id>","labels":[...],"ncols":K,"rows":[["i",v] | ["s",text] | ["n"], ...],"nrows":R,
//    "enum":"q<idx>.jsonl", "enum_n": E, "work": W}
// The enumeration (every matching, as the values of the plan's variables) goes to
// <out>/q<idx>.jsonl so that the masked path can be compared per matching, not only per result.
#include <cstdio>
#include <fstream>
#include <string>

#include "v3_exec.h"
#include "v3_executor.h"
#include "v3_json.h"
#include "v3_plan.h"

using namespace v3;

int main(int argc, char** argv) {
    std::string sidecar, round_path, out;
    long long limit = -1;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--sidecar" && i + 1 < argc) sidecar = argv[++i];
        else if (a == "--round" && i + 1 < argc) round_path = argv[++i];
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--limit" && i + 1 < argc) limit = std::atoll(argv[++i]);
    }
    if (sidecar.empty() || round_path.empty() || out.empty()) {
        std::fprintf(stderr, "usage: v3_dump --sidecar DIR --round FILE --out DIR [--limit N]\n");
        return 2;
    }
    GraphStore g;
    std::string err;
    if (!g.load(sidecar, &err)) {
        std::fprintf(stderr, "load: %s\n", err.c_str());
        return 1;
    }
    Round r;
    if (!load_round(round_path, &r, &err)) {
        std::fprintf(stderr, "round: %s\n", err.c_str());
        return 1;
    }
    QueryExec qe(g, &err);
    for (size_t i = 0; i < r.queries.size(); ++i) {
        const Query& q = r.queries[i];
        Counters c;
        CandidateResult res;
        if (!qe.run(q, &res, &c)) {
            std::fprintf(stderr, "run %s\n", q.id.c_str());
            return 1;
        }
        std::vector<Env> envs;
        if (!qe.enumerate(q, &envs, nullptr, limit, nullptr, &err)) {
            std::fprintf(stderr, "enum %s: %s\n", q.id.c_str(), err.c_str());
            return 1;
        }
        std::string enum_path = out + "/q" + std::to_string(i) + ".jsonl";
        {
            std::ofstream ef(enum_path);
            for (const auto& e : envs) {
                ef << "[";
                for (size_t v = 0; v < e.v.size(); ++v) {
                    if (v) ef << ",";
                    ef << (e.v[v] == kNoVal ? -1 : (long long)e.v[v]);
                }
                ef << "]\n";
            }
        }
        std::ofstream f(out + "/ref-" + std::to_string(i) + ".jsonl");
        json::Writer w;
        w.raw("{\"c\":" + json::Writer::quote(q.id) + ",\"i\":" + std::to_string(i) + ",\"labels\":[");
        for (size_t k = 0; k < res.labels.size(); ++k) w.raw((k ? "," : "") + json::Writer::quote(res.labels[k]));
        w.raw("],\"ncols\":" + std::to_string(res.ncols) + ",\"rows\":[");
        const size_t nrows = res.ncols ? res.data.size() / res.ncols : 0;
        for (size_t row = 0; row < nrows; ++row) {
            if (row) w.raw(",");
            w.raw("[");
            for (uint32_t c2 = 0; c2 < res.ncols; ++c2) {
                const Value& x = res.data[row * res.ncols + c2];
                if (c2) w.raw(",");
                if (x.is_null) w.raw("[\"n\",0]");
                else if (x.is_str) w.raw("[\"s\"," + json::Writer::quote(x.s) + "]");
                else w.raw("[\"i\"," + std::to_string(x.i) + "]");
            }
            w.raw("]");
        }
        w.raw("],\"nrows\":" + std::to_string(nrows) + ",\"enum_n\":" + std::to_string(envs.size()) +
              ",\"work\":" + std::to_string(c.work()) + ",\"plan\":" + query_to_json(q) + "}\n");
        f << w.str();
    }
    return 0;
}
