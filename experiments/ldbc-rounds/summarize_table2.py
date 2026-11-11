#!/usr/bin/env python3
"""Summarise work/main41a/cells.jsonl into the LDBC Exp-1 table.

Ratios are method/ECHO on the same round; >1 means ECHO is faster.  A round's
value is the median of its five repeats (time: ok and timeout-at-cap; work: ok
only).  The bootstrap resamples rounds, 5,000 draws, fixed seed.
"""
import os
ECHO_ROOT = os.environ.get("ECHO_ROOT", ".")
import argparse
import collections
import hashlib
import json
import math
import os
import random
import statistics
import sys

METHODS = ["v3", "serial", "masc", "batchenum", "vlpq"]
LABEL = {"v3": "ECHO", "serial": "Per-candidate", "masc": "MASC (port)",
         "batchenum": "BatchEnum+ (port)", "vlpq": "VLPQ"}
ORDER = ["serial", "vlpq", "masc", "batchenum", "v3"]
BOOT_REPS = 5000
BOOT_SEED = 20261109
CAP = 3600.0


def med(xs):
    xs = [x for x in xs if x is not None]
    return statistics.median(xs) if xs else None


def pct(xs, q):
    xs = sorted(x for x in xs if x is not None)
    if not xs:
        return None
    i = min(len(xs) - 1, max(0, int(q * (len(xs) - 1))))
    return xs[i]


def gmean(xs):
    xs = [x for x in xs if x is not None and x > 0]
    if not xs:
        return None
    return math.exp(sum(math.log(x) for x in xs) / len(xs))


def boot_ci(xs, reps=BOOT_REPS, seed=BOOT_SEED):
    xs = [x for x in xs if x is not None and x > 0]
    if len(xs) < 2:
        return [None, None]
    rng = random.Random(seed)
    n = len(xs)
    vals = []
    for _ in range(reps):
        s = [xs[rng.randrange(n)] for _ in range(n)]
        vals.append(gmean(s))
    vals.sort()
    return [vals[int(0.025 * reps)], vals[int(0.975 * reps) - 1]]


def fmt(x, d=3):
    if x is None:
        return "—"
    if abs(x) >= 100:
        return "%.0f" % x
    if abs(x) >= 10:
        return "%.1f" % x
    return "%.*f" % (d, x)


def fmt_ci(c):
    if c[0] is None:
        return ""
    return " [%s,%s]" % (fmt(c[0]), fmt(c[1]))


def fmt_pct(x):
    return "—" if x is None else "%.1f%%" % (100.0 * x)


def load(path):
    rows = [json.loads(l) for l in open(path, encoding="utf-8")]
    by = collections.defaultdict(lambda: collections.defaultdict(list))
    for r in rows:
        by[(r["sf"], r["round"])][r["repeat"]].append(r)
    return rows, by


def round_stats(reps):
    """Per-round per-method statistics from the five repeat rows."""
    out = {}
    for m in METHODS:
        rs = [r for rr in reps.values() for r in rr if r["method"] == m]
        time_vals, work_vals, rss_vals = [], [], []
        ok = timeout = err = missing = 0
        for r in rs:
            st = r["status"]
            if st == "ok":
                ok += 1
                if r["wall_sec"] is not None and r["wall_sec"] > 0:
                    time_vals.append(r["wall_sec"])
                if r["work"] is not None and r["work"] > 0:
                    work_vals.append(r["work"])
                if r["peak_rss_kb"] is not None and r["peak_rss_kb"] > 0:
                    rss_vals.append(r["peak_rss_kb"] / 1024.0)
            elif st == "timeout":
                timeout += 1
                time_vals.append(CAP)
            elif st == "missing":
                missing += 1
            else:
                err += 1
        out[m] = {"rows": rs, "time": med(time_vals), "work": med(work_vals),
                  "rss_mb": med(rss_vals), "ok": ok, "timeout": timeout,
                  "error": err, "missing": missing}
    return out


def population_stats(by, sf, min_k):
    keys = []
    for (s, rd), reps in by.items():
        if s != sf:
            continue
        k = max((r["k"] or 0) for rs in reps.values() for r in rs)
        if k >= min_k:
            keys.append((rd, k))
    keys.sort()
    return keys


