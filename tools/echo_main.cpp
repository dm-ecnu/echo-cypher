// ECHO batch runner: N1-N3 envelope assembly, N4-N5 masked walk, N6 fallback.
// --verify compares every member's embeddings with the per-query DPiso run (set equality).
#include <set>

#include "echo_common.h"
#include "echo_dpiso.h"
#include "echo_envelope.h"
#include "echo_search.h"
#include "echo_calibration.h"
#include <thread>

using namespace echo;

// Multiset equality of two embedding sets for one query; returns 0 or 1.
static int cmp_bags(const std::vector<std::vector<uint32_t>>& a,
                    const std::vector<std::vector<uint32_t>>& b) {
    auto bag = [](const std::vector<std::vector<uint32_t>>& v) {
        return std::multiset<std::vector<uint32_t>>(v.begin(), v.end());
    };
    return bag(a) == bag(b) ? 0 : 1;
}

int main(int argc, char** argv) {
    Args a = parse_args(argc, argv);
    std::string graph = a.get("graph");
    std::string wdir = a.get("workload");
    if (graph.empty() || wdir.empty()) {
        std::fprintf(stderr, "usage: echo_main --graph G --workload DIR [--batch B|--b0 I --bn N]"
                             " [--mode 0|1|2] [--min_core K] [--verify] [--repeat R] [--timeout SEC]\n");
        return 2;
    }
    Graph* data = load_data_graph(graph);
    Workload w = read_workload_manifest(wdir);
    int batch = (int)a.geti("batch", 0);
    int repeat = (int)a.geti("repeat", 2);
    bool verify = !a.get("verify").empty();
    double timeout = a.getd("timeout", 120.0);
    int b0 = (int)a.geti("b0", (int)w.batch_start(batch));
    int bn = (int)a.geti("bn", (int)w.batch_count(batch));
    double batch_deadline = a.getd("batch_deadline", 3600.0);
    int mode = (int)a.geti("mode", 0);
    uint32_t min_core = (uint32_t)a.geti("min_core", 2);
    Clock::time_point t_start = Clock::now();
    auto remain = [&]() {
        return batch_deadline - std::chrono::duration<double>(Clock::now() - t_start).count();
    };

    std::vector<QueryGraph> qs;
    for (int i = b0; i < b0 + bn; ++i) qs.push_back(w.read((uint32_t)i));

    std::vector<long long> label_freq;
    {
        uint32_t maxlab = 0;
        for (uint32_t v = 0; v < data->getVerticesCount(); ++v)
            maxlab = std::max<uint32_t>(maxlab, (uint32_t)data->getVertexLabel(v));
        label_freq.assign(maxlab + 2, 1);
        for (uint32_t v = 0; v < data->getVerticesCount(); ++v) ++label_freq[data->getVertexLabel(v)];
    }
    // N2/N3 planning is a short-lived worker. Running it on the main thread left the base
    // matcher's allocator/heap layout perturbed, and the first N6/per-query repetition paid
    // ~12% extra on DBLP o0 even though planning itself measured <1 ms. The worker has its own
    // malloc arena; the result is joined before the timed loop and is bit-identical.
    Stopwatch plan_sw;
    std::vector<std::vector<uint32_t>> fams;
    std::vector<uint64_t> input_counts;
    std::thread plan_thread([&]() {
        fams = families_of_min(qs, data, 0, 0);
        if (!w.query_batches.empty() || !a.get("counts").empty()) input_counts.assign(bn,0);
    });
    plan_thread.join();
    double family_sec = plan_sw.seconds();

    // Reference: the per-query pipeline, for the equivalence assertion only.
    std::vector<std::vector<std::vector<uint32_t>>> ref_sols;
    double ref_sec = 0;
    if (verify) {
        ref_sols.resize(qs.size());
        for (size_t i = 0; i < qs.size(); ++i) {
            Graph* qg = to_base_query(qs[i]);
            LfrtControl ctl;
            ctl.deadline = Clock::now() + std::chrono::milliseconds((long long)(timeout * 1000));
            PerQueryResult r = dpiso_run(data, qg, &ref_sols[i], &ctl);
            delete qg;
            ref_sec += r.filter_sec + r.order_sec + r.enum_sec;
        }
    }

    Stats st("echo");
    double plan_sum = 0, filter_sum = 0, search_sum = 0;
    long long total = 0, nodes = 0, mismatches = 0, envelopes = 0, covered = 0;
    double depart_sec = 0;
    long long depart_runs = 0, depart_members = 0, depart_depth_sum = 0, depart_depth_max = 0;
    long long walk_timeouts = 0;
    long long n3_merges = 0, n3_splits = 0;
    bool batch_expired = false;
    long long n4_time_fallbacks=0, n4_growth_fallbacks=0, n5_budget_fallbacks=0, n5_deadline_fallbacks=0, n6_queries=0;
    uint64_t candidate_sum_attempted=0, discarded_speculative_solutions=0;
    if (!(kN3Kappa > 0)) throw std::runtime_error("N3 calibration is not frozen");
    double attempt_filter_sec=0, attempt_walk_sec=0, attempt_wall_sec=0, attempt_budget_sec=0, n6_sec=0;
    double rep_last = 0;
    for (int rep = 0; rep < repeat; ++rep) {
        Stopwatch sw;
        for (auto& fam : fams) {
            // N6: a lone query, and the degraded path when no envelope can be assembled.
            // `need_sols` is false outside verification: the pipeline reports the embedding count,
            // and materialising every embedding would be work the per-query baseline does not do.
            auto run_n6 = [&](uint32_t q, bool need_sols) {
                double rem = std::min(timeout, remain());
                if (rem <= 0) { batch_expired = true; return; }
                ++n6_queries;
                Stopwatch n6_sw;
                Graph* qg = to_base_query(qs[q]);
                LfrtControl ctl;
                ctl.deadline = Clock::now() + std::chrono::milliseconds((long long)(rem * 1000));
                std::vector<std::vector<uint32_t>> sols;
                PerQueryResult pr = dpiso_run(data, qg, need_sols ? &sols : nullptr, &ctl);
                if (pr.timed_out) ++walk_timeouts;
                delete qg;
                filter_sum += pr.filter_sec + pr.order_sec;
                search_sum += pr.enum_sec;
                total += (long long)(need_sols ? sols.size() : pr.embeddings);
                if (!input_counts.empty()) input_counts[q] += pr.embeddings;
                if (need_sols && verify && rep == 0) mismatches += cmp_bags(ref_sols[q], sols);
                n6_sec += n6_sw.seconds();
            };
            const bool need_sols = verify && rep == 0;
            // Run one envelope over `grp` and fold its filter/search/departures into the cell.
            auto run_env_one = [&](const Envelope& e, const std::vector<uint32_t>& grp, long long candidate_est=0, long long separate_est=0) {
            if (grp.size() > 1) { ++envelopes; covered += (long long)grp.size(); }
            double rem = std::min(timeout, remain());
            if (rem <= 0) { batch_expired = true; return; }
            EnvLimits limits;
            const bool guarded = grp.size()>1 && candidate_est>0 && separate_est>0;
            if (guarded) { limits.max_candidates=2.0*candidate_est; limits.attempt_seconds=0.25*kN3Kappa*separate_est; }
            Stopwatch attempt_sw;
            EnvRun r = run_envelope(qs, data, e, {}, mode, nullptr,
                                    Clock::now() + std::chrono::milliseconds((long long)(rem * 1000)), limits);
            if (guarded) { attempt_filter_sec+=r.filter_sec; attempt_walk_sec+=r.search_sec; attempt_wall_sec+=attempt_sw.seconds(); attempt_budget_sec+=limits.attempt_seconds; }
            filter_sum += r.filter_sec;
            search_sum += r.search_sec;
            nodes += (long long)r.nodes;
            // A walk that hits its deadline loses the tail of the candidate space; report it as a
            // timeout so the cell is not recorded as a completed batch (see queries_done below).
            candidate_sum_attempted += r.cell_sum;
            discarded_speculative_solutions += r.discarded_solutions;
            auto fallback_family = [&]() {
                if (grp.size()>1) covered -= (long long)grp.size();
                for (uint32_t q : grp) run_n6(q, need_sols);
            };
            if (guarded && (r.candidate_exceeded || r.budget_exceeded || r.timed_out)) {
                if (r.candidate_exceeded) ++n4_growth_fallbacks;
                else if (r.filter_budget_exceeded) ++n4_time_fallbacks;
                else if (r.budget_exceeded) ++n5_budget_fallbacks;
                else ++n5_deadline_fallbacks;
                fallback_family(); return;
            }
            if (r.timed_out) ++walk_timeouts;
            // A member that left the walk is completed from each recorded prefix by the base
            // pipeline with fixed candidates: the prefix pins those query vertices to their data
            // vertices, the base filters and enumerates the rest. No full enumeration, no
            // hand-written recursion.
            depart_members += (long long)r.depart.size();
            for (auto& dep : r.depart) {
                int m = dep.first;
                if (batch_expired) break;
                auto& pres = dep.second;
                if (pres.empty()) continue;
                // Few prefixes: pin each and complete. Many: one unpinned run, then filter (the
                // per-prefix filter would dominate and the result is the same set).
                // Deep departures leave 0-1 query vertices free. Finish those from the base's
                // own filtered candidate lists: one bounded scan of the free vertex's
                // candidates, no per-prefix edge-table build. Anything larger (rare) still goes
                // through the base enumerator with fixed candidates.
                {
                    bool all_shallow = true;
                    for (auto& pre : pres) if ((size_t)qs[grp[m]].n > pre.size() + 1) all_shallow = false;
                    if (all_shallow) {
                        Graph* qg = to_base_query(qs[grp[m]]);
                        std::vector<std::vector<uint32_t>> cand;
                        Stopwatch fw;
                        bool okc = dpiso_candidates(data, qg, &cand);
                        filter_sum += fw.seconds();
                        if (okc) {
                            const QueryGraph& g = qs[grp[m]];
                            std::vector<std::vector<uint32_t>> acc;
                            for (auto& pre : pres) {
                                                std::vector<char> pinned(g.n, 0);
                                std::vector<uint32_t> val(g.n, 0xFFFFFFFFu);
                                for (auto& pv : pre) { pinned[pv.first] = 1; val[pv.first] = pv.second; }
                                // N6 input adapter: a no-mask departure may carry a prefix
                                // that violates this member's predicate. Apply the same
                                // domain intersection as the full pinned base entry.
                                bool input_ok = true;
                                if (g.domain_id) for (auto& pv : pre)
                                    if (!g.permits(pv.first, pv.second)) { input_ok = false; break; }
                                if (!input_ok) continue;
                                std::vector<int> free_v;
                                for (uint32_t v = 0; v < g.n; ++v) if (!pinned[v]) free_v.push_back((int)v);
                                ++depart_runs;
                                depart_depth_sum += (long long)pre.size();
                                depart_depth_max = std::max<long long>(depart_depth_max, (long long)pre.size());
                                if (free_v.empty()) {
                                    bool ok2 = true;
                                    for (uint32_t u = 0; u < g.n && ok2; ++u)
                                        for (uint32_t w : g.adj[u]) {
                                            if (w < u) continue;
                                            if (!data->checkEdgeExistence(val[u], val[w])) { ok2 = false; break; }
                                        }
                                    if (ok2) acc.push_back(val);
                                    continue;
                                }
                                uint32_t u = (uint32_t)free_v[0];
                                for (uint32_t c : cand[u]) {
                                                        bool ok2 = true;
                                    for (uint32_t w : g.adj[u]) {
                                        uint32_t x = val[w];
                                        if (x == 0xFFFFFFFFu) continue;
                                        if (c == x || !data->checkEdgeExistence(c, x)) { ok2 = false; break; }
                                    }
                                    if (!ok2) continue;
                                    std::vector<uint32_t> sol = val;
                                    sol[u] = c;
                                    acc.push_back(std::move(sol));
                                }
                            }
                            r.per_query[m] = std::move(acc);
                            delete qg;
                            continue;
                        }
                        delete qg;
                    }
                }
                if (pres.size() <= 32) {
                    std::vector<std::vector<std::pair<int, uint32_t>>> pins_list;
                    pins_list.reserve(pres.size());
                    for (auto& pre : pres) {
                        std::vector<std::pair<int, uint32_t>> pins;
                        pins.reserve(pre.size());
                        for (auto& pv : pre) pins.emplace_back((int)pv.first, pv.second);
                        depart_depth_sum += (long long)pins.size();
                        depart_depth_max = std::max<long long>(depart_depth_max, (long long)pins.size());
                        pins_list.push_back(std::move(pins));
                    }
                    double rem2 = std::min(timeout, remain());
                    if (rem2 <= 0) { batch_expired = true; break; }
                    Graph* qg = to_base_query(qs[grp[m]]);
                    LfrtControl ctl;
                    ctl.deadline = Clock::now() + std::chrono::milliseconds((long long)(rem2 * 1000));
                    std::vector<std::vector<std::vector<uint32_t>>> got;
                    PerQueryResult pr = dpiso_run_pinned_multi(data, qg, pins_list, &got, &ctl);
                    if (pr.timed_out) ++walk_timeouts;
                    delete qg;
                    filter_sum += pr.filter_sec;
                    search_sum += pr.order_sec + pr.enum_sec;
                    depart_sec += pr.filter_sec + pr.order_sec + pr.enum_sec;
                    depart_runs += (long long)pins_list.size();
                    std::vector<std::vector<uint32_t>> acc;
                    for (auto& per_prefix : got)
                        for (auto& g : per_prefix) acc.push_back(std::move(g));
                    r.per_query[m] = std::move(acc);
                } else {
                    double rem2 = std::min(timeout, remain());
                    if (rem2 <= 0) { batch_expired = true; break; }
                    Graph* qg = to_base_query(qs[grp[m]]);
                    LfrtControl ctl;
                    ctl.deadline = Clock::now() + std::chrono::milliseconds((long long)(rem2 * 1000));
                    std::vector<std::vector<uint32_t>> full;
                    PerQueryResult pr = dpiso_run(data, qg, &full, &ctl);
                    if (pr.timed_out) ++walk_timeouts;
                    delete qg;
                    filter_sum += pr.filter_sec + pr.order_sec;
                    search_sum += pr.enum_sec;
                    depart_sec += pr.filter_sec + pr.order_sec + pr.enum_sec;
                    ++depart_runs;
                    std::vector<std::vector<uint32_t>> keep;
                    for (auto& f : full) {
                        bool ok = false;
                        for (auto& pre : pres) {
                            bool agree = true;
                            for (auto& pv : pre)
                                if (f[pv.first] != pv.second) { agree = false; break; }
                            if (agree) { ok = true; break; }
                        }
                        if (ok) keep.push_back(std::move(f));
                    }
                    r.per_query[m] = std::move(keep);
                }
            }
            for (auto& v : r.per_query) total += (long long)v.size();
            if (!input_counts.empty()) for (size_t m=0; m<grp.size(); ++m) input_counts[grp[m]] += r.per_query[m].size();
            if (verify && rep == 0)
                for (size_t m = 0; m < grp.size(); ++m) {
                    int bad = cmp_bags(ref_sols[grp[m]], r.per_query[m]);
                    mismatches += bad;
                    if (bad && !a.get("dump_mismatch").empty()) {
                        std::multiset<std::vector<uint32_t>> rr(ref_sols[grp[m]].begin(), ref_sols[grp[m]].end()),
                                                      ee(r.per_query[m].begin(), r.per_query[m].end());
                        std::vector<std::vector<uint32_t>> onlyE, onlyR;
                        std::set_difference(ee.begin(), ee.end(), rr.begin(), rr.end(), std::back_inserter(onlyE));
                        std::set_difference(rr.begin(), rr.end(), ee.begin(), ee.end(), std::back_inserter(onlyR));
                        std::fprintf(stderr, "MISM q%u onlyE=%zu onlyR=%zu\n", grp[m], onlyE.size(), onlyR.size());
                        for (size_t k = 0; k < onlyE.size() && k < 2; ++k) {
                            std::fprintf(stderr, "  E:");
                            for (uint32_t x : onlyE[k]) std::fprintf(stderr, " %u", x);
                            std::fprintf(stderr, "\n");
                        }
                        for (size_t k = 0; k < onlyR.size() && k < 2; ++k) {
                            std::fprintf(stderr, "  R:");
                            for (uint32_t x : onlyR[k]) std::fprintf(stderr, " %u", x);
                            std::fprintf(stderr, "\n");
                        }
                    }
                }
            };
            // No sharing: each member gets its own envelope through the same N4/N5 code.
            // This is the E5 no-envelope control and what N3 falls back to when a merge is
            // not estimated cheaper than the members apart.
            auto run_split = [&](const std::vector<uint32_t>& grp) {
                for (uint32_t q : grp) {
                    if (batch_expired) return;
                    Envelope e1 = make_single_envelope(qs, q);
                    run_env_one(e1, {q});
                }
            };
            if (fam.size() == 1) { run_n6(fam[0], need_sols); continue; }
            Stopwatch pw;
            Envelope e = make_core_envelope(qs, fam, data, min_core);
            plan_sum += pw.seconds();
            if (e.n == 0) {  // no shared core: each member falls back to its own pipeline
                for (uint32_t q : fam) run_n6(q, need_sols);
                if (batch_expired) break;
                continue;
            }
            // N3 cost estimate (paper form): the work an envelope implies is the sum over its
            // classes of the class domain, and a class domain is one label-frequency term per
            // distinct label it holds (a member whose label differs is masked out of that term).
            // Keeping the members apart costs the same sum over their own vertices. Merge only
            // when est(group) < sum of est({q}) -- at overlap 0 the labels at a class are all
            // different, the two sides are equal, and the group is not formed.
            long long est_merged = 0;
            for (uint32_t c = 0; c < e.n; ++c) {
                // A class keeps one candidate term per distinct (label, degree) it holds: the
                // base filter keys its candidate lists on that pair, so two members with the same
                // label but different degree still need separate terms.
                std::vector<std::pair<uint32_t, uint32_t>> terms;
                for (auto& code : e.span_v[c]) {
                    uint32_t t = code >> 16, v = code & 0xFFFF;
                    terms.emplace_back(qs[fam[t]].label[v], (uint32_t)qs[fam[t]].adj[v].size());
                }
                std::sort(terms.begin(), terms.end());
                terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
                for (auto& tm : terms) est_merged += (long long)label_freq[tm.first];
            }
            long long est_split = 0;
            std::vector<long long> est_one;
            est_one.reserve(fam.size());
            for (uint32_t q : fam) {
                long long e1 = 0;
                for (uint32_t v = 0; v < qs[q].n; ++v) e1 += (long long)label_freq[qs[q].label[v]];
                est_one.push_back(e1);
                est_split += e1;
            }
            // A member that owns a class of its own (a vertex the alignment left unmatched) will
            // leave the walk there and be enumerated by the base pipeline once. That cost belongs
            // to the merged estimate: without it a family with a few incidental shared classes
            // would look cheap although every member is enumerated twice.
            {
                std::vector<char> private_member(fam.size(), 0);
                for (uint32_t c = 0; c < e.n; ++c)
                    if (e.span_v[c].size() == 1) private_member[e.span_v[c][0] >> 16] = 1;
                for (size_t t = 0; t < fam.size(); ++t)
                    if (private_member[t]) est_merged += est_one[t];
            }
            if (getenv("ECHO_N3_DIAG")) {
                // Per-family calibration record: the paper-form estimate next to the two measured
                // alternatives (one merged envelope, and one envelope per member). Runs both, so
                // the cell totals are meaningless here; only the printed per-family deltas count.
                int mixed = 0, maxset = 1;
                long long cellest = 0;
                for (uint32_t c = 0; c < e.n; ++c) {
                    std::vector<uint32_t> labs;
                    for (auto& code : e.span_v[c])
                        labs.push_back(qs[fam[code >> 16]].label[code & 0xFFFF]);
                    std::sort(labs.begin(), labs.end());
                    labs.erase(std::unique(labs.begin(), labs.end()), labs.end());
                    if (labs.size() > 1) { ++mixed; maxset = std::max<int>(maxset, (int)labs.size()); }
                }
                auto env_run = [&](const Envelope& ee) {
                    double rem = std::min(timeout, remain());
                    return run_envelope(qs, data, ee, {}, 0, nullptr,
                                        Clock::now() + std::chrono::milliseconds((long long)(rem * 1000)));
                };
                EnvRun rm = env_run(e);
                double sf = 0, ss = 0;
                long long sn = 0;
                for (uint32_t q : fam) {
                    Envelope e1 = make_single_envelope(qs, q);
                    EnvRun rs = env_run(e1);
                    sf += rs.filter_sec;
                    ss += rs.search_sec;
                    sn += (long long)rs.nodes;
                }
                std::fprintf(stderr,
                             "N3 q=%u k=%zu npos=%u mixed=%d maxset=%d cell_sum=%llu cell_max=%u "
                             "mem_cell_sum=%llu kills=%llu est_m=%lld est_s=%lld | "
                             "M f=%.4f s=%.4f n=%lld sol=%llu | S f=%.4f s=%.4f n=%lld\n",
                             fam[0], fam.size(), e.n, mixed, maxset, (unsigned long long)rm.cell_sum,
                             rm.cell_max, (unsigned long long)rm.member_cell_sum,
                             (unsigned long long)rm.mask_kills, est_merged, est_split,
                             rm.filter_sec, rm.search_sec, (long long)rm.nodes,
                             (unsigned long long)rm.solutions, sf, ss, sn);
                (void)cellest;
                continue;
            }
            // A margin, not a bare comparison: an incidental single (label, degree) collision
            // between two unrelated queries would otherwise be enough to merge the whole family.
            // N3 calibrated estimate: the class-candidate sum above is the filter term, in units of
            // label-frequency entries. The walk term is measured on the three graphs: a merged
            // envelope's walk costs about 3 ms, i.e. ~60k label-frequency units (the median over
            // the o9/o12 calibration families, ECHO_N3_DIAG records). Merging has to save more
            // filter work than the walk it adds, otherwise the members stay apart.
            const long long walk_units = (long long)kN3WalkUnits;
            // N3 split (and every singleton family above) runs N6: the base DPiso matcher on each
            // query, exactly the perquery path. The per-member envelope is only the E5 control.
            if (mode == kNoEnvelope) {
                ++n3_splits;
                run_split(fam);
                if (batch_expired) break;
                continue;
            }
            if (est_merged + walk_units >= est_split) {
                ++n3_splits;
                for (uint32_t q : fam) run_n6(q, need_sols);
                if (batch_expired) break;
                continue;
            }
            ++n3_merges;
            run_env_one(e, fam, est_merged, est_split);
        }
        rep_last = sw.seconds();
        st.set("repeat_sec", rep_last);
        if (batch_expired) break;
    }
    st.set("family_sec", family_sec);
    st.set("plan_total_sec", plan_sum);
    st.set("ref_sec", ref_sec);
    for (size_t i=0; i<input_counts.size(); ++i) {
        st.seti("query_index", b0+i); st.seti("embeddings", input_counts[i]); st.sets("record_end", "1");
    }
    st.sets("tau_rule", "ceil(0.7*max(ns,nq))");
    st.set("n3_kappa",kN3Kappa); st.seti("n3_walk_units",kN3WalkUnits);
    st.set("attempt_filter_sec",attempt_filter_sec); st.set("attempt_walk_sec",attempt_walk_sec);
    st.set("attempt_wall_sec",attempt_wall_sec); st.set("attempt_budget_sec",attempt_budget_sec); st.set("n6_sec",n6_sec);
    st.set("n3_rho",0.25); st.seti("n4_time_fallbacks",n4_time_fallbacks);
    st.seti("n4_growth_fallbacks",n4_growth_fallbacks); st.seti("n5_budget_fallbacks",n5_budget_fallbacks);
    st.seti("n5_deadline_fallbacks",n5_deadline_fallbacks);st.seti("n6_queries",n6_queries);
    st.seti("candidate_sum_attempted",candidate_sum_attempted);
    st.seti("discarded_speculative_solutions",discarded_speculative_solutions);
    st.seti("envelopes", envelopes);
    st.seti("covered_queries", covered);
    st.seti("queries_total", bn);
    st.seti("solutions", total);
    st.seti("nodes", nodes);
    st.set("filter_sec", filter_sum / std::max(1, repeat));
    st.set("search_sec", search_sum / std::max(1, repeat));
    st.seti("peak_rss_kb", peak_rss_kb());
    st.seti("mismatches", mismatches);
    st.set("depart_sec", depart_sec);
    st.seti("depart_runs", depart_runs);
    st.seti("depart_members", depart_members);
    st.seti("depart_depth_sum", depart_depth_sum);
    st.seti("depart_depth_max", depart_depth_max);
    st.seti("n3_merges", n3_merges);
    st.seti("n3_splits", n3_splits);
    st.seti("queries_done", (batch_expired || walk_timeouts) ? 0 : bn);
    st.seti("timeouts", (batch_expired ? 1 : 0) + walk_timeouts);
    st.set("batch_sec", rep_last);
    st.flush();
    return 0;
}
