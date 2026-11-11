// v3_check: the round's correctness assertion.
//
//   v3_check --sidecar DIR --round FILE [--base DIR] [--quiet]
//
// With --base, every candidate is also checked against the shared base's per-query reference
// (BaseQuery: the base's DPiso filter -> GQL order -> LFTJ over the candidate's v2-encoded
// view), and its result multiset must equal that one too.  Candidates the base path reports
// unsupported are counted, never skipped silently.
//
// For every candidate of the round:
//   1. the reference evaluation (QueryExec: per candidate, its own walk) gives the result rows;
//   2. the round runner (M1..M6: alignment, masked evaluation of the shared round, demultiplex,
//      per-candidate RETURN) gives its rows;
//   3. the two result multisets must be equal (row-wise, value-wise), and every matching of the
//      masked path must be a matching of the reference (per-matching digest subset).
// Exit status 0 when every candidate passes; 1 otherwise.  Prints one line per candidate.
#include <algorithm>
#include <cstdio>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "v3_base.h"
#include "v3_exec.h"
#include "v3_executor.h"
#include "v3_plan.h"
#include "v3_masc_ldbc.h"
#include "v3_point.h"

using namespace v3;

namespace {
// A matching key: vertex values as stored; a relationship variable through its property row,
// because the graph stores both directions of a relationship as two adjacency rows and the walk
// may take either copy (the base's semantics are that of the relationship, not of the row).
std::string match_key(const GraphStore& g, const Query& q, const std::vector<uint32_t>& m) {
    std::string s;
    s.reserve(m.size() * sizeof(uint32_t));
    for (size_t k = 0; k < m.size(); ++k) {
        uint32_t x = m[k];
        if (x != UINT32_MAX && k < q.vars.size() && q.vars[k].kind == PatternVar::EDGE)
            x = g.edge_row(x);
        s.append(reinterpret_cast<const char*>(&x), sizeof x);
    }
    return s;
}
std::string row_key(const std::vector<Value>& r) {
    std::string s;
    for (const auto& v : r) {
        const char tag = v.is_null ? 'n' : (v.is_str ? 's' : 'i');
        s.push_back(tag);
        s.append(reinterpret_cast<const char*>(&v.i), sizeof v.i);
        s.push_back('\1');
        s.append(v.s);
        s.push_back('\2');
    }
    return s;
}
}  // namespace