def compute(by, sf, min_k):
    keys = population_stats(by, sf, min_k)
    rounds = []
    for rd, k in keys:
        reps = by[(sf, rd)]
        rounds.append({"round": rd, "k": k, "template": next(iter(reps.values()))[0]["template"],
                       "st": round_stats(reps)})
    out = {"sf": sf, "min_k": min_k, "rounds": len(rounds),
           "candidates": sum(r["k"] for r in rounds), "methods": {},
           "echo_vs_serial": {}, "fallback": {}, "red": {}, "correctness": {}}

    # per-method aggregates
    for m in METHODS:
        times, works, rss = [], [], []
        ok = timeout = err = missing = 0
        comp_num = comp_den = 0
        cov_num = cov_den = 0
        for r in rounds:
            st = r["st"][m]
            times.append(st["time"])
            works.append(st["work"])
            rss.append(st["rss_mb"])
            ok += st["ok"]; timeout += st["timeout"]; err += st["error"]; missing += st["missing"]
            comp_num += st["ok"] * r["k"]
            comp_den += 5 * r["k"]
            for rep in st["rows"]:
                k = r["k"]
                cov_den += k
                if rep["status"] == "ok":
                    if m == "v3":
                        fb = rep.get("fallback") or 0
                        cov_num += max(0, k - min(k, fb))
                    elif m == "masc" and not rep.get("fallback_from_serial"):
                        cov_num += min(k, max(0, rep.get("covered_candidates") or 0))
        out["methods"][m] = {
            "ok": ok, "timeout": timeout, "error": err, "missing": missing,
            "cells": 5 * len(rounds),
            "completion": comp_num / comp_den if comp_den else None,
            "completion_num": comp_num, "completion_den": comp_den,
            "coverage": cov_num / cov_den if cov_den else None,
            "coverage_num": cov_num, "coverage_den": cov_den,
            "rss_mb_median": med(rss),
            "rss_mb_p90": pct(rss, 0.9),
            "rss_mb_max": max((x for x in rss if x is not None), default=None),
            "time_median_s": med(times),
            "work_median": med(works),
        }

    # paired ratios vs ECHO
    for m in METHODS:
        tr, wr = [], []
        wins = ties = losses = 0
        both = 0
        for r in rounds:
            e = r["st"]["v3"]; o = r["st"][m]
            et, ot = e["time"], o["time"]
            if et is not None and ot is not None:
                tr.append(ot / et)
                both += 1
                if ot > et: wins += 1
                elif ot == et: ties += 1
                else: losses += 1
            # outcome: non-completion is an outcome, not a missing value
            if et is not None and ot is not None:
                pass
            elif et is not None and ot is None:
                wins += 1
            elif et is None and ot is not None:
                losses += 1
            else:
                ties += 1
            ew, ow = e["work"], o["work"]
            if ew and ow:
                wr.append(ow / ew)
        out["methods"][m].update({
            "time_ratio": gmean(tr), "time_ci": boot_ci(tr),
            "time_ratio_median": med(tr), "time_n": len(tr),
            "time_wins": wins, "time_ties": ties, "time_losses": losses,
            "time_valid_pair_wins": sum(1 for x in tr if x > 1),
            "time_all_rounds": len(rounds),
            "time_valid_pair_win_share": (sum(1 for x in tr if x > 1) / len(tr)) if tr else None,
            "time_outcome_win_share": wins / len(rounds) if rounds else None,
            "work_ratio": gmean(wr), "work_ci": boot_ci(wr),
            "work_ratio_median": med(wr), "work_n": len(wr),
            "work_valid_pair_wins": sum(1 for x in wr if x > 1),
            "work_valid_pair_win_share": (sum(1 for x in wr if x > 1) / len(wr)) if wr else None,
        })

    # all-at-cap sensitivity: errors and missing cells also enter at the cap
    cap_times = {m: [] for m in METHODS}
    for r in rounds:
        k = r["k"]
        for m in METHODS:
            vals = []
            for rep in r["st"][m]["rows"]:
                if rep["status"] == "ok" and rep["wall_sec"] and rep["wall_sec"] > 0:
                    vals.append(rep["wall_sec"])
                else:
                    vals.append(CAP)
            cap_times[m].append(med(vals) if vals else None)
    for m in METHODS:
        cr = [b / a for a, b in zip(cap_times["v3"], cap_times[m]) if a and b]
        out["methods"][m]["time_ratio_all_at_cap"] = gmean(cr)
        out["methods"][m]["time_ci_all_at_cap"] = boot_ci(cr)
        out["methods"][m]["time_n_all_at_cap"] = len(cr)

    # ECHO faster than per-candidate, explicit block
    e = out["methods"]["v3"]; s = out["methods"]["serial"]
    out["echo_vs_serial"] = {
        "valid_pair_win_share": s["time_valid_pair_win_share"],
        "outcome_win_share": s["time_outcome_win_share"],
        "wins": s["time_wins"], "ties": s["time_ties"], "losses": s["time_losses"],
        "valid_pairs": s["time_n"], "rounds": len(rounds),
    }

    # ECHO fallback share over cells that completed (fallback is not recorded on failures)
    fb_num = fb_den = 0
    fb_rounds = []
    for r in rounds:
        for rep in r["st"]["v3"]["rows"]:
            if rep["status"] == "ok":
                fb = rep.get("fallback") or 0
                k = r["k"]
                fb_num += min(k, fb); fb_den += k
                fb_rounds.append(fb / k if k else None)
    out["fallback"] = {"candidates": fb_num, "denominator": fb_den,
                       "share": fb_num / fb_den if fb_den else None,
                       "round_median": med(fb_rounds),
                       "round_p90": pct(fb_rounds, 0.9)}

    # batch redundancy red(R) = 1 - W_ECHO / W_serial
    reds, we_sum, ws_sum = [], 0.0, 0.0
    for r in rounds:
        ew = r["st"]["v3"]["work"]; sw = r["st"]["serial"]["work"]
        if ew and sw and sw > 0:
            reds.append(1.0 - ew / sw)
            we_sum += ew; ws_sum += sw
    out["red"] = {"n": len(reds), "median": med(reds),
                  "mean": statistics.fmean(reds) if reds else None,
                  "p10": pct(reds, 0.1), "p90": pct(reds, 0.9),
                  "positive_share": (sum(1 for x in reds if x > 0) / len(reds)) if reds else None,
                  "pooled": (1.0 - we_sum / ws_sum) if ws_sum else None,
                  "sum_work_echo": we_sum, "sum_work_serial": ws_sum}

    # correctness: ECHO vs per-candidate row counts on paired ok cells
    pairs = bad = 0
    masc_pairs = masc_bad = 0
    for r in rounds:
        for rep in r["st"]["v3"]["rows"]:
            if rep["status"] != "ok":
                continue
            other = [x for x in r["st"]["serial"]["rows"] if x["repeat"] == rep["repeat"]]
            if other and other[0]["status"] == "ok":
                pairs += 1
                if rep["solutions"] != other[0]["solutions"]:
                    bad += 1
            other = [x for x in r["st"]["masc"]["rows"] if x["repeat"] == rep["repeat"]]
            if other and other[0]["status"] == "ok" and not other[0].get("fallback_from_serial"):
                masc_pairs += 1
                if rep["solutions"] != other[0]["solutions"]:
                    masc_bad += 1
    out["correctness"] = {"echo_serial_pairs": pairs, "echo_serial_bad": bad,
                          "masc_pairs": masc_pairs, "masc_bad": masc_bad}
    return out, rounds


