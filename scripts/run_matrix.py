#!/usr/bin/env python3
"""E1-E5 matrix runner.
results/cells.jsonl by scripts/run_cell.sh; cells already recorded (any rc) are skipped.
Frozen protocol (MASC SIGMOD'26 Sec.6): k=15, 300 queries per setting, batch 30 default,
REPS repetitions, batch deadline 3600 s, per-query 120 s.
Usage: run_matrix.py E1|E2|E3|E5 [--cores 32-47] [--methods a,b,c] [--graphs g1,g2] [--reps 5]
"""
import os
ECHO_WORK = os.environ.get("ECHO_WORK", ".")
import argparse, itertools, json, os, subprocess, sys, threading, queue, time

REPO = (ECHO_WORK + "-native")
CELLS = os.path.join(REPO, "results", "cells.jsonl")
GRAPHS = {"figeys": ("maayan-figeys-100", "figeys-k15-b30-o{ov}-s0"),
          "dblp": ("dblp-100", "dblp-100-k15-b30-o{ov}-s0"),
          "wordnet": ("wordnet-words-100", "wordnet-words-100-k15-b30-o{ov}-s0")}
BATCHES = {"figeys": 10, "dblp": 10, "wordnet": 10}  # 300 queries / 30
METHODS_DEFAULT = "echo,perquery,masc,gup"

def done_keys():
    keys = set()
    if os.path.exists(CELLS):
        for line in open(CELLS):
            try: r = json.loads(line)
            except Exception: continue
            keys.add((r["graph"], r["workload"], r["method"], r["rep"], r["b0"], r["bn"]))
    return keys

def wl(g, ov):
    wf, wp = GRAPHS[g]
    return wf, wp.format(ov=ov)

def cells_for(exp, graphs):
    out = []
    if exp in ("E1", "E2"):
        ovs = [0] if exp == "E1" else [0, 3, 6, 9, 12]
        for g in graphs:
            for ov in ovs:
                wf, w = wl(g, ov)
                for b in range(BATCHES[g]):
                    out.append((g, wf, w, b * 30, 30))
    elif exp == "E3":
        for g in graphs:
            wf, w = wl(g, 0)
            for bn in (10, 20, 30, 40, 50):
                for b0 in range(0, 300, bn):
                    # The last group of a batch size need not be full; asking for more queries
                    # than the workload holds aborts the run (missing query file).
                    out.append((g, wf, w, b0, min(bn, 300 - b0)))
    elif exp == "E5":
        for g in graphs:
            wf, w = wl(g, 9)
            for b in range(BATCHES[g]):
                out.append((g, wf, w, b * 30, 30))
    else:
        raise SystemExit(f"unknown exp {exp} (E4 lives in echo-native-data)")
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exp")
    ap.add_argument("--cores", default="32-47")
    ap.add_argument("--methods", default=METHODS_DEFAULT)
    ap.add_argument("--graphs", default="figeys,dblp,wordnet")
    ap.add_argument("--reps", type=int, default=5)
    args = ap.parse_args()
    a, b = args.cores.split("-")
    cores = list(range(int(a), int(b) + 1))
    methods = args.methods.split(",")
    graphs = args.graphs.split(",")
    have = done_keys()
    todo = []
    for (gg, wf, w, b0, bn) in cells_for(args.exp, graphs):
        for rep, m in itertools.product(range(args.reps), methods):
            if (gg, w, m, rep, b0, bn) in have: continue
            todo.append((gg, wf, w, m, rep, b0, bn))
    print(f"{args.exp}: {len(todo)} cells on cores {cores[0]}-{cores[-1]}", flush=True)
    q = queue.Queue()
    for c in todo: q.put(c)
    def worker(core):
        while True:
            try: gg, wf, w, m, rep, b0, bn = q.get_nowait()
            except queue.Empty: return
            env = dict(os.environ, ECHO_EXP=args.exp)
            cmd = ["bash", os.path.join(REPO, "scripts", "run_cell.sh"), gg, wf, w, m,
                   str(rep), str(b0), str(bn), str(core)]
            logp = os.path.join(REPO, "results", f"cellerr_{gg}_{w}_{m}_r{rep}_b{b0}.log")
            subprocess.run(cmd, cwd=REPO, env=env, stdout=subprocess.DEVNULL,
                           stderr=open(logp, "a"))
    ths = [threading.Thread(target=worker, args=(c,), daemon=True) for c in cores]
    t0 = time.time()
    for t in ths: t.start()
    for t in ths: t.join()
    print(f"{args.exp} finished, {len(todo)} cells, {time.time()-t0:.0f}s", flush=True)

main()
