import os
ECHO_WORK = os.environ.get("ECHO_WORK", ".")
TMPDIR = os.environ.get("TMPDIR", "/tmp")
import pathlib,json,subprocess,collections,time,sys,datetime
R=pathlib.Path((ECHO_WORK + '-native'));D=pathlib.Path((ECHO_WORK + '-native-data'));T=pathlib.Path((TMPDIR + '/ldbc-e4-e5-input'));B=pathlib.Path((TMPDIR + '/echo-build/tools'));frozen=json.load(open(T/'frozen.json'));C=frozen['commit'];w=D/'data/workloads/ldbc-sf1-k8-v2';O=D/f'results/ldbc-e4-e5-{C}/sf1-gate';O.mkdir(parents=True,exist_ok=True)
q=json.load(open(w/'queries.json'));ids=json.load(open(w/'batch_index.json'))['batches'][0]['indices'];pred=json.load(open(w/'predicate-index.json'));original=D/'results/e4-e34a10f/sf1-gate/perquery.out'
plain=[]
for line in original.read_text().splitlines():
 a=line.split()
 if len(a)==4 and all(x.isdigit() for x in a):plain.append(list(map(int,a)))
assert len(plain)==9892
expected={}
for i in ids:
 assert q[i]['query']['labels']==q[0]['query']['labels'] and q[i]['query']['edges']==q[0]['query']['edges'], 'first batch changed structural variant; needs its full plain reference'
 enc=q[i]['query'];truth=[sol for sol in plain if all(sol[a['vertex']] in pred[a['predicate_key']] for a in enc['anchors'])];assert len(truth)==1
 expected[i]=truth
assert expected[0]==[[8933,3157083,3561328,3561329]],expected[0] # concrete trusted original-anchor embedding

def parse(path):
 recs=[];cur={};stats={};on=False;sol=[]
 for line in path.read_text().splitlines():
  if line=='ECHO_STATS_BEGIN':on=True;continue
  if line=='ECHO_STATS_END':on=False;continue
  if on and '=' in line:
   k,v=line.split('=',1);stats[k]=v;cur[k]=v
   if k=='record_end':recs.append(cur);cur={}
  elif not on:
   x=line.split()
   if len(x)==4 and all(a.isdigit() for a in x):sol.append(list(map(int,x)))
 return stats,recs,sol
peak={};result={}
for method in ['perquery','masc','echo','gup']:
 cmd=[str(B/(('echo_main' if method=='echo' else method)+'-'+C)),'--graph',str(D/'data/graphs/ldbc-sf1.graph'),'--workload',str(w),'--batch','0','--timeout','120','--batch_deadline','3600']
 if method in ['perquery','masc']:cmd+=['--collect']
 if method=='echo':cmd+=['--repeat','1','--verify']
 if method=='gup':cmd=['python3',str(R/'scripts/gup_ldbc.py'),'--graph',str(D/'data/graphs/ldbc-sf1.graph'),'--workload',str(w),'--batch','0','--gup',str(B/('gup-'+C)),'--timeout','120']
 # No other E4/E5 native process is started before this sequential gate.
 mem=int(next(l.split()[1] for l in open('/proc/meminfo') if l.startswith('MemAvailable:')))*1024
 assert mem-64*(1<<30)*1.25>=60e9
 wrapped=['/usr/bin/time','-v','-o',str(O/f'{method}.memory'),'timeout','3700','taskset','-c','264','prlimit','--as=68719476736']+cmd
 print('gate start',method,datetime.datetime.now().isoformat(),flush=True)
 existing=O/f'{method}.out'
 cached=parse(existing)[0] if existing.exists() else {}
 if int(cached.get('queries_done',0))!=len(ids) or int(cached.get('timeouts',1))!=0:
  with open(existing,'w') as out,open(O/f'{method}.err','w') as err:p=subprocess.run(wrapped,stdout=out,stderr=err)
  assert p.returncode==0,(method,p.returncode)
 st,recs,sol=parse(O/f'{method}.out');assert int(st['timeouts'])==0 and int(st['queries_done'])==len(ids),(method,st)
 counts={int(r['query_index']):int(r['embeddings']) for r in recs};assert counts=={i:len(v) for i,v in expected.items()},(method,counts)
 if method in ['perquery','masc']:
  offset=0
  for rec in recs:
   i=int(rec['query_index']);n=int(rec['solution_lines']);assert collections.Counter(map(tuple,sol[offset:offset+n]))==collections.Counter(map(tuple,expected[i]));offset+=n
  assert offset==len(sol)
 if method=='echo':assert int(st['mismatches'])==0
 peak[method]=int(next(l.split(':')[-1] for l in open(O/f'{method}.memory') if 'Maximum resident set size' in l));result[method]=counts;print('gate passed',method,counts,flush=True)
config=dict(passed=True,commit=C,input_commit=subprocess.check_output(['git','-C',str(D),'rev-parse','--short','HEAD'],text=True).strip(),peak_rss_kb=peak,counts=result,gate_time=datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=8))).isoformat(),original_unanchored_count=9892,truth_count=1)
(T/'gate.json').write_text(json.dumps(config,indent=1));print('four-method SF1 gate passed',flush=True)
