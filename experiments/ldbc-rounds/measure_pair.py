#!/usr/bin/env python3
"""Measure two concurrent graph-load processes: legacy loader vs shared cache."""
import argparse, json, os, re, subprocess, time

def pss(pid):
    try:
        t = open(f"/proc/{pid}/smaps_rollup").read()
        m = re.search(r"^Pss:\s+(\d+)", t, re.M)
        return int(m.group(1)) if m else 0
    except Exception:
        return 0
def rss(pid):
    try:
        t = open(f"/proc/{pid}/status").read()
        m = re.search(r"^VmRSS:\s+(\d+)", t, re.M)
        return int(m.group(1)) if m else 0
    except Exception:
        return 0
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dataset", required=True)
    ap.add_argument("--old", required=True)
    ap.add_argument("--new", required=True)
    ap.add_argument("--sidecar", required=True)
    ap.add_argument("--outdir", required=True)
    ap.add_argument("--cores", default="122,123")
    ap.add_argument("--mem-gib", type=int, default=64)
    ap.add_argument("--hold", type=int, default=30)
    a = ap.parse_args()
    os.makedirs(os.path.join(a.outdir, "logs"), exist_ok=True)
    empty = os.path.join(a.outdir, "empty.list")
    open(empty, "w").close()
    cores = [int(x) for x in a.cores.split(",") if x.strip()]
    rows = []
    for tag, b in (("old", a.old), ("new", a.new)):
        procs = []
        for i in range(2):
            log = open(os.path.join(a.outdir, "logs", f"{tag}-{i}.log"), "w")
            cmd = ["taskset", "-c", str(cores[i]), "prlimit",
                   f"--as={a.mem_gib * (1 << 30)}", f"--rss={a.mem_gib * (1 << 30)}", b,
                   "--sidecar", a.sidecar, "--list", empty,
                   "--out", os.path.join(a.outdir, f"{tag}-{i}.out"),
                   "--load-hold", str(a.hold), "--run-id", "main40h-load-" + tag,
                   "--dataset", "ldbc-" + a.dataset]
            p = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, preexec_fn=os.setsid)
            procs.append((p, log))
        peak = 0; peak_each = [0, 0]
        t0 = time.time()
        while any(p.poll() is None for p, _ in procs):
            vals = [pss(p.pid) for p, _ in procs]
            peak = max(peak, sum(vals)); peak_each = [max(peak_each[i], vals[i]) for i in range(2)]
            time.sleep(1)
        rcs = [p.wait() for p, _ in procs]
        for _, log in procs: log.close()
        row = {"label": tag, "rc": rcs, "elapsed_sec": time.time() - t0,
               "peak_pair_pss_kb": peak, "peak_each_pss_kb": peak_each,
               "peak_pair_pss_gib": round(peak / 1024 / 1024, 3),
               "peak_each_pss_gib": [round(x / 1024 / 1024, 3) for x in peak_each]}
        rows.append(row); print(json.dumps(row), flush=True)
    json.dump(rows, open(os.path.join(a.outdir, "memory.json"), "w"), indent=1)

if __name__ == "__main__":
    main()
