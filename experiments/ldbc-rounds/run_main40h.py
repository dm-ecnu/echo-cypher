#!/usr/bin/env python3
"""main40h: rerun the correct SF1 population and complete/replace the SF10 sample.

SF1: the 685 rounds from main26/e4/lists26r, remapped to rounds-new-all.  Every round
run as one (round, repeat) job because no correct older cell exists for the heavy test.
SF10: keep a completed old cell only when its round is outside the close-edge affected set;
rerun every repeat of the 21 affected sample rounds and every incomplete (round, repeat).
Cells are written only under work/main40h/run.
"""
import os
ECHO_ROOT = os.environ.get("ECHO_ROOT", ".")
import argparse, collections, glob, json, os, queue, re, signal, subprocess, sys, threading, time

V3 = (ECHO_ROOT)
# Repository root: the frozen round workloads ship under workloads/ldbc-rounds/.
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
METHODS = ["v3", "serial", "masc", "batchenum", "vlpq"]
AFFECTED = set(list(range(75, 80)) + list(range(140, 145)) + list(range(250, 255)) +
               list(range(300, 350)) + list(range(550, 555)) +
               list(range(850, 855)) + list(range(875, 880)))

def now(): return time.strftime("%F %T")
def rno(p):
    m = re.search(r"round-(\d+)\.json$", p)
    return int(m.group(1)) if m else -1
def tmpl(p):
    try: return json.load(open(p)).get("template", "?")
    except Exception: return "?"
def pss_kb(pid):
    try:
        for line in open(f"/proc/{pid}/smaps_rollup"):
            if line.startswith("Pss:"): return int(line.split()[1])
    except Exception: pass
    return 0
def mem_available_kb():
    for line in open("/proc/meminfo"):
        if line.startswith("MemAvailable:"): return int(line.split()[1])
    return 0
def ours_pss_kb():
    total = 0
    for d in glob.glob("/proc/[0-9]*"):
        try: cmd = open(os.path.join(d, "cmdline"), "rb").read().replace(b"\0", b" ").decode("utf8", "ignore")
        except Exception: continue
        if "icde2027/echo/v3/" not in cmd or "v3_batch" not in cmd: continue
        if not any(k in cmd for k in ("work/main40/", "work/main40-e56/", "work/main40h/", "work/main40g/", "work/main40f/")): continue
        total += pss_kb(int(d.rsplit("/", 1)[-1]))
    return total

def load_cells(root):
    out = {}
    for f in glob.glob(os.path.join(root, "**", "cells*.jsonl"), recursive=True):
        try:
            mt = os.path.getmtime(f)
            for line in open(f):
                try: r = json.loads(line)
                except Exception: continue
                p, rep, m = r.get("v3_round"), r.get("repeat"), r.get("method")
                if not p or rep is None or m not in METHODS: continue
                k = (os.path.realpath(p), int(rep), m)
                tend = float(r.get("tend", mt))
                if k not in out or tend >= out[k][0]: out[k] = (tend, r)
        except OSError: pass
    return {k: v[1] for k, v in out.items()}

def active_old_reps():
    """(round, repeat) pairs held by a live old main40 SF10 job."""
    out = set()
    for d in glob.glob("/proc/[0-9]*"):
        try:
            cmd = open(os.path.join(d, "cmdline"), "rb").read().replace(b"\0", b" ").decode("utf8", "ignore")
        except Exception:
            continue
        if "work/main40/run/sf10/" not in cmd or "v3_batch-b737def" not in cmd:
            continue
        m = re.search(r"work/main40/run/sf10/(round-\d+-rep\d+)/", cmd)
        if not m:
            continue
        rep = int(m.group(1).rsplit("-rep", 1)[1])
        lp = os.path.join(V3, "work/main40/run/sf10", m.group(1), "list.txt")
        try:
            for line in open(lp):
                p = line.strip()
                if p: out.add((os.path.realpath(p), rep))
        except OSError:
            pass
    return out

