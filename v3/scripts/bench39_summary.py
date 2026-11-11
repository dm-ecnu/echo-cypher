#!/usr/bin/env python3
"""main37: paired E4 table for the two LLM-era benchmarks.

Per round the five methods ran in one process on one core, five repetitions, method order
reversed on odd repeats.  A round's statistic is the median over its ok repeats.  Ratios are
ECHO/opponent (<1: ECHO faster); the geometric mean is bootstrapped over rounds.  Wins count
rounds whose ECHO median is smaller.  Work uses rounds where both sides report work > 0.
Completion is cell-level ok / (rounds x 5); timeouts and errors stay in the denominator.
Coverage is covered/attempted for the opponent adapter and compiled/8 for ECHO's pool.
"""
import argparse
import collections
import glob
import json
import math
import os
import random
import statistics

METHODS = ['v3', 'serial', 'masc', 'batchenum', 'vlpq']
NAME = {'v3': 'ECHO', 'serial': '逐条', 'masc': 'MASC (port)',
        'batchenum': 'BatchEnum+ (port)', 'vlpq': 'VLPQ'}
REPS = 5


def gmean(xs):
    xs = [x for x in xs if x > 0]
    if not xs:
        return None
    return math.exp(sum(math.log(x) for x in xs) / len(xs))


def boot(xs, rng, n=5000):
    xs = [x for x in xs if x > 0]
    if len(xs) < 2:
        return None
    vals = []
    for _ in range(n):
        s = [xs[rng.randrange(len(xs))] for _ in xs]
        g = gmean(s)
        if g:
            vals.append(g)
    vals.sort()
    return [vals[int(0.025 * len(vals))], vals[int(0.975 * len(vals)) - 1]]


def summarise(by, rounds, sample, rng):
    med = {}
    for r in rounds:
        med[r] = {}
        for m in METHODS:
            xs = [c for c in by[r].get(m, []) if c.get('status') == 'ok']
            if xs:
                med[r][m] = {
                    'time': statistics.median(c['wall_sec'] for c in xs),
                    'work': statistics.median(c.get('work', 0) for c in xs),
                    'rss': statistics.median(c.get('peak_rss_kb', 0) for c in xs) / 1024.0,
                }
    # A port that covers no candidate has no algorithm cell of its own: its row is the same
    # serial fallback (the main30/main35 convention), not a slower re-execution of duplicates.
    for m in ('batchenum', 'vlpq'):
        if not any(c.get('covered_candidates', 0) for r in rounds for c in by[r].get(m, [])):
            for r in rounds:
                if m in med[r] and 'serial' in med[r]:
                    med[r][m] = dict(med[r]['serial'])
    out = {'rounds': len(rounds), 'sample_rounds': len(sample), 'methods': {}}
    for m in METHODS[1:]:
        tr, wr, wins_t, wins_w = [], [], 0, 0
        for r in rounds:
            if 'v3' not in med[r] or m not in med[r]:
                continue
            if med[r][m]['time'] > 0:
                ratio = med[r]['v3']['time'] / med[r][m]['time']
                tr.append(ratio)
                wins_t += 1 if ratio < 1 else 0
            a, b = med[r]['v3']['work'], med[r][m]['work']
            if a > 0 and b > 0:
                wr.append(a / b)
                wins_w += 1 if a / b < 1 else 0

        def block(xs, wins):
            if not xs:
                return None
            return {'gmean': gmean(xs), 'ci': boot(xs, rng), 'median': statistics.median(xs),
                    'wins': wins, 'n': len(xs)}
        allc = [c for r in rounds for c in by[r].get(m, [])]
        okc = [c for c in allc if c.get('status') == 'ok']
        cov = collections.Counter()
        for c in allc:
            cov['covered'] += c.get('covered_candidates', 0)
            cov['attempted'] += c.get('attempted_candidates', 0)
        out['methods'][NAME[m]] = {
            'time': block(tr, wins_t), 'work': block(wr, wins_w),
            'rss_median_mb': statistics.median([c['peak_rss_kb'] for c in okc]) / 1024.0 if okc else None,
            'cells_ok': len(okc), 'cells': len(allc),
            'timeout': sum(1 for c in allc if c.get('timeout')),
            'error': sum(1 for c in allc if c.get('status') == 'error'),
            'completion': len(okc) / (len(sample) * REPS) if sample else 0,
            'covered': cov['covered'], 'attempted': cov['attempted'],
            'coverage_attempted': (cov['covered'] / cov['attempted']) if cov['attempted'] else 0.0,
            'coverage_k8': (cov['covered'] / (8.0 * len(rounds))) if rounds else 0.0,
        }
    e = [c for r in rounds for c in by[r].get('v3', [])]
    eok = [c for c in e if c.get('status') == 'ok']
    out['echo'] = {'cells_ok': len(eok), 'cells': len(e),
                   'timeout': sum(1 for c in e if c.get('timeout')),
                   'error': sum(1 for c in e if c.get('status') == 'error'),
                   'rss_median_mb': statistics.median([c['peak_rss_kb'] for c in eok]) / 1024.0 if eok else None,
                   'coverage_k8': sum(sample[r].get('compiled', 0) for r in rounds) / (8.0 * len(rounds)) if rounds else 0.0}
    return out


