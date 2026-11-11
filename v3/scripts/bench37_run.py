#!/usr/bin/env python3
"""main37 E4 runner: one (round, repetition) per v3_batch process.

Within a repetition all five methods run in one process on one core, in the protocol's
order, with the method order reversed on odd repeats.  Repetitions are independent and are
allowed to run on different cores, which keeps a pathological cell from serialising the
whole round behind 5 x 3600 s.  The warm-up uses a 60 s cap (it is not recorded); every
measured cell keeps the E4 3600 s cap.  The cell file of a round is the concatenation of
its five repetition files; a round is complete only when all 25 cells exist.
"""
import argparse
import json
import os
import queue
import subprocess
import threading
import time

METHODS = ['v3', 'serial', 'masc', 'batchenum', 'vlpq']


def pss_bytes():
    total = 0
    for pid in os.listdir('/proc'):
        if not pid.isdigit():
            continue
        try:
            cmd = open('/proc/%s/cmdline' % pid, 'rb').read()
            if b'v3_batch' not in cmd or b'main37' not in cmd:
                continue
            for line in open('/proc/%s/smaps_rollup' % pid):
                if line.startswith('Pss:'):
                    total += int(line.split()[1]) * 1024
                    break
        except Exception:
            pass
    return total


def monitor(stop, out, interval=5.0):
    peak = 0
    with open(os.path.join(out, 'pss.jsonl'), 'w') as f:
        while not stop.is_set():
            v = pss_bytes()
            peak = max(peak, v)
            f.write(json.dumps({'t': time.time(), 'pss': v, 'peak': peak}) + '\n')
            f.flush()
            stop.wait(interval)
    with open(os.path.join(out, 'pss_peak.json'), 'w') as f:
        json.dump({'peak_pss': peak}, f)


def rep_complete(path):
    if not os.path.exists(path):
        return False
    seen = set()
    for line in open(path):
        try:
            r = json.loads(line)
            seen.add((r.get('method'), r.get('repeat')))
        except Exception:
            pass
    return all((m, 0) in seen for m in METHODS)


def round_complete(path):
    if not os.path.exists(path):
        return False
    seen = set()
    for line in open(path):
        try:
            r = json.loads(line)
            seen.add((r.get('method'), r.get('repeat')))
        except Exception:
            pass
    return all((m, k) in seen for m in METHODS for k in range(5))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--sample', required=True)
    ap.add_argument('--sidecar-root', required=True)
    ap.add_argument('--rounds-root', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--bin', required=True)
    ap.add_argument('--workers', type=int, default=64)
    ap.add_argument('--cpu-start', type=int, default=200)
    ap.add_argument('--mem-gib', type=int, default=10)
    ap.add_argument('--cap', type=int, default=3600)
    ap.add_argument('--warm-cap', type=int, default=60)
    ap.add_argument('--reps', type=int, default=5)
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--dataset-prefix', default='')
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    rows = [json.loads(l) for l in open(args.sample)]
    if args.limit:
        rows = rows[:args.limit]
    tasks = []
    for r in rows:
        rd = os.path.join(args.out, 'round-%s-%05d' % (r['graph'], r['round']))
        r['outdir'] = rd
        if round_complete(os.path.join(rd, 'cells.jsonl')):
            continue
        for k in range(args.reps):
            repdir = os.path.join(rd, 'rep-%d' % k)
            cells = os.path.join(repdir, 'cells.jsonl')
            if not rep_complete(cells):
                tasks.append((r, k, repdir, cells))
    print('rounds', len(rows), 'rep tasks', len(tasks), flush=True)
    stop = threading.Event()
    th = threading.Thread(target=monitor, args=(stop, args.out), daemon=True)
    th.start()
    mem = str(args.mem_gib * (1 << 30))
    lock = threading.Lock()
    finished = []
    cpus = queue.Queue()
    for k in range(args.workers):
        cpus.put(args.cpu_start + k)

    def work(item):
        r, k, repdir, cells = item
        os.makedirs(repdir, exist_ok=True)
        cpu = cpus.get()
        rc = -1
        try:
            lst = os.path.join(repdir, 'one.list')
            with open(lst, 'w') as f:
                f.write(os.path.join(args.rounds_root + r['graph'], 'rounds', r['file']) + '\n')
            cmd = ['taskset', '-c', str(cpu), 'prlimit', '--as=' + mem, '--rss=' + mem,
                   args.bin, '--sidecar', os.path.join(args.sidecar_root, r['graph']),
                   '--list', lst, '--out', cells, '--reps', '1', '--rep-base', str(k),
                   '--warmup', '--warm-cap', str(args.warm_cap), '--cell-cap', str(args.cap),
                   '--run-id', 'main37-' + r['graph'],
                   '--dataset', args.dataset_prefix + r['graph'],
                   '--opponents', 'serial,masc,batchenum,vlpq']
            with open(os.path.join(repdir, 'run.log'), 'w') as lf:
                rc = subprocess.call(cmd, stdout=lf, stderr=subprocess.STDOUT)
        finally:
            cpus.put(cpu)
        with lock:
            finished.append((r['round'], k, rc))
            if len(finished) % 50 == 0 or len(finished) == len(tasks):
                print('done', len(finished), '/', len(tasks), 'rc', rc, time.strftime('%F %T'), flush=True)
        return rc

    import concurrent.futures as cf
    with cf.ThreadPoolExecutor(max_workers=args.workers) as ex:
        list(ex.map(work, tasks))
    stop.set()
    th.join(timeout=10)
    # merge the five repetition files of every round
    for r in rows:
        rd = r['outdir']
        out = os.path.join(rd, 'cells.jsonl')
        recs = {}
        for k in range(args.reps):
            p = os.path.join(rd, 'rep-%d' % k, 'cells.jsonl')
            if not os.path.exists(p):
                continue
            for line in open(p):
                try:
                    x = json.loads(line)
                    recs[(x.get('method'), x.get('repeat'))] = x
                except Exception:
                    pass
        if recs:
            with open(out, 'w') as f:
                for key in sorted(recs, key=lambda z: (z[1] if z[1] is not None else -1, str(z[0]))):
                    f.write(json.dumps(recs[key]) + '\n')
    with open(os.path.join(args.out, 'DONE'), 'w') as f:
        f.write('%d rounds, %d rep tasks, %s\n' % (len(rows), len(tasks), time.strftime('%F %T')))
    print('ALLDONE', len(rows), len(tasks), flush=True)
    return 0


if __name__ == '__main__':
    import sys
    sys.exit(main())
