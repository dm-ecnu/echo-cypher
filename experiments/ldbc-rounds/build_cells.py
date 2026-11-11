#!/usr/bin/env python3
"""Build the merged main41a cell table for the LDBC Exp-1 table.

Inputs
  work/main40h/run/...            main40h SF1 and SF10 cells (binary 0915fb6)
  work/main40/run/sf10/...        retained old main40 SF10 cells (b737def)

Retention rule (from work/main40h/scripts/run_main40h.py and manifest.json):
  - SF1: every (round, repeat) is the new main40h run.
  - SF10 affected rounds: use main40h only; never use old cells.
  - SF10 other rounds: use a new cell where present, otherwise a completed old cell.
  - IC9 heavy round-reps in the protocol ran only ECHO and per-candidate; the two
    published ports and VLPQ fall back to per-candidate.  For the table they are
    represented by the serial cell with coverage 0 and an explicit
    source="serial-fallback" marker.  This is the main30/main39 convention, not an
    estimate.
  - Missing cells stay missing.  No value is fabricated.

Outputs
  cells.jsonl       one row per (sf, round, repeat, method)
  inventory.json    populations, row counts, candidate counts, missing cells
  inputs.sha256     SHA256 and line count for every input file that contributed
"""
import os
ECHO_ROOT = os.environ.get("ECHO_ROOT", ".")
import argparse
import collections
import glob
import hashlib
import json
import os
import re
import sys

METHODS = ["v3", "serial", "masc", "batchenum", "vlpq"]
AFFECTED = set(list(range(75, 80)) + list(range(140, 145)) +
               list(range(250, 255)) + list(range(300, 350)) +
               list(range(550, 555)) + list(range(850, 855)) +
               list(range(875, 880)))


def rno(path):
    m = re.search(r"round-(\d+)\.json$", path)
    return int(m.group(1)) if m else -1


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def line_count(path):
    n = 0
    with open(path, "rb") as f:
        for _ in f:
            n += 1
    return n


def load_cells(root):
    """Return key -> (tend, row, file).  Keep the latest execution."""
    out = {}
    files = sorted(glob.glob(os.path.join(root, "**", "cells*.jsonl"), recursive=True))
    for f in files:
        try:
            for line in open(f, encoding="utf-8"):
                try:
                    r = json.loads(line)
                except Exception:
                    continue
                p, rep, m = r.get("v3_round"), r.get("repeat"), r.get("method")
                if not p or rep is None or m not in METHODS:
                    continue
                k = (os.path.realpath(p), int(rep), m)
                tend = float(r.get("tend", 0.0))
                if k not in out or tend >= out[k][0]:
                    out[k] = (tend, r, f)
        except OSError:
            pass
    return out


def old_max_wall(root):
    """Old per-round wall sums, used only to mirror the driver's 'heavy' flag."""
    by = collections.defaultdict(float)
    for f in glob.glob(os.path.join(root, "work/e4v3/cells-120382e/sf10/cells-*.jsonl")):
        for line in open(f, encoding="utf-8"):
            try:
                r = json.loads(line)
            except Exception:
                continue
            p = r.get("v3_round")
            if p and r.get("repeat") is not None:
                by[(os.path.realpath(p), int(r["repeat"]))] += float(r.get("wall_sec", 0.0))
    out = collections.defaultdict(float)
    for (p, rep), w in by.items():
        out[p] = max(out[p], w)
    return out


def sf1_population(root):
    out = []
    for f in sorted(glob.glob(os.path.join(root, "work/main26/e4/lists26r/shard-*.txt"))):
        for line in open(f, encoding="utf-8"):
            p = line.strip()
            if p:
                out.append(os.path.join(root, "work/diag/rounds-new-all", os.path.basename(p)))
    return sorted(set(out))


def sf10_population(root):
    p = os.path.join(root, "work/main40/data/sf10-sample200.jsonl")
    return sorted({json.loads(l)["path"] for l in open(p, encoding="utf-8")})


