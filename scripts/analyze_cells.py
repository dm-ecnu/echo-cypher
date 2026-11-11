#!/usr/bin/env python3
"""Paired batch ratios from a cells.jsonl ledger.

For every (graph, workload, method) cell, keep the last row per (exp, graph, workload,
method, rep, b0, bn) (append-only ledger), drop censored rows, compute per-batch geometric
mean over the 5 repetitions, then the geometric mean over the 10 batches; bootstrap the
batches 5000 times (seed 12345) for the 95% CI and report the win proportion (ratio < 1).

Usage: analyze_cells.py LEDGER [E1,E2,E5,E125,KS ...]
"""
import json, math, random, sys, collections

path = sys.argv[1] if len(sys.argv) > 1 else 'results/cells.jsonl'
wanted = sys.argv[2].split(',') if len(sys.argv) > 2 else ['E1', 'E2', 'E5', 'KS']
rows = [json.loads(l) for l in open(path)]
last = {}
for r in rows:
    k = (r.get('exp'), r.get('graph'), r.get('workload'), r.get('method'), r.get('rep'),
         r.get('b0'), r.get('bn'))
    last[k] = r
def valid(r):
    return (r.get('rc') == 0
            and (r.get('queries_done') is None or r.get('queries_done') == r.get('bn'))
            and r.get('timeouts', 0) == 0 and r.get('batch_sec', 0) > 0)
rows = [r for r in last.values() if valid(r)]
# An ECHO row's batch_sec covers the timed repetition loop; N1-N2 grouping (family_sec) runs once
# before it and is charged to the method here. Baseline rows carry no family_sec.
def t(r):
    return r['batch_sec'] + (r.get('family_sec') or 0)
idx = {(r['exp'], r['graph'], r['workload'], r['method'], r['rep'], r['b0']): r for r in rows}

def stat(vals):
    if not vals:
        return None
    gm = math.exp(sum(math.log(x) for x in vals) / len(vals))
    win = sum(1 for x in vals if x < 1) / len(vals)
    random.seed(12345)
    bs = []
    for _ in range(5000):
        s = [vals[random.randrange(len(vals))] for _ in vals]
        bs.append(math.exp(sum(math.log(x) for x in s) / len(s)))
    bs.sort()
    return gm, bs[int(0.025 * len(bs))], bs[int(0.975 * len(bs)) - 1], win, len(vals)

def ratios(exp, g, w, meth):
    per_batch = collections.defaultdict(list)
    for (e, gg, ww, m, rep, b0), r in idx.items():
        if e != exp or gg != g or ww != w or m != 'echo':
            continue
        o = idx.get((e, gg, ww, meth, rep, b0))
        if o and o['batch_sec'] > 0:
            per_batch[b0].append(t(r) / t(o))
    vals = []
    for b0, rs in per_batch.items():
        if len(rs) >= 3:
            vals.append(math.exp(sum(math.log(x) for x in rs) / len(rs)))
    return vals

def graph_of(w):
    return w.split('-')[0]

for exp in wanted:
    print('==', exp)
    keys = sorted({(r['graph'], r['workload']) for r in rows if r['exp'] == exp})
    if exp == 'E125':
        keys = sorted({(r['graph'], r['workload']) for r in rows if r['exp'] == 'E125'})
    for g, w in keys:
        ov = w.split('-o')[1].split('-')[0] if '-o' in w else ''
        for meth in ['perquery', 'masc', 'gup', 'echo_noenvelope', 'echo_nomask']:
            vals = ratios(exp, g, w, meth)
            s = stat(vals)
            if s:
                print(f"{g:8s} {w:38s} o{ov:>2s} vs {meth:16s} n={s[4]:2d} "
                      f"gm={s[0]:.3f} [{s[1]:.3f},{s[2]:.3f}] win={s[3]:.2f}")
