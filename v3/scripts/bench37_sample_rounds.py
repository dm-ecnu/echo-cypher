#!/usr/bin/env python3
"""main37: fixed-seed stratified round sample for CypherBench.

The full pool is 11,740 rounds (2,348 questions x 5 samplings).  The 50-round-per-graph
probe found a heavy tail (company 3/50 and geography 1/50 cells over 60 s, which the E4
protocol would hold to 3600 s); the full matrix extrapolates beyond 24 h on the planned
64-shard layout.  This script draws N >= 1000 rounds, proportional to the written rounds of
each graph, with random.Random(0) over the sorted round ids.  Every round keeps all five
method repetitions and every method; only the set of rounds is sampled.
"""
import argparse
import collections
import glob
import json
import os
import random


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--n', type=int, default=1200)
    ap.add_argument('--seed', type=int, default=0)
    args = ap.parse_args()
    by_graph = {}
    for p in sorted(glob.glob(os.path.join(args.root, 'rounds-*', 'rounds.jsonl'))):
        g = os.path.basename(os.path.dirname(p)).split('rounds-', 1)[1]
        rows = [json.loads(l) for l in open(p)]
        by_graph[g] = rows
    written = {g: len(v) for g, v in by_graph.items()}
    total = sum(written.values())
    raw = {g: args.n * written[g] / total for g in written}
    alloc = {g: int(raw[g]) for g in written}
    rem = args.n - sum(alloc.values())
    for g in sorted(written, key=lambda x: (-(raw[x] - alloc[x]), x))[:rem]:
        alloc[g] += 1
    chosen = []
    rng = random.Random(args.seed)
    for g in sorted(written):
        rows = sorted(by_graph[g], key=lambda r: r['round'])
        k = min(alloc[g], len(rows))
        for r in rng.sample(rows, k):
            chosen.append({'graph': g, 'round': r['round'], 'family': r['family'],
                           'file': r['file'], 'qid': r['qid'], 'rep': r['rep'],
                           'compiled': r['compiled'], 'refused': r['refused']})
    chosen.sort(key=lambda r: (r['graph'], r['round']))
    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, 'sample.jsonl'), 'w') as f:
        for r in chosen:
            f.write(json.dumps(r) + '\n')
    meta = {'benchmark': 'cypherbench', 'pool_rounds': total, 'written': written,
            'sample_n': len(chosen), 'allocation': alloc, 'seed': args.seed,
            'rule': 'proportional stratified by graph, random.Random(0).sample over sorted round ids',
            'rounds_jsonl_sha256': {
                g: __import__('hashlib').sha256(
                    open(os.path.join(args.root, 'rounds-' + g, 'rounds.jsonl'), 'rb').read()).hexdigest()
                for g in written}}
    with open(os.path.join(args.out, 'sample_meta.json'), 'w') as f:
        json.dump(meta, f, indent=1)
    print(json.dumps(meta, indent=1))


if __name__ == '__main__':
    main()
