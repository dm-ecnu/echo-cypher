#!/usr/bin/env python3
"""main37: compile a frozen candidate pool into v3 rounds and report coverage/redundancy.

One round = one (question, repetition) K=8 candidate set.  Each candidate is compiled from
its text with the existing LDBC compiler (v3_builder.py).  A candidate the compiler refuses is
a coverage fact and is not silently approximated; it is absent from the round (the Fallback
row) and counted by reason.

The redundancy JSON uses the same seven edit categories as the paper's Fig. 5 on LDBC.  For
every unordered pair of compiled candidates in a round we compare canonical plan signatures and
count the pair under each category that differs; a pair may differ in several categories.
"""
import argparse
import collections
import hashlib
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
import v3_builder as B


def plan_sig(q):
    labels = {}
    for v in q['variables']:
        if v['kind'] == 'vertex':
            labels[v['name']] = tuple(sorted(v['labels']))
    nodes = sorted(labels.values())
    ekey = lambda e: (tuple(sorted(labels.get(e['from'], ()))),
                      tuple(sorted(labels.get(e['to'], ()))), tuple(sorted(e['types'])),
                      e.get('dir', '->'), int(e.get('lo', 1)), int(e.get('hi', 1)))
    edges = sorted(ekey(e) for e in q['edges'])
    edge_dir = sorted((e.get('dir', '->'), int(e.get('lo', 1)), int(e.get('hi', 1)))
                      for e in q['edges'])
    hop = sorted((int(e.get('lo', 1)), int(e.get('hi', 1))) for e in q['edges'])
    opt = sum(1 for c in q['clauses'] if c.get('kind') == 'OPTIONAL_MATCH')
    pred_const = collections.Counter()
    pred_shape = collections.Counter()
    for a in q.get('where', []):
        cs = tuple(sorted((str(x) for x in a.get('consts', [])), key=str))
        pred_shape[(a.get('var', ''), a.get('prop', ''), a.get('op', ''), cs)] += 1
        pred_const[(a.get('prop', ''), a.get('op', ''), cs)] += 1
    items = collections.Counter()
    for it in q.get('items', []):
        if it.get('kind') in ('key', 'agg'):
            items[(it.get('kind'), it.get('var', ''), it.get('prop', ''), it.get('func', ''),
                   bool(it.get('distinct')))] += 1
    return {'nodes': tuple(nodes), 'edges': tuple(edges), 'edge_dir': tuple(edge_dir),
            'hop': tuple(hop), 'optional': opt, 'pred_const': pred_const,
            'pred_shape': pred_shape, 'items': items}


