#!/usr/bin/env python3
"""main37: freeze the Text2GraphQuery-Bench Cypher subset and its one-time K=8 x 5 samples.

The 200-question set is a fixed-seed (0) proportional stratified sample by database.
The prompt is the CypherBench official zero-shot prompt used by the LDBC frozen pool
(`baselines/prompting.py`), temperature 0.7, K=8 independent calls, five independent
repetitions; each repetition is frozen as one candidate round.  No candidate is filtered by
its output or by execution.

The script appends one JSONL row per completed round, so an interrupted run resumes from the
rows already on disk.  Re-running never re-samples a round that is already frozen.
"""
import os
ECHO_WORK = os.environ.get("ECHO_WORK", ".")
import argparse
import concurrent.futures as cf
import hashlib
import json
import os
import random
import re
import sys
import threading
import time

import requests

BASE = (ECHO_WORK + '/public-benchmarks/text2graphquery-neo4j-v1')
QDIR = (ECHO_WORK + '/public-benchmarks/normalized-v1/text2graphquery/questions')
GRAPHS = ['t2gq_financial_payment', 't2gq_information_technology_it_asset_management',
          't2gq_knowledge_graph_geography', 't2gq_manufacturing_bom_bill_of_materials',
          't2gq_manufacturing_production_process', 't2gq_social_network_twitter']
PROMPT = """Translate the question to Cypher query based on the schema of a Neo4j knowledge graph.
- Output the Cypher query in a single line, without any additional output or explanation. Do not wrap the query with any formatting like ```.
- Perform graph pattern matching in the `MATCH` clause if possible.
- Avoid listing the same entity multiple times in the results. However, if multiple distinct entities share the same name, their names should be repeated as separate entries.
- Do not return node objects. Instead, return entity names or properties.

Graph Schema:
{schema}

Question: {question}
Cypher: """
TYPE = {'STRING': 'str', 'INTEGER': 'int', 'INT': 'int', 'FLOAT': 'float', 'DOUBLE': 'float',
        'BOOLEAN': 'bool', 'BOOL': 'bool', 'DATE': 'date', 'DATETIME': 'date', 'TIME': 'date'}
LOCK = threading.Lock()


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def schema_str(graph):
    d = json.load(open(os.path.join(BASE, 'schemas', graph + '_schema.json')))
    ents = []
    for e in sorted(d['entities'], key=lambda x: x['label']):
        props = {}
        for p, t in sorted(e.get('properties', {}).items()):
            props[p] = TYPE.get(str(t).upper(), 'str')
        ents.append({'label': e['label'], 'properties': props})
    rels = []
    for r in sorted(d.get('relations', []), key=lambda x: (x['label'], x['subj_label'], x['obj_label'])):
        props = {}
        for p, t in sorted(r.get('properties', {}).items()):
            props[p] = TYPE.get(str(t).upper(), 'str')
        rels.append({'label': r['label'], 'subj_label': r['subj_label'],
                     'obj_label': r['obj_label'], 'properties': props})
    obj = {'name': d.get('name', graph), 'entities': ents, 'relations': rels}
    return json.dumps(obj, indent=2, sort_keys=False)


def strip_code(s):
    s = (s or '').strip()
    if s.startswith('```'):
        s = re.sub(r'^```[A-Za-z0-9_+-]*\s*', '', s)
        s = re.sub(r'\s*```$', '', s)
    return s.strip()


def sample_set(seed=0, n=200):
    rows = {}
    for g in GRAPHS:
        qs = json.load(open(os.path.join(QDIR, g + '.json')))
        rows[g] = qs
    total = sum(len(v) for v in rows.values())
    raw = {g: n * len(rows[g]) / total for g in GRAPHS}
    alloc = {g: int(raw[g]) for g in GRAPHS}
    rem = n - sum(alloc.values())
    order = sorted(GRAPHS, key=lambda g: (-(raw[g] - alloc[g]), g))
    for g in order[:rem]:
        alloc[g] += 1
    chosen = []
    rng = random.Random(seed)
    for g in GRAPHS:
        qs = sorted(rows[g], key=lambda q: q['qid'])
        k = min(alloc[g], len(qs))
        chosen += rng.sample(qs, k)
    return chosen, alloc, total


