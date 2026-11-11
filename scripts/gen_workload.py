#!/usr/bin/env python3
"""MASC-style multi-query workload generator (SIGMOD'26 Sec. 6.1).

Queries are connected induced subgraphs of the data graph found by random walks, with k = 15
vertices. Each batch of b queries consists of b / f families; a family of size f shares a common
core of k x beta vertices (the common core is sampled by a random walk and every member extends
it by a random walk to k vertices). Overlap size 0 means no common core: every query is an
independent random walk, beta = 0.

Output layout: <out>/batch-BB/qIII.graph in the native t / v / e format and
<out>/manifest.json with the parameters and per-family records (core vertices, measured overlap).

The paper does not publish seeds, so the output is a protocol-matched workload.
"""
import argparse
import hashlib
import json
import random
from pathlib import Path


def read_graph(path):
    adj = {}
    label = {}
    with open(path) as f:
        header = f.readline().split()
        assert header[0] == 't', header
        n, m = int(header[1]), int(header[2])
        for line in f:
            parts = line.split()
            if not parts:
                continue
            if parts[0] == 'v':
                v, lab = int(parts[1]), int(parts[2])
                label[v] = lab
                adj.setdefault(v, [])
            elif parts[0] == 'e':
                a, b = int(parts[1]), int(parts[2])
                adj.setdefault(a, []).append(b)
                adj.setdefault(b, []).append(a)
    assert len(adj) == n, (len(adj), n)
    return adj, label


def induced(adj, chosen):
    s = set(chosen)
    edges = []
    for u in chosen:
        for v in adj[u]:
            if v in s and u < v:
                edges.append((u, v))
    return edges


def walk_extend(adj, rng, start_set, k, max_steps=60000, replace_from=None):
    """Random walk from a vertex of start_set until k distinct vertices are visited."""
    chosen = [rng.choice(sorted(start_set))]
    seen = set(chosen)
    cur = chosen[0]
    for step in range(max_steps):
        if len(chosen) == k:
            return chosen
        nbrs = adj[cur]
        if nbrs:
            r = rng.random()
            if r < 0.75:
                cur = nbrs[rng.randrange(len(nbrs))]
            elif r < 0.9:
                cur = rng.choice(chosen)
        if cur in seen:
            if replace_from is not None and replace_from(cur):
                return None
            continue
        seen.add(cur)
        chosen.append(cur)
    return None


def write_query(path, adj, label, chosen):
    local = {u: i for i, u in enumerate(chosen)}
    edges = sorted((min(local[u], local[v]), max(local[u], local[v])) for u, v in induced(adj, chosen))
    with open(path, 'w') as out:
        out.write(f't {len(chosen)} {len(edges)}\n')
        for u in chosen:
            out.write(f'v {local[u]} {label[u]} {len(adj[u])}\n')
        for a, b in edges:
            out.write(f'e {a} {b}\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--graph', required=True, type=Path)
    ap.add_argument('--out', required=True, type=Path)
    ap.add_argument('--seed', type=int, default=0)
    ap.add_argument('--queries', type=int, default=300)
    ap.add_argument('--batch', type=int, default=30)
    ap.add_argument('--k', type=int, default=15)
    ap.add_argument('--family', type=int, default=5)
    ap.add_argument('--overlap', type=int, default=0)
    args = ap.parse_args()
    if args.queries % args.batch or args.batch % args.family or args.overlap >= args.k:
        ap.error('queries, batches and families are inconsistent')
    if (args.out / 'manifest.json').exists():
        ap.error('output exists; workloads are immutable')
    adj, label = read_graph(args.graph)
    starts = sorted(v for v in adj if adj[v])
    rng = random.Random(args.seed)
    args.out.mkdir(parents=True, exist_ok=True)
    records = []
    index = 0
    for b in range(args.queries // args.batch):
        folder = args.out / f'batch-{b:02d}'
        if folder.exists():  # partial output from an interrupted run
            import shutil
            shutil.rmtree(folder)
        folder.mkdir()
        for fam in range(args.batch // args.family):
            core = None
            if args.overlap > 0:
                for _ in range(100):
                    core = walk_extend(adj, rng, starts, args.overlap)
                    if core is not None:
                        break
                if core is None:
                    raise RuntimeError('could not sample a core')
            family_queries = []
            for _ in range(args.family):
                for attempt in range(50):
                    if core is None:
                        q = walk_extend(adj, rng, starts, args.k)
                        if q is not None:
                            break
                        continue
                    # extend the core by a random walk; the walk may pass through core vertices
                    ext = walk_extend(adj, rng, core, args.k)
                    if ext is None:
                        core = None
                        continue
                    chosen = []
                    for v in core + ext:
                        if v not in chosen:
                            chosen.append(v)
                        if len(chosen) == args.k:
                            break
                    if len(chosen) == args.k:
                        q = chosen
                        break
                else:
                    raise RuntimeError(f'could not sample a query at index {index} (core {core})')
                if q is None or (core is not None and not set(core) <= set(q)):
                    raise RuntimeError(f'could not sample a query at index {index} (core {core})')
                family_queries.append(q)
            for q in family_queries:
                write_query(folder / f'q{index:03d}.graph', adj, label, q)
                index += 1
            chosen_list = family_queries
            common = set(chosen_list[0])
            for q in chosen_list[1:]:
                common &= set(q)
            pair = [len(set(chosen_list[i]) & set(chosen_list[j])) for i in range(len(chosen_list))
                    for j in range(i + 1, len(chosen_list))]
            rec = {'batch': b, 'family': fam, 'core': core, 'common': len(common),
                   'pair_min': min(pair), 'pair_max': max(pair)}
            # Non-core vertices of the walk can coincide inside one member (a random walk may
            # close a cycle); such a member then shares fewer than k x beta vertices with the
            # family and the core size is an upper bound of the realised overlap.
            rec['family_min_common'] = min(rec['common'], rec['pair_min'])
            records.append(rec)
    manifest = {
        'graph': str(args.graph),
        'graph_sha256': hashlib.sha256(args.graph.read_bytes()).hexdigest(),
        'seed': args.seed, 'queries': args.queries, 'batch_size': args.batch,
        'k': args.k, 'family_size': args.family, 'overlap_vertices': args.overlap,
        'protocol': 'MASC SIGMOD 2026 Sec. 6.1 (random walk induced subgraphs, common core k x beta)',
        'families': records,
    }
    (args.out / 'manifest.json').write_text(json.dumps(manifest, indent=1) + '\n')
    print(json.dumps({k: v for k, v in manifest.items() if k != 'families'}))


if __name__ == '__main__':
    main()