def old_max_wall():
    by = collections.defaultdict(float)
    for f in glob.glob(os.path.join(V3, "work/e4v3/cells-120382e/sf10/cells-*.jsonl")):
        for line in open(f):
            try: r = json.loads(line)
            except Exception: continue
            p = r.get("v3_round")
            if p and r.get("repeat") is not None:
                by[(os.path.realpath(p), int(r["repeat"]))] += float(r.get("wall_sec", 0.0))
    out = collections.defaultdict(float)
    for (p, rep), w in by.items(): out[p] = max(out[p], w)
    return out

def sf1_population():
    out = []
    for f in sorted(glob.glob(os.path.join(REPO, "workloads/ldbc-rounds/populations/sf1-shards/shard-*.txt"))):
        for line in open(f):
            p = line.strip()
            if p: out.append(os.path.join(REPO, "workloads/ldbc-rounds/sf1", os.path.basename(p)))
    return sorted(set(out))

def sf10_population():
    p = os.path.join(REPO, "workloads/ldbc-rounds/populations/sf10-sample200.jsonl")
    return sorted({os.path.join(REPO, json.loads(l)["path"]) for l in open(p)})

def expected_methods(path, heavy):
    return ["v3", "serial"] if tmpl(path) == "IC9" and heavy else METHODS