def call(client, schema, question, model, temperature, retries=4):
    body = {'model': model, 'messages': [{'role': 'user',
                                          'content': PROMPT.format(schema=schema, question=question)}],
            'temperature': temperature, 'max_tokens': 1024}
    last = None
    for a in range(retries):
        try:
            r = client.post('http://127.0.0.1:4000/v1/chat/completions', json=body, timeout=240,
                            headers={'Authorization': 'Bearer ' + os.environ['LLM_API_KEY']})
            if r.status_code == 200:
                j = r.json()
                txt = j['choices'][0]['message'].get('content') or ''
                return strip_code(txt), None, r.status_code, j.get('usage', {})
            last = 'HTTP %d %s' % (r.status_code, r.text[:200])
        except Exception as e:
            last = type(e).__name__ + ': ' + str(e)[:200]
        time.sleep(1.5 * (a + 1))
    return '', last, None, {}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('--model', default='local-qwen')
    ap.add_argument('--temperature', type=float, default=0.7)
    ap.add_argument('--K', type=int, default=8)
    ap.add_argument('--reps', type=int, default=5)
    ap.add_argument('--seed', type=int, default=0)
    ap.add_argument('--n', type=int, default=200)
    ap.add_argument('--workers', type=int, default=24)
    ap.add_argument('--limit', type=int, default=0)
    args = ap.parse_args()
    questions, alloc, total = sample_set(args.seed, args.n)
    if args.limit:
        questions = questions[:args.limit]
    os.makedirs(args.out, exist_ok=True)
    dst = os.path.join(args.out, 'samples.jsonl')
    done = set()
    if os.path.exists(dst):
        for line in open(dst):
            try:
                r = json.loads(line)
                done.add((r['qid'], r['rep']))
            except Exception:
                pass
    schemas = {g: schema_str(g) for g in GRAPHS}
    manifest = {'benchmark': 'text2graphquery', 'seed': args.seed, 'n': len(questions),
                'allocation': alloc, 'available': total, 'K': args.K, 'reps': args.reps,
                'model': args.model, 'temperature': args.temperature,
                'prompt': 'CypherBench NL2CYPHER_PROMPT_DEFAULT (same structure as the LDBC pool)',
                'schema_source': os.path.join(BASE, 'schemas'),
                'question_sources': {g: {'path': os.path.join(QDIR, g + '.json'),
                                         'sha256': sha256(os.path.join(QDIR, g + '.json'))}
                                     for g in GRAPHS},
                'qids': [q['qid'] for q in questions],
                'sampling_rule': 'proportional stratified by database, random.Random(0).sample(sorted qids)'}
    with open(os.path.join(args.out, 'sample_manifest.json'), 'w') as f:
        json.dump(manifest, f, indent=1)
    tasks = []
    for q in questions:
        for rep in range(args.reps):
            if (q['qid'], rep) not in done:
                tasks.append((q, rep))
    print('questions', len(questions), 'todo rounds', len(tasks), 'done', len(done), flush=True)
    client = requests.Session()
    lock = threading.Lock()

    def one(task):
        q, rep = task
        t0 = time.time()
        cands = []
        for i in range(args.K):
            txt, err, code, usage = call(client, schemas[q['graph']], q['nl_question'],
                                         args.model, args.temperature)
            cands.append({'i': i, 'cypher': txt, 'error': err, 'http': code, 'usage': usage})
        row = {'qid': q['qid'], 'graph': q['graph'], 'family': 't2gq_%s_%s_r%d' % (q['graph'], q['qid'], rep),
               'rep': rep, 'seed': rep, 'K': args.K, 'model': args.model,
               'temperature': args.temperature, 'template': 'text2cypher',
               'nl_question': q['nl_question'], 'gold_cypher': q.get('gold_cypher'),
               'question_sha256': hashlib.sha256(json.dumps(q, sort_keys=True).encode()).hexdigest(),
               'schema_sha256': hashlib.sha256(schemas[q['graph']].encode()).hexdigest(),
               'cand_log': cands, 'seconds': round(time.time() - t0, 3),
               'sampled_at': time.strftime('%F %T')}
        with lock:
            with open(dst, 'a') as f:
                f.write(json.dumps(row, ensure_ascii=False) + '\n')
        return len([c for c in cands if c['error'] is None])

    ok = 0
    with cf.ThreadPoolExecutor(max_workers=args.workers) as ex:
        for n, k in enumerate(ex.map(one, tasks), 1):
            ok += k
            if n % 20 == 0:
                print('rounds', n, '/', len(tasks), 'ok calls', ok, time.strftime('%F %T'), flush=True)
    print('DONE rounds', len(tasks), 'ok calls', ok, 'of', len(tasks) * args.K, flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