def diff_categories(a, b):
    out = set()
    if a['nodes'] != b['nodes']:
        out.add('node_label')
    na, nb = len(a['edges']), len(b['edges'])
    if na != nb:
        out.add('edge_added' if nb > na else 'edge_deleted')
    if a['edge_dir'] != b['edge_dir']:
        out.add('direction')
    if a['hop'] != b['hop']:
        out.add('hop_range')
    if a['optional'] != b['optional']:
        out.add('optional')
    if a['pred_const'] != b['pred_const']:
        out.add('predicate_constant')
    if a['items'] != b['items']:
        out.add('group_key')
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pool', required=True)
    ap.add_argument('--labels', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--min-queries', type=int, default=1)
    ap.add_argument('--benchmark', required=True)
    args = ap.parse_args()
    manifest = json.load(open(args.labels))
    recs = [json.loads(l) for l in open(args.pool)]
    by_family = collections.OrderedDict()
    for r in recs:
        by_family.setdefault(r['family'], []).append(r)
    os.makedirs(args.out, exist_ok=True)
    rdir = os.path.join(args.out, 'rounds')
    os.makedirs(rdir, exist_ok=True)
    reasons = collections.Counter()
    by_graph = collections.defaultdict(lambda: collections.Counter())
    by_template = collections.defaultdict(collections.Counter)
    rounds = []
    total = compiled_unique = compiled_positions = unique_texts = 0
    pairs = 0
    red = collections.Counter()
    red_by_graph = collections.defaultdict(collections.Counter)
    red_rounds = 0
    for ri, (fam, rows) in enumerate(by_family.items()):
        total += len(rows)
        by_graph[rows[0]['graph']]['candidates'] += len(rows)
        by_template[rows[0].get('template', '')]['candidates'] += len(rows)
        seen = {}
        plans = []
        why = collections.Counter()
        for r in rows:
            t = r['text']
            if t in seen:
                if seen[t] is not None:
                    compiled_positions += 1
                continue
            unique_texts += 1
            try:
                p = B.build(t, '%s#%d' % (fam, r['candidate']), fam, r.get('template', ''), manifest)
            except B.Refuse as e:
                why[str(e).split(':')[0]] += 1
                seen[t] = None
                continue
            except Exception as e:
                why['internal:' + type(e).__name__] += 1
                seen[t] = None
                continue
            seen[t] = p
            plans.append(p)
            compiled_positions += 1
        compiled_unique += len(plans)
        if len(plans) < args.min_queries:
            reasons.update(why)
            by_graph[rows[0]['graph']]['refused'] += len(rows) - sum(1 for v in seen.values() if v is not None)
            continue
        reasons.update(why)
        by_graph[rows[0]['graph']]['compiled'] += len(plans)
        by_graph[rows[0]['graph']]['compiled_positions'] += sum(1 for v in seen.values() if v is not None)
        by_graph[rows[0]['graph']]['rounds'] += 1
        by_template[rows[0].get('template', '')]['rounds'] += 1
        rd = {'family': fam, 'template': rows[0].get('template', ''), 'seed': rows[0].get('seed', 0),
              'queries': plans}
        with open(os.path.join(rdir, 'round-%05d.json' % len(rounds)), 'w') as f:
            json.dump(rd, f, ensure_ascii=False)
        rounds.append({'round': len(rounds), 'file': 'round-%05d.json' % len(rounds),
                       'family': fam, 'graph': rows[0]['graph'], 'qid': rows[0].get('qid'),
                       'rep': rows[0].get('rep'), 'compiled': len(plans),
                       'refused': len(rows) - len(plans), 'reasons': dict(why)})
        if len(plans) >= 2:
            sigs = [plan_sig(p) for p in plans]
            red_rounds += 1
            for i in range(len(sigs)):
                for j in range(i + 1, len(sigs)):
                    pairs += 1
                    cats = diff_categories(sigs[i], sigs[j])
                    for c in cats:
                        red[c] += 1
                        red_by_graph[rows[0]['graph']][c] += 1
    with open(os.path.join(args.out, 'rounds.jsonl'), 'w') as f:
        for r in rounds:
            f.write(json.dumps(r) + '\n')
    if args.benchmark == 'cypherbench':
        cand = total
    else:
        cand = total
    cov = {'benchmark': args.benchmark, 'pool': os.path.abspath(args.pool),
           'pool_sha256': __import__('hashlib').sha256(open(args.pool, 'rb').read()).hexdigest(),
           'labels': os.path.abspath(args.labels),
           'candidates': cand, 'unique_texts': unique_texts,
           'compiled': compiled_positions, 'compiled_unique': compiled_unique,
           'refused': cand - compiled_positions,
           'cqplus_entered': compiled_positions, 'fallback_refused': cand - compiled_positions,
           'rounds_input': len(by_family), 'rounds_written': len(rounds),
           'rounds_ge2': sum(1 for r in rounds if r['compiled'] >= 2),
           'reasons': reasons.most_common(),
           'by_graph': {g: dict(c) for g, c in by_graph.items()},
           'by_template': {t: dict(c) for t, c in by_template.items()}}
    with open(os.path.join(args.out, 'coverage.json'), 'w') as f:
        json.dump(cov, f, indent=1)
    redj = {'benchmark': args.benchmark, 'rounds_with_pairs': red_rounds, 'pairs': pairs,
            'categories': {k: red[k] for k in ('node_label', 'direction', 'hop_range', 'optional',
                                               'predicate_constant', 'group_key', 'edge_added',
                                               'edge_deleted')},
            'by_graph': {g: dict(c) for g, c in red_by_graph.items()},
            'rule': 'unordered candidate pairs per round; a pair is counted once per differing category'}
    with open(os.path.join(args.out, 'redundancy.json'), 'w') as f:
        json.dump(redj, f, indent=1)
    print(json.dumps({'candidates': cand, 'compiled': compiled_positions,
                      'compiled_unique': compiled_unique,
                      'coverage': round(compiled_positions / cand, 4) if cand else 0,
                      'rounds_input': len(by_family), 'rounds_written': len(rounds),
                      'rounds_ge2': cov['rounds_ge2'], 'pairs': pairs,
                      'top_reasons': reasons.most_common(12)}, indent=1))


if __name__ == '__main__':
    sys.exit(main())