def complete(cells, path, rep, methods):
    p = os.path.realpath(path)
    return all((p, rep, m) in cells for m in methods)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--cores", required=True)
    ap.add_argument("--conc-sf1", type=int, default=24)
    ap.add_argument("--conc-sf10", type=int, default=12)
    ap.add_argument("--mem-sf1-gib", type=int, default=24)
    ap.add_argument("--mem-sf10-gib", type=int, default=32)
    ap.add_argument("--est-sf1-gib", type=float, default=5.5)
    ap.add_argument("--est-sf10-gib", type=float, default=15.0)
    ap.add_argument("--shard", type=int, default=5)
    ap.add_argument("--pss-fraction", type=float, default=0.5)
    ap.add_argument("--cell-cap", type=int, default=3600)
    ap.add_argument("--warm-cap", type=int, default=60)
    ap.add_argument("--retry", type=int, default=1)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--only", choices=["all", "sf1", "sf10"], default="all")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    newcells = load_cells(os.path.join(a.out, "run"))
    oldcells = load_cells(os.path.join(V3, "work/main40/run/sf10"))
    oldwall = old_max_wall()
    active_old = active_old_reps()
    pops = {"sf1": sf1_population(), "sf10": sf10_population()}
    assert len(pops["sf1"]) == 685, len(pops["sf1"])
    assert len(pops["sf10"]) == 200, len(pops["sf10"])
    jobs = []
    counts = {}
    for sf in ("sf1", "sf10"):
        if a.only != "all" and a.only != sf:
            continue
        todo = 0
        for rep in range(5):
            singles, normal = [], []
            for p in pops[sf]:
                rp = os.path.realpath(p)
                if sf == "sf10" and (rp, rep) in active_old:
                    continue
                affected = sf == "sf10" and rno(p) in AFFECTED
                heavy = sf == "sf1" or affected or oldwall.get(rp, 0.0) > 60.0 or rp not in oldwall
                methods = expected_methods(p, heavy)
                if complete(newcells, p, rep, methods):
                    continue
                if sf == "sf10" and not affected and complete(oldcells, p, rep, methods):
                    continue
                todo += 1
                (singles if heavy else normal).append((p, methods))
            for i, (p, methods) in enumerate(singles):
                jobs.append({"sf": sf, "rep": rep, "rounds": [p], "methods": methods,
                             "name": f"{sf}-r{rep}-s-{rno(p):04d}",
                             "heavy": True, "affected": sf == "sf10" and rno(p) in AFFECTED})
            # Greedy groups of normal rounds with the same method set.  IC9 is normally not heavy;
            # if it is normal it uses the full opponent set like main40's normal shards.
            for i in range(0, len(normal), a.shard):
                chunk = normal[i:i + a.shard]
                if len({tuple(x[1]) for x in chunk}) != 1:
                    chunk = [chunk[0]]
                jobs.append({"sf": sf, "rep": rep, "rounds": [x[0] for x in chunk],
                             "methods": chunk[0][1], "name": f"{sf}-r{rep}-n-{i//a.shard:03d}",
                             "heavy": False, "affected": False})
        counts[sf] = todo
    # Affected and heavy first; then SF10 completion before the much larger SF1 population.
    jobs.sort(key=lambda j: (0 if (j["sf"] == "sf10" and j["affected"]) else 1 if j["sf"] == "sf10" else 2,
                             j["rep"], j["name"]))
    manifest = {"bin": a.bin, "population": {k: len(v) for k, v in pops.items()},
                "todo_round_reps": counts, "jobs": len(jobs), "affected_sf10_rounds": sorted(
                    [os.path.basename(p) for p in pops["sf10"] if rno(p) in AFFECTED]),
                "affected_sf10_n": sum(1 for p in pops["sf10"] if rno(p) in AFFECTED),
                "conc": {"sf1": a.conc_sf1, "sf10": a.conc_sf10}, "cores": a.cores,
                "mem_gib": {"sf1": a.mem_sf1_gib, "sf10": a.mem_sf10_gib},
                "cell_cap": a.cell_cap, "warm_cap": a.warm_cap, "pss_fraction": a.pss_fraction,
                "only": a.only, "old_cells": len(oldcells), "new_cells": len(newcells), "active_old_reps": len(active_old)}
    json.dump(manifest, open(os.path.join(a.out, "manifest.json"), "w"), indent=1)
    print("START", now(), json.dumps(manifest), flush=True)
    if a.dry_run:
        return 0

    cores = queue.Queue()
    for x in a.cores.split(","):
        if x.strip(): cores.put(int(x.strip()))
    pending = collections.deque(jobs)
    running = {}
    attempts = collections.Counter()
    failed = []
    stop = {"flag": False}
    signal.signal(signal.SIGTERM, lambda *_: stop.__setitem__("flag", True))
    pssf = open(os.path.join(a.out, "pss.jsonl"), "a")
    lock = threading.Lock()
    peak = {"pss": 0, "procs": 0}
    def monitor():
        while not stop["flag"]:
            p = ours_pss_kb(); av = mem_available_kb()
            with lock:
                peak["pss"] = max(peak["pss"], p)
                pssf.write(json.dumps({"t": time.time(), "ours_pss_kb": p,
                                       "mem_available_kb": av, "half_avail_kb": av//2}) + "\n")
                pssf.flush()
            time.sleep(5)
    threading.Thread(target=monitor, daemon=True).start()

    t0 = time.time(); finished = 0; admitted = 0
    while (pending or running) and not stop["flag"]:
        for pid, (job, proc, ts) in list(running.items()):
            rc = proc.poll()
            if rc is None: continue
            del running[pid]; cores.put(job["core"]); finished += 1
            cf = job["cells"]
            got = 0
            if os.path.exists(cf):
                try: got = sum(1 for _ in open(cf))
                except Exception: got = 0
            exp = len(job["rounds"]) * len(job["methods"])
            print("JOB", job["name"], "rc", rc, "rows", got, "exp", exp,
                  "%.0fs" % (time.time() - ts), flush=True)
            # Pull this job's rows into the in-memory set before deciding what is still missing.
            for line in open(cf) if os.path.exists(cf) else []:
                try: r = json.loads(line)
                except Exception: continue
                p, rep, m = r.get("v3_round"), r.get("repeat"), r.get("method")
                if p and rep is not None and m in METHODS:
                    newcells[(os.path.realpath(p), int(rep), m)] = r
            miss = [p for p in job["rounds"] if not complete(newcells, p, job["rep"], job["methods"])]
            if miss and attempts[job["name"]] < a.retry and not stop["flag"]:
                attempts[job["name"]] += 1
                pending.appendleft(dict(job, name=job["name"] + "-retry", rounds=miss))
            elif miss:
                failed.extend((job["name"], p) for p in miss)
        run_sf = collections.Counter(j["sf"] for j, _, _ in running.values())
        while pending and not stop["flag"] and not cores.empty():
            ours = ours_pss_kb(); av = mem_available_kb(); budget = int(av * a.pss_fraction)
            pick = None
            for i, j in enumerate(pending):
                cap = a.conc_sf1 if j["sf"] == "sf1" else a.conc_sf10
                est = a.est_sf1_gib if j["sf"] == "sf1" else a.est_sf10_gib
                if run_sf[j["sf"]] >= cap: continue
                if ours + int(est * (1 << 20)) <= budget:
                    pick = i; break
            if pick is None: break
            job = pending[pick]; del pending[pick]
            core = cores.get(); run_sf[job["sf"]] += 1
            d = os.path.join(a.out, "run", job["name"]); os.makedirs(d, exist_ok=True)
            lst = os.path.join(d, "list.txt")
            with open(lst, "w") as f:
                for p in job["rounds"]: f.write(p + "\n")
            cf = os.path.join(d, "cells.jsonl"); job["cells"] = cf; job["core"] = core
            if os.path.exists(cf): os.unlink(cf)
            mem = a.mem_sf1_gib if job["sf"] == "sf1" else a.mem_sf10_gib
            side = os.path.join(V3, "sidecar", "sf1i" if job["sf"] == "sf1" else "sf10")
            cmd = ["taskset", "-c", str(core), "prlimit", f"--as={mem * (1 << 30)}", f"--rss={mem * (1 << 30)}",
                   a.bin, "--sidecar", side, "--list", lst, "--out", cf,
                   "--reps", "1", "--rep-base", str(job["rep"]),
                   "--warmup", "--warm-cap", str(a.warm_cap), "--cell-cap", str(a.cell_cap),
                   "--run-id", "main40h-" + job["sf"], "--dataset", "ldbc-" + job["sf"]]
            # main40 used only the serial opponent for heavy IC9 rounds.
            opp = [m for m in job["methods"] if m != "v3"]
            if opp: cmd += ["--opponents", ",".join(opp)]
            os.makedirs(os.path.join(a.out, "logs"), exist_ok=True)
            log = open(os.path.join(a.out, "logs", job["name"] + ".log"), "a")
            log.write(f"START {now()} core={core} rounds={len(job['rounds'])} rep={job['rep']} methods={','.join(job['methods'])} ours={ours/1048576:.1f} avail={av/1048576:.1f}\n")
            log.flush()
            try:
                proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, preexec_fn=os.setsid)
            except Exception as e:
                log.write(f"Popen failed: {e!r}\n"); log.close(); failed.append((job["name"], "popen")); cores.put(core); continue
            running[proc.pid] = (job, proc, time.time()); admitted += 1
            print("LAUNCH", job["name"], "core", core, "rounds", len(job["rounds"]),
                  "rep", job["rep"], "methods", len(job["methods"]),
                  "ours", round(ours/1048576, 1), "avail", round(av/1048576, 1), flush=True)
        time.sleep(2)
    if stop["flag"]:
        print("STOP requested; no new dispatch, waiting for running jobs", flush=True)
        while running:
            for pid, (job, proc, ts) in list(running.items()):
                if proc.poll() is not None:
                    del running[pid]; cores.put(job["core"])
            time.sleep(2)
    stop["flag"] = True
    time.sleep(0.2)
    pssf.close()
    status = {"finished_jobs": finished, "admitted": admitted, "failed_n": len(failed),
              "failed": failed[:200], "elapsed_sec": time.time() - t0,
              "peak_ours_pss_kb": peak["pss"], "job_count": len(jobs)}
    json.dump(status, open(os.path.join(a.out, "status.json"), "w"), indent=1)
    print("DONE", json.dumps(status), flush=True)

if __name__ == "__main__":
    sys.exit(main())