def template_of(path):
    try:
        return json.load(open(path, encoding="utf-8")).get("template", "?")
    except Exception:
        return "?"


def queries_of(path):
    try:
        return len(json.load(open(path, encoding="utf-8")).get("queries", []))
    except Exception:
        return None


def serial_fallback(src, sf, path, rep, k, template):
    r = dict(src)
    r["method"] = "masc"
    r["status"] = src.get("status")
    r["covered_candidates"] = 0
    r["attempted_candidates"] = 0
    r["coverage"] = 0.0
    r["fallback_from_serial"] = True
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=(ECHO_ROOT))
    ap.add_argument("--out", default=None)
    a = ap.parse_args()
    root = a.root
    outdir = a.out or os.path.join(root, "work/main41a")
    os.makedirs(outdir, exist_ok=True)

    pops = {"sf1": sf1_population(root), "sf10": sf10_population(root)}
    assert len(pops["sf1"]) == 685, len(pops["sf1"])
    assert len(pops["sf10"]) == 200, len(pops["sf10"])
    tm = {p: template_of(p) for p in pops["sf1"] + pops["sf10"]}
    kq = {p: queries_of(p) for p in pops["sf1"] + pops["sf10"]}

    new = {
        "sf1": load_cells(os.path.join(root, "work/main40h/run")),
        "sf10": load_cells(os.path.join(root, "work/main40h/run")),
    }
    # load_cells(root) scans both sf1 and sf10; keep them separate by round path.
    all_new = load_cells(os.path.join(root, "work/main40h/run"))
    old_sf10 = load_cells(os.path.join(root, "work/main40/run/sf10"))
    oldwall = old_max_wall(root)

    used_files = collections.Counter()
    rows = []
    counts = collections.Counter()
    missing = []
    k_by_sf = collections.Counter()
    methods_by_sf = collections.Counter()

    for sf in ("sf1", "sf10"):
        pop = pops[sf]
        for p in pop:
            rp = os.path.realpath(p)
            t = tm[p]
            k = kq[p]
            for rep in range(5):
                if sf == "sf10":
                    affected = rno(p) in AFFECTED
                    heavy = (affected or oldwall.get(rp, 0.0) > 60.0 or rp not in oldwall)
                else:
                    affected = False
                    heavy = True
                expected_methods = ["v3", "serial"] if (t == "IC9" and heavy) else METHODS
                for m in METHODS:
                    src = None
                    source = None
                    if sf == "sf1":
                        key = (rp, rep, m)
                        if key in all_new:
                            src = all_new[key][1]
                            source = "main40h"
                            used_files[all_new[key][2]] += 1
                    else:
                        key = (rp, rep, m)
                        if affected:
                            if key in all_new:
                                src = all_new[key][1]
                                source = "main40h"
                                used_files[all_new[key][2]] += 1
                        else:
                            if key in all_new:
                                src = all_new[key][1]
                                source = "main40h"
                                used_files[all_new[key][2]] += 1
                            elif key in old_sf10:
                                src = old_sf10[key][1]
                                source = "main40-old"
                                used_files[old_sf10[key][2]] += 1
                    if src is None:
                        if t == "IC9" and m in ("masc", "batchenum", "vlpq"):
                            serial_key = (rp, rep, "serial")
                            serial_src = all_new.get(serial_key)
                            if sf == "sf10" and not affected and serial_key in old_sf10:
                                serial_src = old_sf10.get(serial_key) or serial_src
                            if serial_src is not None:
                                src = serial_fallback(serial_src[1], sf, p, rep, k, t)
                                src["method"] = m
                                source = "serial-fallback"
                                used_files[serial_src[2]] += 1
                    if src is None:
                        rows.append({
                            "sf": sf, "round": p, "round_basename": os.path.basename(p),
                            "round_index": rno(p), "template": t, "repeat": rep,
                            "method": m, "status": "missing", "timeout": False,
                            "rc": None, "wall_sec": None, "work": None,
                            "peak_rss_kb": None, "fallback": None, "envelopes": None,
                            "mcs_vertices": None, "mcs_edges": None,
                            "covered_candidates": None, "attempted_candidates": None,
                            "solutions": None, "output_tuples": None,
                            "k": k, "expected": m in expected_methods,
                            "source": "missing", "affected": affected,
                        })
                        missing.append({"sf": sf, "round": os.path.basename(p),
                                        "repeat": rep, "method": m, "template": t})
                        counts[(sf, m, "missing")] += 1
                        continue
                    r = {
                        "sf": sf, "round": p, "round_basename": os.path.basename(p),
                        "round_index": rno(p), "template": t, "repeat": rep,
                        "method": m, "status": src.get("status", "?"),
                        "timeout": bool(src.get("timeout", False)),
                        "rc": src.get("rc"),
                        "wall_sec": src.get("wall_sec"),
                        "work": src.get("work"),
                        "peak_rss_kb": src.get("peak_rss_kb"),
                        "fallback": src.get("fallback"),
                        "envelopes": src.get("envelopes"),
                        "mcs_vertices": src.get("mcs_vertices"),
                        "mcs_edges": src.get("mcs_edges"),
                        "covered_candidates": src.get("covered_candidates"),
                        "attempted_candidates": src.get("attempted_candidates"),
                        "solutions": src.get("solutions"),
                        "output_tuples": src.get("output_tuples"),
                        "k": k,
                        "expected": m in expected_methods,
                        "source": source,
                        "fallback_from_serial": bool(src.get("fallback_from_serial", False)),
                        "affected": affected,
                    }
                    rows.append(r)
                    counts[(sf, m, r["status"])] += 1
                    methods_by_sf[sf] += 1
            k_by_sf[sf] += k

    rows.sort(key=lambda r: (r["sf"], r["round_index"], r["repeat"], METHODS.index(r["method"])))
    with open(os.path.join(outdir, "cells.jsonl"), "w", encoding="utf-8") as f:
        for r in rows:
            f.write(json.dumps(r, sort_keys=True) + "\n")

    # Input manifest: hash every contributing file and record its line count.
    entries = []
    for f, used in sorted(used_files.items()):
        entries.append((sha256_file(f), line_count(f), used, f))
    with open(os.path.join(outdir, "inputs.sha256"), "w", encoding="utf-8") as f:
        for sha, n, used, path in entries:
            f.write("%s  %s  rows=%d  used=%d\n" % (sha, path, n, used))
    digest = hashlib.sha256()
    for sha, n, used, path in entries:
        digest.update(("%s  %s  %d  %d\n" % (sha, path, n, used)).encode())
    inventory = {
        "root": root,
        "populations": {k: len(v) for k, v in pops.items()},
        "round_reps": {"sf1": 3425, "sf10": 1000},
        "candidate_sum_per_round": dict(k_by_sf),
        "rows": len(rows),
        "rows_by_sf_method_status": {"|".join(k): v for k, v in sorted(counts.items())},
        "missing": missing,
        "n_missing": len(missing),
        "input_files": len(entries),
        "input_rows": sum(n for _, n, _, _ in entries),
        "input_digest": digest.hexdigest(),
    }
    json.dump(inventory, open(os.path.join(outdir, "inventory.json"), "w", encoding="utf-8"),
              indent=1, sort_keys=True)
    print(json.dumps({k: inventory[k] for k in
                      ("populations", "candidate_sum_per_round", "rows", "n_missing",
                       "input_files", "input_rows", "input_digest")}, indent=1))
    print("missing:", inventory["missing"][:10], "..." if len(missing) > 10 else "")


if __name__ == "__main__":
    sys.exit(main())