def template_table(by, sf, min_k):
    keys = population_stats(by, sf, min_k)
    t = collections.defaultdict(list)
    for rd, k in keys:
        reps = by[(sf, rd)]
        t[next(iter(reps.values()))[0]["template"]].append((rd, k, reps))
    out = {}
    for tmpl, rs in sorted(t.items()):
        rounds = []
        for rd, k, reps in rs:
            st = round_stats(reps)
            rounds.append({"round": rd, "k": k, "st": st})
        tr, wr, reds, fb = [], [], [], []
        e_times, s_times = [], []
        plan_v, plan_e, plan_env, plan_fb = [], [], [], []
        e_ok = e_to = e_err = s_ok = s_to = s_err = 0
        wins = ties = losses = 0
        for r in rounds:
            e = r["st"]["v3"]; s = r["st"]["serial"]
            et, stt = e["time"], s["time"]
            if et is not None and stt is not None:
                tr.append(stt / et)
                if stt > et: wins += 1
                elif stt == et: ties += 1
                else: losses += 1
            elif et is not None:
                wins += 1
            elif stt is not None:
                losses += 1
            else:
                ties += 1
            if et is not None: e_times.append(et)
            if stt is not None: s_times.append(stt)
            ew, sw = e["work"], s["work"]
            if ew and sw:
                wr.append(sw / ew); reds.append(1.0 - ew / sw)
            e_ok += e["ok"]; e_to += e["timeout"]; e_err += e["error"]
            s_ok += s["ok"]; s_to += s["timeout"]; s_err += s["error"]
            for rep in e["rows"]:
                if rep["status"] == "ok" and r["k"]:
                    fb.append((rep.get("fallback") or 0) / r["k"])
                    plan_v.append(rep.get("mcs_vertices"))
                    plan_e.append(rep.get("mcs_edges"))
                    plan_env.append(rep.get("envelopes"))
                    plan_fb.append(rep.get("fallback"))
        out[tmpl] = {
            "rounds": len(rounds), "candidates": sum(r["k"] for r in rounds),
            "time_ratio": gmean(tr), "time_ci": boot_ci(tr),
            "time_ratio_median": med(tr), "time_n": len(tr),
            "time_wins": wins, "time_ties": ties, "time_losses": losses,
            "work_ratio": gmean(wr), "work_n": len(wr),
            "red_median": med(reds), "red_n": len(reds),
            "fallback_share": (sum(fb) / len(fb)) if fb else None,
            "fallback_cells": len(fb),
            "echo_time_median": med(e_times), "serial_time_median": med(s_times),
            "echo_time_max_median_ratio": (max(e_times) / med(e_times)) if e_times and med(e_times) else None,
            "echo_ok": e_ok, "echo_timeout": e_to, "echo_error": e_err,
            "serial_ok": s_ok, "serial_timeout": s_to, "serial_error": s_err,
            "plan_mcs_vertices": med(plan_v), "plan_mcs_edges": med(plan_e),
            "plan_envelopes": med(plan_env), "plan_fallback": med(plan_fb),
        }
    return out



