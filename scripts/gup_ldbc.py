#!/usr/bin/env python3
"""Existing official GuP batch contract with shared predicate input adaptation.
Data graph conversion is streamed once before the matrix, never inside timing.
Official search/filter algorithms remain unchanged except initial domain intersection.
"""
import argparse,json,pathlib,subprocess,os,resource,time,tempfile
p=argparse.ArgumentParser();p.add_argument('--graph',required=True);p.add_argument('--workload',required=True);p.add_argument('--batch',type=int,required=True);p.add_argument('--gup',required=True);p.add_argument('--timeout',type=float,default=120);p.add_argument('--batch_deadline',type=float,default=3600);a=p.parse_args()
w=pathlib.Path(a.workload);m=json.load(open(w/'manifest.json'))
batches=json.load(open(w/'batch_index.json'))['batches'];idx=batches[a.batch]['indices'];rec=json.load(open(w/'queries.json'));pred=json.load(open(w/'predicate-index.json'))
qs=[]
for i in idx:
 q=rec[i]['query'];allowed=[None]*q['n']
 for anchor in q['anchors']:
  u=anchor['vertex'];vs=set(pred[anchor['predicate_key']]);allowed[u]=vs if allowed[u] is None else allowed[u]&vs
 qs.append(dict(vertices=q['labels'],edges=q['edges'],allowed=[None if vs is None else sorted(vs) for vs in allowed]))
# JSON is a YAML subset accepted by official serde_yaml.
with tempfile.NamedTemporaryFile(mode='w',suffix='.yaml',dir=os.environ.get('TMPDIR','/tmp')) as f:
 json.dump(qs,f);f.flush()
 cmd=[a.gup,'--graph',a.graph+'.gup','--match-limit',str(2**64-1),'--parallelism','1','--timeout',str(int(a.timeout)),f.name]
 t=time.monotonic();r=subprocess.run(cmd,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=dict(os.environ,RUST_LOG='error'))
 print(r.stdout,flush=True)
 if r.returncode:print(r.stderr,flush=True);raise SystemExit(r.returncode)
 # Retain the official per-query timing fields, parse its stable flat YAML.
 import re
 rows=[];whole=None
 for line in r.stdout.splitlines():
  mt=re.match(r'^- index: (\d+)$',line)
  if mt:rows.append({'slot':int(mt[1])})
  mt=re.match(r'^  (match_count|search_sec): (.+)$',line)
  if mt:rows[-1][mt[1]]=float(mt[2]) if mt[1]=='search_sec' else int(mt[2])
  if line.startswith('whole_sec:'):whole=float(line.split(':')[1])
 assert len(rows)==len(idx) and whole is not None,'official GuP incomplete result'
 print('ECHO_STATS_BEGIN\nmethod=gup');timeouts=0
 for row in rows:
  timed=int(row['search_sec']>=a.timeout*.999);timeouts+=timed
  print(f"query_index={idx[row['slot']]}\nembeddings={row['match_count']}\nfilter_sec=0\nquery_sec={row['search_sec']}\ntimeout={timed}\nrecord_end=1")
 print(f'queries_done={len(idx)}\ntimeouts={timeouts}\nbatch_sec={whole}\nfilter_sec=0\npeak_rss_kb={resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss}\nECHO_STATS_END')