def fmt(x):
    if not x:
        return '—'
    ci = x['ci']
    return '%.3f [%.3f,%.3f]' % (x['gmean'], ci[0], ci[1]) if ci else '%.3f' % x['gmean']


def table(out):
    lines = ['| 对手 | ECHO/对手 time | 胜 | 中位 | ECHO/对手 work | 胜 | 中位 | RSS MB E/O | Compl. E/O | Cov. |',
             '|---|---:|---:|---|---:|---:|---|---:|---|---:|']
    for m in METHODS[1:]:
        d = out['methods'][NAME[m]]
        t, w = d['time'], d['work']
        lines.append('| %s | %s | %d/%d | %.3f | %s | %d/%d | %.3f | %.0f/%.0f | %d/%d vs %d/%d | %.1f%% |' % (
            NAME[m], fmt(t), t['wins'] if t else 0, t['n'] if t else 0, t['median'] if t else 0,
            fmt(w), w['wins'] if w else 0, w['n'] if w else 0, w['median'] if w else 0,
            out['echo']['rss_median_mb'] or 0, d['rss_median_mb'] or 0,
            out['echo']['cells_ok'], out['sample_rounds'] * REPS, d['cells_ok'], out['sample_rounds'] * REPS,
            100 * d['coverage_attempted']))
    return '\n'.join(lines) + '\n'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', required=True)
    ap.add_argument('--sample', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--benchmark', required=True)
    ap.add_argument('--coverage', default='')
    args = ap.parse_args()
    sample = {r['family']: r for r in (json.loads(l) for l in open(args.sample))}
    cells = []
    for p in sorted(glob.glob(os.path.join(args.root, 'round-*', 'cells.jsonl'))):
        for line in open(p):
            try:
                cells.append(json.loads(line))
            except Exception:
                pass
    by = collections.defaultdict(lambda: collections.defaultdict(list))
    for c in cells:
        by[c.get('family', c.get('v3_round'))][c['method']].append(c)
    rounds = sorted(by)
    rng = random.Random(0)
    out = {'benchmark': args.benchmark, 'cells': len(cells), 'sample_rounds': len(sample),
           'all': summarise(by, rounds, sample, rng), 'by_graph': {}}
    graphs = sorted({r['graph'] for r in sample.values()})
    for g in graphs:
        rs = [r for r in rounds if sample.get(r, {}).get('graph') == g]
        if rs:
            sub = {f: r for f, r in sample.items() if r.get('graph') == g}
            out['by_graph'][g] = summarise(by, rs, sub, rng)
    if args.coverage and os.path.exists(args.coverage):
        out['pool'] = json.load(open(args.coverage))
    with open(os.path.join(args.out, 'summary.json'), 'w') as f:
        json.dump(out, f, indent=1)
    text = table(out['all'])
    with open(os.path.join(args.out, 'table.md'), 'w') as f:
        f.write(text)
    print(text)
    gtext = []
    for g, d in out['by_graph'].items():
        gtext.append('### %s\n\n%s' % (g, table(d)))
    with open(os.path.join(args.out, 'table_by_graph.md'), 'w') as f:
        f.write('\n'.join(gtext))


if __name__ == '__main__':
    main()
