#!/usr/bin/env python3
import os
ECHO_WORK = os.environ.get("ECHO_WORK", ".")
import pathlib,json,sys,collections,math,statistics,contextlib,io,runpy,datetime
R=pathlib.Path((ECHO_WORK + '-native'));D=pathlib.Path((ECHO_WORK + '-native-data'));C=sys.argv[1];O=D/f'results/ldbc-e4-e5-{C}';p=O/'cells.jsonl'
if not p.exists():raise SystemExit('no formal rows yet')
last={}
for ln in p.read_text().splitlines():
 r=json.loads(ln);last[r['exp'],r['sf'],r['batch'],r['rep'],r['method']]=r
rows=list(last.values());windows=collections.defaultdict(list)
for r in rows:windows[r['exp'],r['sf'],r['batch'],r['rep']].append(r)
for key,rs in windows.items():assert len({(r['core'],r['commit'],r['input_sha256']) for r in rs})==1,key
args=sys.argv;sys.argv=[str(R/'scripts/analyze_cells.py'),str(p),'E4,E5'];output=io.StringIO()
with contextlib.redirect_stdout(output):ns=runpy.run_path(str(R/'scripts/analyze_cells.py'))
sys.argv=args
summary=dict(commit=C,time=datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=8))).isoformat(),raw_rows=sum(1 for _ in p.open()),unique_rows=len(rows),groups=[])
for exp,sf in [('E4',1),('E4',10),('E5',10)]:
 methods=['echo','perquery','masc','gup'] if exp=='E4' else ['echo','echo_noenvelope','echo_nomask'];rs=[r for r in rows if r['exp']==exp and r['sf']==sf];m=json.load(open(D/f'data/workloads/ldbc-sf{sf}-k8-v2/manifest.json'));batchids=range(m['batches']);terminal=sum(all(all((exp,sf,b,rep,method) in last for method in methods) for rep in range(5)) for b in batchids)
 group=dict(exp=exp,sf=sf,graph_vertices=37693800 if sf==1 else 383234701,graph_edges=51768114 if sf==1 else 529870299,batches_total=m['batches'],batches_terminal=terminal,patterns=m['queries_usable'],candidates=m['candidates_usable'],candidates_total=m['candidates_total'],excluded=m['candidates_blocked'],methods={},ratios={})
 for method in methods:
  rr=[r for r in rs if r['method']==method];valid=[r for r in rr if ns['valid'](r)];cens=lambda r:r.get('timeouts',0)>0 or r.get('batch_expired',0)>0 or r['rc']==124
  stats=dict(terminal_repeats=len(rr),total_repeats=5*m['batches'],valid_repeats=len(valid),censored_repeats=sum(cens(r) for r in rr),failed_repeats=sum(not ns['valid'](r) and not cens(r) for r in rr),return_codes=dict(collections.Counter(str(r['rc']) for r in rr)))
  if valid:
   # These descriptive metrics retain the raw batch measurements; time ratios use paired statistics below.
   stats['batch_seconds_median']=statistics.median(ns['t'](r) for r in valid);stats['batch_seconds_mean']=statistics.mean(ns['t'](r) for r in valid);stats['batch_seconds_std']=statistics.stdev(ns['t'](r) for r in valid) if len(valid)>1 else None
   stats['filter_seconds_median']=statistics.median(r.get('filter_sec',0) for r in valid);stats['peak_rss_kb_median']=statistics.median(r['peak_rss_kb'] for r in valid)
   if method=='echo':stats['n6_queries_successful_repeats']=sum(r['bn']-r.get('covered_queries',0) for r in valid)
  group['methods'][method]=stats
 for method in methods[1:]:
  vals=ns['ratios'](exp,f'ldbc-sf{sf}',f'ldbc-sf{sf}-k8-v2',method);s=ns['stat'](vals)
  group['ratios'][method]=dict(geomean=s[0],ci95=[s[1],s[2]],wins=sum(v<1 for v in vals),valid_batches=len(vals),per_batch_ratios=vals) if s else None
 summary['groups'].append(group)
(O/'summary.json').write_text(json.dumps(summary,indent=1));(O/'analysis.txt').write_text(output.getvalue());print(json.dumps(summary,ensure_ascii=False))
