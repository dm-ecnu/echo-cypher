#!/usr/bin/env python3
"""main37: freeze the candidate pools of the two LLM-era benchmarks.

cypherbench: reuse the v2 frozen K=8 vote pool (`sys/runs/eval/vote-K8-qwenlp`), one row per
(question, repetition), 8 candidates in `cand_log` order.  No new sampling and no filtering by
model output.

t2gq: read the frozen jsonl written by bench37_sample_t2gq.py and emit the same record shape.

Every pool record: {family, graph, template, seed, candidate, text, qid, rep, source}.
"""
import argparse
import hashlib
import json
import os
import sys

GRAPHS = ['nba', 'fictional_character', 'flight_accident', 'company', 'geography', 'movie', 'politics']


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def cypherbench(pool_dir, test_json, out):
    test = json.load(open(test_json))
    by_qid = {q['qid']: q for q in test}
    records = []
    src_sha = {}
    files = []
    for g in GRAPHS:
        fs = sorted(__import__('glob').glob(os.path.join(pool_dir, g, 'n*_rep*.jsonl')))
        if not fs:
            raise SystemExit('no pool files for ' + g)
        files += fs
        for p in fs:
            src_sha[p] = sha256(p)
            for line in open(p):
                if not line.strip():
                    continue
                r = json.loads(line)
                qid = r['qid']
                q = by_qid.get(qid)
                if q is None:
                    raise SystemExit('qid not in test.json: ' + qid)
                if q['graph'] != g:
                    raise SystemExit('graph mismatch %s %s %s' % (qid, q['graph'], g))
                fam = 'cb_%s_%s_r%d' % (g, qid, int(r['rep']))
                cands = r.get('cand_log') or []
                if len(cands) != 8:
                    raise SystemExit('K != 8 at %s: %d' % (qid, len(cands)))
                for i, c in enumerate(cands):
                    records.append({'family': fam, 'graph': g, 'template': 'text2cypher',
                                    'seed': int(r['rep']), 'candidate': i,
                                    'text': c['cypher'], 'qid': qid, 'rep': int(r['rep']),
                                    'sample_error': c.get('error')})
    os.makedirs(out, exist_ok=True)
    dst = os.path.join(out, 'pool.jsonl')
    with open(dst, 'w') as f:
        for r in records:
            f.write(json.dumps(r, ensure_ascii=False) + '\n')
    meta = {'benchmark': 'cypherbench', 'pool_dir': os.path.abspath(pool_dir),
            'test_json': os.path.abspath(test_json), 'test_sha256': sha256(test_json),
            'source_files': src_sha, 'n_source_files': len(files),
            'questions': len({r['qid'] for r in records}),
            'rounds': len({r['family'] for r in records}), 'records': len(records),
            'K': 8, 'repetitions': 5, 'graphs': GRAPHS,
            'pool_sha256': sha256(dst)}
    with open(os.path.join(out, 'pool_meta.json'), 'w') as f:
        json.dump(meta, f, indent=1)
    print(json.dumps({k: meta[k] for k in ('questions', 'rounds', 'records', 'n_source_files')}))
    print('pool', dst, meta['pool_sha256'])
    return meta


def t2gq(frozen, out):
    rows = [json.loads(l) for l in open(frozen)]
    recs = []
    for r in rows:
        cands = r.get('cand_log') or []
        if len(cands) != 8:
            raise SystemExit('K != 8 at %s: %d' % (r['qid'], len(cands)))
        for i, c in enumerate(cands):
            recs.append({'family': r['family'], 'graph': r['graph'], 'template': 'text2cypher',
                         'seed': int(r.get('rep', r.get('seed', 0))), 'candidate': i,
                         'text': c['cypher'], 'qid': r['qid'], 'rep': int(r.get('rep', 0)),
                         'sample_error': c.get('error')})
    os.makedirs(out, exist_ok=True)
    dst = os.path.join(out, 'pool.jsonl')
    with open(dst, 'w') as f:
        for r in recs:
            f.write(json.dumps(r, ensure_ascii=False) + '\n')
    meta = {'benchmark': 'text2graphquery', 'frozen': os.path.abspath(frozen),
            'frozen_sha256': sha256(frozen), 'records': len(recs),
            'questions': len({r['qid'] for r in recs}),
            'rounds': len({r['family'] for r in recs}), 'K': 8, 'repetitions': 5,
            'pool_sha256': sha256(dst)}
    with open(os.path.join(out, 'pool_meta.json'), 'w') as f:
        json.dump(meta, f, indent=1)
    print(json.dumps(meta, indent=1))


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    a = sub.add_parser('cypherbench')
    a.add_argument('--pool-dir', required=True)
    a.add_argument('--test-json', required=True)
    a.add_argument('--out', required=True)
    b = sub.add_parser('t2gq')
    b.add_argument('--frozen', required=True)
    b.add_argument('--out', required=True)
    args = ap.parse_args()
    if args.cmd == 'cypherbench':
        cypherbench(args.pool_dir, args.test_json, args.out)
    else:
        t2gq(args.frozen, args.out)


if __name__ == '__main__':
    main()