int main(int argc, char** argv) {
    std::string sidecar, round_path, base_dir;
    long long limit = -1;
    bool quiet = false;
    bool budget = false;
    bool no_masc = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--sidecar" && i + 1 < argc) sidecar = argv[++i];
        else if (a == "--round" && i + 1 < argc) round_path = argv[++i];
        else if (a == "--limit" && i + 1 < argc) limit = std::atoll(argv[++i]);
        else if (a == "--quiet") quiet = true;
        else if (a == "--budget") budget = true;
        else if (a == "--no-masc") no_masc = true;
        else if (a == "--base" && i + 1 < argc) base_dir = argv[++i];
    }
    if (sidecar.empty() || round_path.empty()) {
        std::fprintf(stderr, "usage: v3_check --sidecar DIR --round FILE [--limit N] [--quiet]\n");
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
    prepare_tiny_round(g, &r);
    prepare_point_round(g, &r);
    Executor::Options opt;
    opt.digests = true;
    opt.enforce_budget = budget;   // the check exercises the masked path unless --budget
    Executor ex(g, g.schema(), opt);
    std::vector<CandidateOut> out;
    Counters cnt;
    RoundPlan plan;
    if (!ex.run(r, &out, &cnt, &plan, &err)) {
        std::fprintf(stderr, "run: %s\n", err.c_str());
        return 2;
    }
    MascLdbcRound mr;
    if (!no_masc) mr = masc_ldbc_run(g, r);
    TargetDistCache tdc;
    QueryExec qe(g, &err, &tdc);
    int failed = 0, masc_failed = 0;
    for (size_t i = 0; i < r.queries.size(); ++i) {
        const Query& q = r.queries[i];
        Counters rc;
        CandidateResult ref;
        if (!qe.run(q, &ref, &rc)) {
            std::printf("%s FAIL reference error\n", q.id.c_str());
            ++failed;
            continue;
        }
        // An all-aggregating candidate returns one row per group; the per-matching record is
        // not compared for it, so materialising 10^8 matchings only to discard them is waste.
        const auto& cd = out[i];
        std::vector<Env> envs;
        if (!cd.aggregated && !qe.enumerate(q, &envs, nullptr, limit, nullptr, &err)) {
            std::printf("%s FAIL reference enumeration error\n", q.id.c_str());
            ++failed;
            continue;
        }
        // 1. result multiset
        const size_t nc = ref.ncols;
        std::map<std::string, long long> want, have;
        const size_t rn = nc ? ref.data.size() / nc : 0;
        for (size_t k = 0; k < rn; ++k)
            ++want[row_key(std::vector<Value>(ref.data.begin() + k * nc, ref.data.begin() + (k + 1) * nc))];
        const size_t hn = nc ? cd.data.size() / nc : 0;
        for (size_t k = 0; k < hn; ++k)
            ++have[row_key(std::vector<Value>(cd.data.begin() + k * nc, cd.data.begin() + (k + 1) * nc))];
        bool rows_ok = (want == have);
        if (!rows_ok && getenv("V3_DEBUG_ALIGN")) {
        size_t shown = 0;
        for (const auto& kv : want) {
            auto it = have.find(kv.first);
            const long long h = it == have.end() ? 0 : it->second;
            if (h != kv.second && shown++ < 3) {
                std::string txt;
                for (char ch : kv.first) txt += (ch >= 32 && ch < 127) ? ch : '.';
                std::fprintf(stderr, "DIFF ref %lld have %lld key %s\n", kv.second, h, txt.c_str());
            }
        }
    }
    // 2. matching digests: the masked and the reference matching multisets are equal (a masked
    //    matching that the reference does not have, and a reference matching the mask lost).
    //    The two lists are the same object only for a single-stage pattern: the reference
    //    enumeration stops at the first WITH, while a departed prefix member's list is the row
    //    set after the whole pipeline.  A WITH stage is therefore checked by its result rows
    //    (and by the round's solution count), not by this per-matching record.
        bool multi_stage = false;
        for (const auto& c : q.clauses)
            if (c.kind == ClauseKind::WITH) { multi_stage = true; break; }
        const bool compare_digests = cd.planned && !cd.aggregated && !multi_stage;
        std::map<std::string, long long> ref_d;
        for (const auto& e : envs) ref_d[match_key(g, q, e.v)]++;
        std::map<std::string, long long> masked_d;
        for (const auto& m : cd.matchings) masked_d[match_key(g, q, m)]++;
        size_t extra = 0, missing = 0;
        if (compare_digests) {
            for (const auto& kv : masked_d) {
                auto it = ref_d.find(kv.first);
                if (it == ref_d.end()) extra += (size_t)kv.second;
                else if (it->second < kv.second) extra += (size_t)(kv.second - it->second);
            }
            for (const auto& kv : ref_d) {
                auto it = masked_d.find(kv.first);
                if (it == masked_d.end()) missing += (size_t)kv.second;
                else if (it->second < kv.second) missing += (size_t)(kv.second - it->second);
            }
        }
        // a fallback candidate has no masked matching list (its reference evaluation is the only
        // one that ran); its rows are still compared above
        const bool digests_ok = !compare_digests || (extra == 0 && missing == 0);
        const bool ok = rows_ok && digests_ok;
        if (!ok) {
            ++failed;
            if (getenv("V3_DEBUG_MATCH")) {
                auto show = [&](const char* tag, const std::vector<uint32_t>& v) {
                    std::fprintf(stderr, "%s", tag);
                    for (uint32_t x : v) std::fprintf(stderr, " %u", x);
                    std::fprintf(stderr, "\n");
                };
                if (!envs.empty()) show("ref matching:", envs[0].v);
                if (!cd.matchings.empty()) show("masked matching:", cd.matchings[0]);
            }
        }
        if (!quiet || !ok) {
            std::string detail;
            if (!rows_ok) detail += " ROWS-DIFFER";
            if (compare_digests && extra) detail += " EXTRA-MATCHINGS " + std::to_string(extra);
            if (compare_digests && missing) detail += " MISSING-MATCHINGS " + std::to_string(missing);
            std::printf("%s %s nrows %zu masked %zu matchings %zu ref_matchings %zu%s\n", q.id.c_str(),
                        ok ? "OK" : "FAIL", rn, hn, cd.matchings.size(), envs.size(), detail.c_str());
        }
        // Same-contract MASC port: every covered candidate must return the same result multiset
        // and the same number of matchings as the serial reference.  Uncovered candidates are
        // evaluated by the same serial executor as the reference, so only covered ones compare.
        if (!no_masc && mr.per_query[i].covered) {
            const CandidateResult& mres = mr.per_query[i].result;
            const size_t mn = mres.ncols ? mres.data.size() / mres.ncols : 0;
            std::map<std::string, long long> mhave;
            for (size_t k = 0; k < mn; ++k)
                ++mhave[row_key(std::vector<Value>(mres.data.begin() + k * mres.ncols,
                                                   mres.data.begin() + (k + 1) * mres.ncols))];
            const bool mrows_ok = (want == mhave);
            const bool msol_ok = (mr.per_query[i].solutions == envs.size());
            if (!mrows_ok || !msol_ok) {
                ++masc_failed;
                std::printf("%s MASC FAIL%s%s nrows %zu masc_rows %zu masc_matchings %llu ref_matchings %zu\n",
                            q.id.c_str(), mrows_ok ? "" : " ROWS-DIFFER",
                            msol_ok ? "" : " MATCHINGS-DIFFER", rn, mn,
                            (unsigned long long)mr.per_query[i].solutions, envs.size());
            } else if (!quiet) {
                std::printf("%s MASC OK nrows %zu masc_matchings %llu\n", q.id.c_str(), mn,
                            (unsigned long long)mr.per_query[i].solutions);
            }
        }
    }
    if (getenv("V3_DEBUG_ALIGN")) {
        const Query& q0 = r.queries[0];
        CandidateResult own0;
        Counters c0;
        qe.run(q0, &own0, &c0);
        const size_t nc0 = own0.ncols;
        std::fprintf(stderr, "ref ncols %zu rows %zu\n", nc0, nc0 ? own0.data.size() / nc0 : 0);
        for (size_t rr = 0; rr < std::min<size_t>(3, nc0 ? own0.data.size() / nc0 : 0); ++rr) {
            std::fprintf(stderr, " ref row %zu:", rr);
            for (size_t c = 0; c < nc0; ++c) {
                const Value& x = own0.data[rr * nc0 + c];
                if (x.is_null) std::fprintf(stderr, " NULL");
                else if (x.is_str) std::fprintf(stderr, " '%s'", x.s.substr(0, 12).c_str());
                else std::fprintf(stderr, " %lld", (long long)x.i);
            }
            std::fprintf(stderr, "\n");
        }
    }
    // the shared base's per-query reference (when asked): the same result multisets
    int base_failed = 0, base_unsupported = 0;
    std::map<std::string, int> base_why;
    if (!base_dir.empty()) {
        for (size_t i = 0; i < r.queries.size(); ++i) {
            const Query& q = r.queries[i];
            const BaseQuery::Result b = BaseQuery::run(g, base_dir, q);
            if (!b.supported) {
                ++base_unsupported;
                ++base_why[b.why];
                continue;
            }
            CandidateResult ref;
            Counters rc;
            std::vector<Env> envs;
            if (!qe.run(q, &ref, &rc) || !qe.enumerate(q, &envs, nullptr, limit, nullptr, &err)) {
                std::printf("%s BASE FAIL reference error\n", q.id.c_str());
                ++base_failed;
                continue;
            }
            std::map<std::string, long long> want;
            const size_t nc = ref.ncols;
            const size_t rn = nc ? ref.data.size() / nc : 0;
            for (size_t k = 0; k < rn; ++k)
                ++want[row_key(std::vector<Value>(ref.data.begin() + k * nc, ref.data.begin() + (k + 1) * nc))];
            // the base's embeddings -> the candidate's result rows
            std::vector<Env> benv;
            for (const auto& e : b.embeds) {
                Env x;
                x.v.assign(e.begin(), e.end());
                benv.push_back(std::move(x));
            }
            CandidateResult bres;
            QueryExec::project_only(g, q, benv, &bres, true, true, true, -1, nullptr);
            std::map<std::string, long long> have;
            const size_t hn = nc ? bres.data.size() / nc : 0;
            for (size_t k = 0; k < hn; ++k)
                ++have[row_key(std::vector<Value>(bres.data.begin() + k * nc, bres.data.begin() + (k + 1) * nc))];
            const bool ok = want == have;
            if (getenv("V3_DEBUG_ALIGN") && b.embeds.size() < 4) {
                std::fprintf(stderr, "base embeds %zu ncols %zu\n", b.embeds.size(), nc);
                for (size_t r = 0; r < std::min<size_t>(rn, 3); ++r) {
                    std::fprintf(stderr, " ref row %zu:", r);
                    for (size_t c = 0; c < nc; ++c) {
                        const Value& x = ref.data[r * nc + c];
                        if (x.is_null) std::fprintf(stderr, " NULL");
                        else if (x.is_str) std::fprintf(stderr, " '%s'", x.s.substr(0, 12).c_str());
                        else std::fprintf(stderr, " %lld", (long long)x.i);
                    }
                    std::fprintf(stderr, "\n");
                }
            }
            if (!ok) ++base_failed;
            if (!quiet || !ok)
                std::printf("%s BASE %s ref_rows %zu base_rows %zu ref_matchings %zu base_matchings %zu\n",
                            q.id.c_str(), ok ? "OK" : "FAIL", rn, hn, envs.size(), b.embeds.size());
        }
    }
    size_t masc_covered = 0;
    for (const auto& x : mr.per_query) if (x.covered) ++masc_covered;
    std::printf("round %s: %zu candidates, %d failed, plan envelopes %zu fallback %zu replay %zu, work %llu, masc %zu covered %d failed\n",
                r.family.c_str(), r.queries.size(), failed,
                plan.depth_chain ? (size_t)1 : plan.envelopes.size(), plan.fallback.size(),
                (size_t)std::count_if(plan.replay.begin(), plan.replay.end(), [](int32_t x) { return x >= 0; }),
                (unsigned long long)cnt.work(), masc_covered, masc_failed);
    if (!base_dir.empty()) {
        std::printf("base reference: %d differ, %d unsupported", base_failed, base_unsupported);
        for (const auto& kv : base_why) std::printf(" %s=%d", kv.first.c_str(), kv.second);
        std::printf("\n");
    }
    return (failed || base_failed || masc_failed) ? 1 : 0;
}
