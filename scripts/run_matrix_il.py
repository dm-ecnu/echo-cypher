#!/usr/bin/env python3
"""One-shot matrix with same-core adjacent interleaving.

Every cell (graph, workload, b0, bn) runs its repetitions and methods back to back on one core,
with the method order rotated per repetition, so a cell's methods share the core, the machine
state and the graph-load cache. Rows go to results/cells.jsonl through scripts/run_cell.sh, the
same protocol as the queued runner (single core, prlimit, 120 s per query, 3600 s per batch).

Experiments: E1 (k=15, o0), E2 (k=15, o3..o12), E5 (k=15, o9 ablations),
KS (MASC Sec. 6.4 query size k=32/64/128/256, o0, five methods).

Usage: run_matrix_il.py E1,E2,E5,KS [--cores 32-63] [--reps 5] [--kscales 32,64]
"""
import os
ECHO_WORK = os.environ.get("ECHO_WORK", ".")
import argparse
import json
import os
import queue
import subprocess
import sys
import threading
import time

REPO = (ECHO_WORK + "-native")
CELLS = os.path.join(REPO, "results", "cells.jsonl")
GRAPHS = {"figeys": ("maayan-figeys-100", "figeys-k15-b30-o{ov}-s0"),
          "dblp": ("dblp-100", "dblp-100-k15-b30-o{ov}-s0"),
          "wordnet": ("wordnet-words-100", "wordnet-words-100-k15-b30-o{ov}-s0")}
BATCHES = {"figeys": 10, "dblp": 10, "wordnet": 10}
METHODS = {"E1": ["echo", "perquery", "masc", "gup"],
           "E2": ["echo", "perquery", "masc", "gup"],
           "E5": ["echo", "echo_noenvelope", "echo_nomask"],
           "E125": ["echo", "perquery", "masc", "gup", "echo_noenvelope", "echo_nomask"],
           "KS": ["echo", "perquery", "masc", "gup", "echo_noenvelope"]}
KSCALE_GRAPHS = {"figeys": "figeys", "dblp": "dblp-100", "wordnet": "wordnet-words-100"}


def cells_for(exp, graphs):
    out = []
    if exp in ("E1", "E2"):
        ovs = [0] if exp == "E1" else [3, 6, 9, 12]
        for g in graphs:
            wf, wp = GRAPHS[g]
            for ov in ovs:
                w = wp.format(ov=ov)
                for b in range(BATCHES[g]):
                    out.append((g, wf, w, b * 30, 30))
    elif exp == "E5":
        for g in graphs:
            wf, wp = GRAPHS[g]
            w = wp.format(ov=9)
            for b in range(BATCHES[g]):
                out.append((g, wf, w, b * 30, 30))
    elif exp == "E125":
        # E1 o0, E2 o3/6/9/12, E5 o9 in one pass, every method on every cell.
        for g in graphs:
            wf, wp = GRAPHS[g]
            for ov in (0, 3, 6, 9, 12):
                w = wp.format(ov=ov)
                for b in range(BATCHES[g]):
                    out.append((g, wf, w, b * 30, 30))
    elif exp == "KS":
        # MASC Sec. 6.4: k=32/64 first so the light cells land before the heavy scales.
        for k in ("32", "64", "128", "256"):
            for g in graphs:
                w = f"{KSCALE_GRAPHS[g]}-k{k}-b30-o0-s0"
                wf = GRAPHS[g][0]
                for b in range(BATCHES[g]):
                    out.append((g, wf, w, b * 30, 30))
    else:
        raise SystemExit(f"unknown exp {exp}")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exps", help="comma separated E1,E2,E5,KS")
    ap.add_argument("--cores", default="32-63")
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--graphs", default="figeys,dblp,wordnet")
    ap.add_argument("--kscales", default="32,64,128,256")
    args = ap.parse_args()
    if "," in args.cores:
        cores = [int(x) for x in args.cores.split(",") if x]
    else:
        a, b = args.cores.split("-")
        cores = list(range(int(a), int(b) + 1))
    graphs = args.graphs.split(",")
    exps = args.exps.split(",")
    keep = args.kscales.split(",")

    todo = queue.Queue()
    total = 0
    for exp in exps:
        for (gg, wf, w, b0, bn) in cells_for(exp, graphs):
            if exp == "KS":
                k = w.split("-k")[1].split("-")[0]
                if k not in keep:
                    continue
            todo.put((exp, gg, wf, w, b0, bn))
            total += 1
    print(f"{args.exps}: {total} cells, {args.reps} reps, interleaved on "
          f"cores {cores[0]}-{cores[-1]}", flush=True)

    lock = threading.Lock()
    done = [0]

    def worker(core):
        while True:
            try:
                exp, gg, wf, w, b0, bn = todo.get_nowait()
            except queue.Empty:
                return
            methods = METHODS[exp]
            for rep in range(args.reps):
                shift = rep % len(methods)
                order = methods[shift:] + methods[:shift]
                for m in order:
                    cmd = ["bash", os.path.join(REPO, "scripts", "run_cell.sh"), gg, wf, w, m,
                           str(rep), str(b0), str(bn), str(core)]
                    env = dict(os.environ, ECHO_EXP=exp,
                               ECHO_COMMIT=os.environ.get("ECHO_COMMIT", ""))
                    logp = os.path.join(REPO, "results",
                                        f"il_{gg}_{w}_{m}_r{rep}_b{b0}_c{core}.log")
                    with open(logp, "a") as lf:
                        subprocess.run(cmd, cwd=REPO, env=env, stdout=subprocess.DEVNULL, stderr=lf)
            with lock:
                done[0] += 1
                if done[0] % 5 == 0:
                    print(f"  {done[0]}/{total} cells", flush=True)

    ths = [threading.Thread(target=worker, args=(c,), daemon=True) for c in cores]
    t0 = time.time()
    for t in ths:
        t.start()
    for t in ths:
        t.join()
    print(f"finished {total} cells in {time.time() - t0:.0f}s", flush=True)


main()