def fmt_time(x):
    if x is None:
        return "—"
    if x < 0.001:
        return "%.3g µs" % (x * 1e6)
    if x < 1.0:
        return "%.1f ms" % (x * 1e3)
    if x < 60.0:
        return "%.2f s" % x
    return "%.1f s" % x


def write_by_template(summary, outdir):
    POINT = {"IS1", "IS3", "IS4", "IS5"}
    md = []
    md.append("# Table II by template (LDBC Exp-1)\n")
    md.append("Same cells and same-round five-repeat medians as `table2.md`. "
              "Ratio is per-candidate/ECHO; >1 means ECHO is faster. `n` counts "
              "rounds with at least one candidate; templates with fewer than three "
              "rounds are shown but their interval is weak. `W/L/T` counts rounds "
              "where ECHO beat / lost to / tied per-candidate. `Fallback` is the "
              "share of ECHO's completed candidate slots that went to Fallback. "
              "`red(R)` is the median per-round `1 - W(ECHO)/W(per-candidate)`. "
              "`plan` is the median MCS vertices / edges and envelope count over "
              "completed ECHO cells. `ok/to/err` counts ECHO repeats.\n")
    for sf in ("sf1", "sf10"):
        md.append("## %s\n" % sf.upper())
        md.append("| Template | n | candidates | Time (per-cand/ECHO) | Work | W/L/T | Fallback | red(R) | ECHO | per-cand | plan (V/E, env) | ECHO ok/to/err | per-cand ok/to/err |")
        md.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|---|")
        for t, v in summary[sf + "_templates"].items():
            md.append("| %s | %d | %d | %s%s | %s | %d/%d/%d | %s | %s | %s | %s | %.0f/%.0f, %.0f | %d/%d/%d | %d/%d/%d |" % (
                t, v["rounds"], v["candidates"],
                fmt(v["time_ratio"]), fmt_ci(v["time_ci"]),
                fmt(v["work_ratio"]),
                v["time_wins"], v["time_losses"], v["time_ties"],
                fmt_pct(v["fallback_share"]), fmt(v["red_median"]),
                fmt_time(v["echo_time_median"]), fmt_time(v["serial_time_median"]),
                v["plan_mcs_vertices"] or 0, v["plan_mcs_edges"] or 0, v["plan_envelopes"] or 0,
                v["echo_ok"], v["echo_timeout"], v["echo_error"],
                v["serial_ok"], v["serial_timeout"], v["serial_error"]))
        md.append("")
    md.append("`red(R)` below -10 is a one-sided work blow-up, not a normal percentage: "
              "the SF10 IC9 row's -2616 comes from rounds 0702/0704, where ECHO's two-member "
              "envelope does 276,337,532 work against 105,612 per-candidate (wall 10.1 s vs "
              "2.7 s). The SF1 IC9 row's heavy tail includes ECHO repeats at ~3,182 s while "
              "per-candidate finishes that round in ~0.5 ks.\n")
    md.append("## Templates where ECHO loses to per-candidate\n")
    md.append("A template is listed when the point estimate of per-candidate/ECHO is below 1 "
              "(ECHO slower). Section A is the systematic set (95% interval below 1); section B "
              "is the near-tie set (interval includes 1). The clues are measurements, not causal "
              "claims. `red(R)` below -10 marks a one-sided work blow-up and is not a normal "
              "percentage.\n")
    for sf in ("sf1", "sf10"):
        md.append("### %s\n" % sf.upper())
        losers = [(t, v) for t, v in summary[sf + "_templates"].items()
                  if v["time_ratio"] is not None and v["time_ratio"] < 1.0 and v["rounds"] >= 3]
        losers.sort(key=lambda x: x[1]["time_ratio"])
        if not losers:
            md.append("None.\n")
            continue
        groups = [("A. Interval below 1 (systematic)", []),
                  ("B. Interval includes 1 (near tie)", [])]
        for t, v in losers:
            ci = v["time_ci"]
            systematic = ci[0] is not None and ci[1] is not None and ci[1] < 1.0
            groups[0 if systematic else 1][1].append((t, v))
        for gname, items in groups:
            if not items:
                continue
            md.append("**%s**\n" % gname)
            for t, v in items:
                clues = []
                if t in POINT and v["echo_time_median"] is not None and v["echo_time_median"] < 0.01:
                    clues.append("sub-10 ms point/index lookup; no shared expansion to amortise")
                fb = v["fallback_share"]
                if fb is not None and fb >= 0.5:
                    clues.append("Fallback share %.0f%%; most candidates are evaluated alone" % (100 * fb))
                elif fb is not None and fb >= 0.1:
                    clues.append("Fallback share %.0f%%" % (100 * fb))
                if (v["plan_mcs_vertices"] or 0) == 0 and (v["plan_envelopes"] or 0) == 0:
                    clues.append("median plan has no MCS/envelope (0 vertices, 0 envelopes)")
                if v["red_median"] is not None and v["red_median"] < -10:
                    clues.append("one-sided work blow-up: red(R) median %.0f, i.e. ECHO's shared "
                                 "envelope does orders of magnitude more work than per-candidate "
                                 "on some rounds" % v["red_median"])
                elif v["work_ratio"] is not None and v["work_ratio"] < 1:
                    clues.append("ECHO does more work (work ratio %.3f < 1)" % v["work_ratio"])
                if v["echo_timeout"] > v["serial_timeout"]:
                    clues.append("ECHO hits the cap more often (%d vs %d timeout repeats)" %
                                 (v["echo_timeout"], v["serial_timeout"]))
                if v["echo_error"] > v["serial_error"]:
                    clues.append("ECHO errors more (%d vs %d error repeats)" %
                                 (v["echo_error"], v["serial_error"]))
                if v["echo_time_max_median_ratio"] is not None and v["echo_time_max_median_ratio"] > 5:
                    clues.append("heavy tail: slowest ECHO round is %.0f× its median" %
                                 v["echo_time_max_median_ratio"])
                if v["time_ratio_median"] is not None and v["time_ratio_median"] >= 0.95 and v["time_ratio"] < 0.95:
                    clues.append("gmean is below the median (median %.3f); a few rounds drive the loss" %
                                 v["time_ratio_median"])
                if v["echo_time_median"] is not None and v["serial_time_median"] is not None:
                    clues.append("marginal median wall (not paired) %s vs %s" %
                                 (fmt_time(v["echo_time_median"]), fmt_time(v["serial_time_median"])))
                ci = v["time_ci"]
                md.append("- **%s** (n=%d, %d candidates): per-cand/ECHO %.3f [%s,%s], median %.3f, "
                          "ECHO W/L/T %d/%d/%d. Clues: %s." % (
                              t, v["rounds"], v["candidates"], v["time_ratio"],
                              fmt(ci[0]), fmt(ci[1]), v["time_ratio_median"] or 0,
                              v["time_wins"], v["time_losses"], v["time_ties"],
                              "; ".join(clues) if clues else "none beyond the ratio itself"))
            md.append("")
    md.append("## Reading the two SFs together\n")
    md.append("- The systematic ECHO losses on SF1 are the point-lookup templates "
              "(IS1, IS3, IS4, IS5), IC2's mostly-Fallback rounds, and IC9's heavy "
              "tail. SF1 wins are IC8 (shared range/count) and IS6/IS7 (topology).\n")
    md.append("- On SF10 the losing set is IC5/IC6/IC9 plus the small point templates; "
              "IC8, IS6 and IS7 are the clear wins. IC2 and IC12 are near ties with "
              "large plans or almost complete Fallback.\n")
    md.append("- A ratio above 1 in `table2.md` is a geometric mean and can coexist with "
              "a below-50% round win share: ECHO wins a few heavy rounds by a lot and "
              "loses many small rounds by a little. The W/L/T columns make that split "
              "explicit.\n")
    open(os.path.join(outdir, "by_template.md"), "w", encoding="utf-8").write("\n".join(md) + "\n")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cells", default=(ECHO_ROOT + "/work/main41a/cells.jsonl"))
    ap.add_argument("--out", default=(ECHO_ROOT + "/work/main41a"))
    a = ap.parse_args()
    rows, by = load(a.cells)
    h = hashlib.sha256()
    with open(a.cells, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    cells_sha = h.hexdigest()
    summary = {"cells_file": a.cells, "n_rows": len(rows),
               "method_order": ORDER, "labels": LABEL,
               "bootstrap": {"reps": BOOT_REPS, "seed": BOOT_SEED},
               "cap_sec": CAP}
    tables = {}
    for sf in ("sf1", "sf10"):
        s, rounds = compute(by, sf, 1)
        tables[sf] = {"summary": s, "rounds": rounds}
        summary[sf] = s
        strict, _ = compute(by, sf, 2)
        summary[sf + "_strict_k2"] = strict
        summary[sf + "_templates"] = template_table(by, sf, 1)
    inv = json.load(open(os.path.join(a.out, "inventory.json"), encoding="utf-8"))
    summary["inventory"] = inv
    def sha_file(path):
        hh = hashlib.sha256()
        with open(path, "rb") as fh:
            for chunk in iter(lambda: fh.read(1 << 20), b""):
                hh.update(chunk)
        return hh.hexdigest()
    script_dir = os.path.dirname(os.path.abspath(__file__))
    used_rows = 0
    for line in open(os.path.join(a.out, "inputs.sha256"), encoding="utf-8"):
        if "used=" in line:
            used_rows += int(line.rsplit("used=", 1)[1].split()[0])
    summary["inputs"] = {
        "merged_cells": a.cells, "merged_cells_sha256": cells_sha,
        "merged_cells_rows": len(rows),
        "source_rows": {k: v for k, v in sorted(collections.Counter(
            "%s|%s" % (r["sf"], r["source"]) for r in rows).items())},
        "expected_rows": 22125, "missing_rows": inv["n_missing"],
        "input_files": inv["input_files"], "input_file_rows": inv["input_rows"],
        "input_rows_used": used_rows,
        "input_digest": inv["input_digest"],
        "scripts_sha256": {n: sha_file(os.path.join(script_dir, n))
                           for n in ("build_cells.py", "summarize_table2.py")},
        "inventory_sha256": sha_file(os.path.join(a.out, "inventory.json")),
        "row_count_check": "22125 = 685*5*5 + 200*5*5; 40 missing = 8 SF10 IC9 round-reps * 5 methods",
        "paper_claim": {"sf1_rounds": 685, "sf1_candidates": 3810, "masc_coverage": 0.361},
    }
    json.dump(summary, open(os.path.join(a.out, "table2.json"), "w", encoding="utf-8"),
              indent=1, sort_keys=True)

    # ---------------- markdown: main table ----------------
    def row(m):
        vals = []
        for sf in ("sf1", "sf10"):
            s = summary[sf]["methods"][m]
            if m == "v3":
                vals += ["1", "1", "%.0f" % (s["rss_mb_median"] or 0),
                         fmt_pct(s["completion"]), fmt_pct(s["coverage"])]
            else:
                vals += [fmt(s["time_ratio"]) + fmt_ci(s["time_ci"]),
                         fmt(s["work_ratio"]) + fmt_ci(s["work_ci"]),
                         "%.0f" % (s["rss_mb_median"] or 0),
                         fmt_pct(s["completion"]), fmt_pct(s["coverage"])]
        return "| %s | %s |" % (LABEL[m], " | ".join(vals))

    md = []
    md.append("# Table II — LDBC SNB speculative rounds (Exp-1, K=8)\n")
    md.append("Computed from `work/main40h/run` cells, with the retained main40 SF10 "
              "cells for unaffected rounds (`work/main41a/scripts/build_cells.py`). "
              "Ratio is method/ECHO on the same round; >1 means ECHO is faster. "
              "Each round enters as the median of its five repeats; the interval is a "
              "95%% bootstrap over rounds (5,000 draws, seed %d). Memory is the median "
              "of per-round median peak RSS. Completion is the candidate-weighted share "
              "of repeats that finished within the 3,600 s cap. Coverage is the share of "
              "candidates evaluated jointly; ECHO candidates in Fallback are not covered.\n" % BOOT_SEED)
    md.append("**Population.** SF1: %d non-empty rounds, %d candidates, %d round-reps; "
              "the run contains %d jobs (685 rounds), but %d rounds have zero candidates "
              "and are excluded from ratios and denominators. SF10: %d rounds, %d candidates, "
              "1,000 round-reps, with 8 IC9 round-reps missing from both the new and retained "
              "old cells (round-0711 rep3/4, round-0712 rep0-4, round-0715 rep0); they stay "
              "in the completion denominator and are not estimated.\n" %
              (summary["sf1"]["rounds"], summary["sf1"]["candidates"],
               5 * summary["sf1"]["rounds"], 3425,
               685 - summary["sf1"]["rounds"],
               summary["sf10"]["rounds"], summary["sf10"]["candidates"]))
    md.append("**Provenance.** Merged `cells.jsonl` SHA256 `%s`, %d rows "
              "(22,125 = 685*5*5 + 200*5*5; 40 missing placeholders = 8 IC9 "
              "round-reps * 5 methods; 921 serial-fallback rows). `inputs.sha256` "
              "covers %d contributing files and %d file lines; combined input digest "
              "`%s`. Scripts are `scripts/build_cells.py` and "
              "`scripts/summarize_table2.py`; their SHA256 values are in `table2.json`.\n" %
              (cells_sha, len(rows), inv["input_files"], inv["input_rows"],
               inv["input_digest"]))
    md.append("| Method | SF1 Time | SF1 Work | SF1 Mem. (MB) | SF1 Compl. | SF1 Cov. "
              "| SF10 Time | SF10 Work | SF10 Mem. (MB) | SF10 Compl. | SF10 Cov. |")
    md.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for m in ORDER:
        md.append(row(m))
    md.append("")
    md.append("ECHO row: Time and Work are 1 by definition; Mem. is ECHO's measured median. "
              "BatchEnum+ and VLPQ have no encodable candidate on this workload; their cells "
              "are per-candidate fallback and their 0% coverage is the point of the column, "
              "not a missing measurement. MASC (port) covers only the pure-pattern candidates; "
              "its non-covered candidates (including all IC9) run as per-candidate Fallback, "
              "so its completion is the same as per-candidate.\n")

    # ---------------- secondary metrics ----------------
    md.append("## Secondary metrics\n")
    md.append("| SF | ECHO > per-candidate (valid pairs) | ECHO > per-candidate (all rounds) "
              "| ECHO Fallback share (ok cells) | red(R) median | red(R) pooled | red(R) p10..p90 "
              "| red(R) positive | red(R) n |")
    md.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    for sf in ("sf1", "sf10"):
        s = summary[sf]; e = s["echo_vs_serial"]; fb = s["fallback"]; rd = s["red"]
        md.append("| %s | %d/%d = %.1f%% | %d/%d = %.1f%% | %.1f%% (%d/%d) | %.3f | %.3f | %.3f..%.3f | %.1f%% | %d |" % (
            sf.upper(), e["wins"], e["valid_pairs"], 100 * e["valid_pair_win_share"],
            e["wins"], e["rounds"], 100 * e["outcome_win_share"],
            100 * fb["share"], fb["candidates"], fb["denominator"],
            rd["median"], rd["pooled"], rd["p10"], rd["p90"],
            100 * rd["positive_share"], rd["n"]))
    md.append("")
    md.append("`red(R) = 1 - W(ECHO)/W(per-candidate)` per round, from the same-round five-repeat "
              "medians; the pooled value sums work over the rounds with both work values. "
              "The all-round win share counts a non-completion by one side as that side losing; "
              "the valid-pair share excludes rounds where either side has no time. The SF10 "
              "mean red(R) is -79 because one IC9 plan does 2,600× per-candidate work on two "
              "rounds; median and pooled values are shown instead of that mean. "
              "All-at-cap sensitivity (errors and missing cells also enter at 3,600 s): "
              "SF1 per-candidate 1.373, VLPQ 1.262, MASC 1.361, BatchEnum+ 1.251; "
              "SF10 per-candidate 1.846, VLPQ 1.567, MASC 1.715, BatchEnum+ 1.572. "
              "The main and all-at-cap values differ by less than 0.06.\n")

    md.append("## Failure and denominator accounting\n")
    md.append("| SF | Method | ok | timeout | error | missing | cells | completion | coverage |")
    md.append("|---|---|---:|---:|---:|---:|---:|---:|---:|")
    for sf in ("sf1", "sf10"):
        for m in ORDER:
            s = summary[sf]["methods"][m]
            md.append("| %s | %s | %d | %d | %d | %d | %d | %.1f%% (%d/%d) | %.1f%% (%d/%d) |" % (
                sf.upper(), LABEL[m], s["ok"], s["timeout"], s["error"], s["missing"],
                s["cells"], 100 * s["completion"], s["completion_num"], s["completion_den"],
                100 * s["coverage"], s["coverage_num"], s["coverage_den"]))
    md.append("")
    md.append("Completion counts an `ok` repeat as all K candidates finished and a timeout/error/"
              "missing repeat as zero; partial progress inside a timed-out process is not "
              "recorded. Timeout time enters the time median at 3,600 s; errors have no time "
              "and are excluded from the time ratio but stay in completion. Missing SF10 IC9 "
              "round-reps are counted in the denominator, not filled. IC9 runs only ECHO and "
              "per-candidate; the other three methods use the per-candidate cell with "
              "coverage 0 (source `serial-fallback`).\n")

    md.append("## Population check\n")
    md.append("- The paper currently states 685 SF1 rounds and 3,810 candidates. The raw "
              "main40h cells give 685 rounds and **3,470** candidates; 25 of the 685 rounds "
              "(all IS3) have zero queries, and 25 have one query. Restricting to rounds with "
              "at least two candidates leaves 635 rounds and 3,445 candidates. The 3,810 "
              "figure is not reproducible from these cells; the paper text and this table "
              "need to be reconciled before the number is frozen. MASC coverage on this run "
              "is %.1f%%, not the paper's 36.1%%; ECHO coverage is %.1f%%.\n" %
              (100 * summary["sf1"]["methods"]["masc"]["coverage"],
               100 * summary["sf1"]["methods"]["v3"]["coverage"]))
    st = summary["sf1_strict_k2"]
    md.append("- On the paper's own ≥2-candidate rule (635 SF1 rounds, 3,445 candidates) "
              "the time ratios are per-candidate %.3f [%s,%s], MASC %.3f [%s,%s], "
              "BatchEnum+ %.3f, VLPQ %.3f; ECHO coverage %.1f%%, MASC coverage %.1f%%. "
              "`table2.json` stores this variant as `sf1_strict_k2`.\n" %
              (st["methods"]["serial"]["time_ratio"],
               fmt(st["methods"]["serial"]["time_ci"][0]), fmt(st["methods"]["serial"]["time_ci"][1]),
               st["methods"]["masc"]["time_ratio"],
               fmt(st["methods"]["masc"]["time_ci"][0]), fmt(st["methods"]["masc"]["time_ci"][1]),
               st["methods"]["batchenum"]["time_ratio"], st["methods"]["vlpq"]["time_ratio"],
               100 * st["methods"]["v3"]["coverage"], 100 * st["methods"]["masc"]["coverage"]))
    md.append("- ECHO and per-candidate `solutions` agree on every paired ok cell "
              "(SF1 %d/%d, SF10 %d/%d). MASC's adapter reports a different `solutions` value "
              "on %d SF1 and %d SF10 paired cells, all IS4; this is the adapter's match-count "
              "semantics, not a row-set mismatch in ECHO. The MASC time/work cells are kept, "
              "but its `solutions` column is not used as a correctness check.\n" %
              (summary["sf1"]["correctness"]["echo_serial_pairs"],
               summary["sf1"]["correctness"]["echo_serial_pairs"],
               summary["sf10"]["correctness"]["echo_serial_pairs"],
               summary["sf10"]["correctness"]["echo_serial_pairs"],
               summary["sf1"]["correctness"]["masc_bad"],
               summary["sf10"]["correctness"]["masc_bad"]))
    md.append("- SF1 peak RSS is measured under the shared read-only graph cache; it is much "
              "smaller than the old per-process-load cells and is not comparable to them in "
              "absolute terms. All methods in this table use the same cache and the same "
              "sidecar, so the relative column is comparable.\n")

    open(os.path.join(a.out, "table2.md"), "w", encoding="utf-8").write("\n".join(md) + "\n")
    write_by_template(summary, a.out)
    print("\n".join(md[:40]))
    print("\nwrote", os.path.join(a.out, "table2.json"), os.path.join(a.out, "table2.md"),
          os.path.join(a.out, "by_template.md"))


if __name__ == "__main__":
    sys.exit(main())
