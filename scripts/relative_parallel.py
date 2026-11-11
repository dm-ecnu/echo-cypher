#!/usr/bin/env python3
"""Parallel continuation of relative_matrix.py (supervisor decision 2026-10-31 20:45 PDT).
E4 SF10 and E5 SF10 use a fixed-seed sample of 12 of the 60 batches (random.Random(12345)); E6 and KS
run in the same pool. Windows, cores, binaries, caps, timeouts, parser and ledger are those of relative_matrix.py;
completed windows in the ledger are reused. Non-SF10 jobs leave room for two SF10 methods while SF10 jobs remain.
"""
import os
ECHO_WORK = os.environ.get("ECHO_WORK", ".")
TMPDIR = os.environ.get("TMPDIR", "/tmp")
import os,json,pathlib,subprocess,time,threading,concurrent.futures,collections,sys,datetime,hashlib
R=pathlib.Path((ECHO_WORK + '-native'));D=pathlib.Path((ECHO_WORK + '-native-data'));B=pathlib.Path((TMPDIR + '/echo-build/tools'))
C=sys.argv[1];T=pathlib.Path(f'{TMPDIR}/rho025-campaign-{C}');O=D/f'results/relative-{C}';O.mkdir(exist_ok=True);(O/'raw').mkdir(exist_ok=True)
os.environ['TMPDIR']=(TMPDIR + '/tmpdir');os.makedirs(os.environ['TMPDIR'],exist_ok=True)
runner_commit=subprocess.check_output(['git','-C',str(R),'rev-parse','HEAD'],text=True).strip();cores=json.load(open(T/os.environ.get('CORES_FILE','cores.json')));assert cores and min(cores)>=216 and max(cores)<=383
cfg=json.load(open(T/'gate.json'));assert cfg['passed'] and cfg['commit']==C
lock=threading.RLock();condition=threading.Condition(lock);free=set(cores);active={};reserved=0.;peaks={};finished=set();stage='';stage_jobs=[]
# Import the existing stable stats parser without executing its launch-time globals.
import ast
source=ast.parse((R/'scripts/ldbc_matrix.py').read_text());fn=next(x for x in source.body if isinstance(x,ast.FunctionDef) and x.name=='parse');exec(compile(ast.Module(body=[fn],type_ignores=[]),str(R/'scripts/ldbc_matrix.py'),'exec'),globals())
def read(path):return [json.loads(x) for x in path.read_text().splitlines() if x] if path.exists() else []
for path in [D/'results/ldbc-e4-e5-0dac581/cells.jsonl',D/'results/e6/paired-final-e34a10f/cells.jsonl',R/'results/cells-ks32-64-final-e34a10f.jsonl',R/'results/cells-ks128256-final-e34a10f.jsonl']:
 for row in read(path):
  if row.get('sf')==10:continue # 64GiB censored loaders do not give a complete 128GiB peak.
  key=(pathlib.Path(row['workload']).name,row['method']);peaks[key]=max(peaks.get(key,0),(row.get('peak_rss_kb') or 0)*1024)
ledger=O/('cells-e6ks.jsonl' if os.environ.get('ONLY_E6KS') else 'cells.jsonl');prior=collections.defaultdict(dict)
# Rows that failed after the root disk filled (2026-11-01 17:34 CST; binaries crash when $TMPDIR=/tmp is full)
# stay in the ledger but are rerun; the analysis keeps the last row per window and method.
DISKFULL=1793525000
for row in read(ledger):
 if row['rc']!=0 and row['tstart']>=DISKFULL:continue
 prior[row['window']][row['method']]=row;k=(row['workload'],row['method']);peaks[k]=max(peaks.get(k,0),row['peak_rss_kb']*1024)
def available():return int(next(l.split()[1] for l in open('/proc/meminfo') if l.startswith('MemAvailable:')))*1024
def methods(j):return ['echo','echo_noenvelope','echo_nomask'] if j['exp']=='E5' else ['gup','perquery','masc','echo']
def token(j):return f"{j['stage']}-{j['graph']}-{j.get('tier','')}-{j['workload']}-b{j['batch']}-r{j['rep']}"
def reserve(j,m):return max(1.5e9,peaks[(j['workload'],m)]*1.25) if (j['workload'],m) in peaks else j['cap_bytes']*1.25
def heavy_left():return any(j['sf']==10 and token(j) not in finished for j in stage_jobs)
def acquire(j,m,core):
 global reserved
 key=token(j)+'-'+m
 with condition:
  while True:
   amount=reserve(j,m)
   heavy=sum(x['reservation']>60e9 for x in active.values())
   room=0 if amount>60e9 or not heavy_left() else 125e9*max(0,2-heavy)
   if reserved+amount<=360e9 and heavy+int(amount>60e9)<=4 and available()-amount>=60e9+room:break
   condition.wait(1)
  active[key]=dict(method=m,core=core,reservation=amount,pid=None,window=token(j));reserved+=amount
 return key
def binary(j,m):
 version=C if m.startswith('echo') else ('0dac581' if j['sf'] else 'e34a10f')
 name='echo_main' if m.startswith('echo') else ('masc_main' if m=='masc' and not j['sf'] else m)
 return B/f'{name}-{version}',version
def run_method(j,m,core):
 global reserved
 key=acquire(j,m,core);exe,version=binary(j,m);stem=O/'raw'/key;w=pathlib.Path(j['workload_path'])
 cmd=[str(exe),'--graph',j['graphfile'],'--workload',str(w),'--timeout','120','--batch_deadline','3600']
 if j['sf'] or m=='masc':cmd+=['--batch',str(j['batch'])]
 else:cmd+=['--b0',str(j['b0']),'--bn',str(j['bn'])]
 if m.startswith('echo'):
  cmd+=['--repeat','1','--counts']
  if m=='echo_noenvelope':cmd+=['--mode','1']
  if m=='echo_nomask':cmd+=['--mode','2']
 if m=='gup':
  wrapper=R/'scripts/gup_ldbc.py' if j['sf'] else pathlib.Path((TMPDIR + '/timeout-closeout-e34a10f/gup_batch.py'))
  cmd=['python3',str(wrapper),'--graph',j['graphfile'],'--workload',str(w),'--batch',str(j['batch']),'--gup',str(exe),'--timeout','120']
  if j['sf']:cmd+=['--batch_deadline','3600']
 start=time.time()
 try:
  wrapped=['/usr/bin/time','-v','-o',str(stem)+'.memory','timeout','3700','taskset','-c',str(core),'prlimit','--as='+str(j['cap_bytes'])]+cmd
  with open(str(stem)+'.out','w') as fo,open(str(stem)+'.err','w') as fe:
   proc=subprocess.Popen(wrapped,stdout=fo,stderr=fe)
   with lock:active[key]['pid']=proc.pid
   rc=proc.wait()
  st,recs=parse(pathlib.Path(str(stem)+'.out'));peak=int(st.get('peak_rss_kb',0))
  for line in pathlib.Path(str(stem)+'.memory').read_text().splitlines():
   if 'Maximum resident set size' in line:peak=max(peak,int(line.split(':')[-1]))
  counts={str(int(r['query_index'])):int(r['embeddings']) for r in recs if 'query_index' in r and 'embeddings' in r}
  if m in ['perquery','masc'] and recs:st['filter_sec']=sum(float(r.get('filter_sec',0)) for r in recs)
  row=dict(j,window=token(j),core=core,method=m,commit=version,echo_commit=C,runner_commit=runner_commit,binary=str(exe),binary_sha256=hashes[str(exe)],tstart=start,tend=time.time(),rc=rc,embedding_counts=counts,stdout=str(stem)+'.out',command=wrapped,**{k:v for k,v in st.items() if k not in ['method','record_end','query_index','embeddings','peak_rss_kb']})
  row['peak_rss_kb']=peak;row.setdefault('queries_done',len(counts));row.setdefault('timeouts',int(rc==124))
  if m=='masc' and 'batch_sec' not in row:row['batch_sec']=row.get('total_sec',0)
  if rc==0 and row['timeouts']==0 and (len(counts)!=j['bn'] or row['queries_done']!=j['bn']):row.update(rc=91,failure='incomplete per-query coverage')
  with condition:
   peaks[j['workload'],m]=max(peaks.get((j['workload'],m),0),peak*1024)
   with ledger.open('a') as f:f.write(json.dumps(row,separators=(',',':'))+'\n')
  return row
 finally:
  with condition:reserved-=active.pop(key)['reservation'];condition.notify_all()
def valid(r):return r['rc']==0 and r.get('timeouts',0)==0 and r.get('queries_done')==r['bn'] and r.get('batch_sec',0)>0
def run_job(j):
 t=token(j);old=prior[t];wanted=next(iter(old.values()))['core'] if old else None
 with condition:
  condition.wait_for(lambda:wanted in free if wanted is not None else bool(free));core=wanted if wanted is not None else min(free);free.remove(core)
 order=methods(j);shift=j['rep']%len(order);order=order[shift:]+order[:shift];rows=dict(old)
 assert not old or len({r['core'] for r in old.values()})==1
 for m in order:
  if m not in rows:rows[m]=run_method(j,m,core)
 ref=rows['echo' if j['exp']=='E5' else 'perquery']
 if valid(ref):
  for m,r in rows.items():
   if valid(r) and r['embedding_counts']!=ref['embedding_counts']:
    correction=dict(r,rc=90,failure='per-query embedding count assertion',reference_method=ref['method'])
    with lock:
     with ledger.open('a') as f:f.write(json.dumps(correction,separators=(',',':'))+'\n')
 with condition:finished.add(t);free.add(core);condition.notify_all()
def snapshot():
 with lock:
  groups=collections.defaultdict(list)
  for j in stage_jobs:groups[(j['graph'],j.get('tier',''),j['workload'])].append(j)
  progress=[]
  for (g,t,w),jobs in groups.items():
   batches=collections.defaultdict(list)
   for j in jobs:batches[j['batch']].append(j)
   progress.append(dict(graph=g,tier=t,workload=w,queries=sum(x[0]['bn'] for x in batches.values()),completed_batches=sum(all(token(j) in finished for j in js) for js in batches.values()),total_batches=len(batches),completed_repetitions=sum(token(j) in finished for j in jobs),total_repetitions=len(jobs)))
  s=dict(time=datetime.datetime.now().astimezone().isoformat(),controller_pid=os.getpid(),commit=C,stage=stage,running_methods=len(active),heavy_methods=sum(x['reservation']>60e9 for x in active.values()),reserved_GB=reserved/1e9,MemAvailable_GB=available()/1e9,progress=progress,active=active,policy='observed peak x1.25 floor1.5GB; total360GB; heavy>60GB max4; MemAvailable-newreserve>=60GB; SF1 64GiB/SF10 128GiB/E6 24GiB/KS64GiB;120/3600/3700s')
  tmp=T/(os.environ.get('STATE_PREFIX','')+'state.tmp');tmp.write_text(json.dumps(s,indent=1));os.replace(tmp,T/(os.environ.get('STATE_PREFIX','')+'state.json'))
def ldbc(exp,sf):
 w=D/f'data/workloads/ldbc-sf{sf}-k8-v2';bs=json.load(open(w/'batch_index.json'))['batches'];out=[]
 for b,entry in enumerate(bs):
  ids=entry['indices']
  for rep in range(5):out.append(dict(exp=exp,stage=f'{exp}-sf{sf}',sf=sf,graph=f'ldbc-sf{sf}',graphfile=str(D/f'data/graphs/ldbc-sf{sf}.graph'),workload=w.name,workload_path=str(w),batch=b,b0=ids[0],bn=len(ids),rep=rep,cap_bytes=(128 if sf==10 else 64)<<30))
 return out
sys.path.insert(0,str(D/'scripts'));from e6_run import cells
import random
SAMPLE=sorted(random.Random(12345).sample(range(60),12))
stages=[] if os.environ.get('ONLY_E6KS') else [[j for j in ldbc('E4',10) if j['batch'] in SAMPLE],[j for j in ldbc('E5',10) if j['batch'] in SAMPLE]]
e6=[]
for c in cells():
 if c['method']!='perquery':continue
 e6.append(dict(c,exp='E6',stage='E6',sf=0,workload=pathlib.Path(c['workload']).name,workload_path=c['workload'],b0=c['batch']*30,cap_bytes=24<<30))
stages.append(e6) if not os.environ.get('ONLY_SF10') else None;ks=[]
for k in [32,64,128,256]:
 for g,prefix,gn in [('figeys','figeys','maayan-figeys-100'),('dblp','dblp-100','dblp-100'),('wordnet','wordnet-words-100','wordnet-words-100')]:
  w=R/f'data/workloads/{prefix}-k{k}-b30-o0-s0'
  for b in range(10):
   for rep in range(5):ks.append(dict(exp='KS',stage='KS',sf=0,graph=g,graphfile=str(R/f'data/graphs/{gn}.graph'),workload=w.name,workload_path=str(w),k=k,batch=b,b0=b*30,bn=30,rep=rep,cap_bytes=64<<30))
stages.append(ks) if not os.environ.get('ONLY_SF10') else None
hashes={}
for jobs in stages:
 for j in jobs:
  for m in methods(j):
   exe,_=binary(j,m)
   if str(exe) not in hashes:hashes[str(exe)]=hashlib.sha256(exe.read_bytes()).hexdigest()
stage='parallel';stage_jobs=[j for jobs in stages for j in jobs]
print('parallel start sample',SAMPLE,' '.join(f"{jobs[0]['stage']}:{len(jobs)}" for jobs in stages),flush=True)
with concurrent.futures.ThreadPoolExecutor(max_workers=len(stage_jobs)) as pool:
 futures={jobs[0]['stage']:[pool.submit(run_job,j) for j in jobs] for jobs in stages};marked=set()
 while True:
  snapshot()
  for st,fs in futures.items():
   if st not in marked and all(f.done() for f in fs):
    for f in fs:f.result()
    marked.add(st);(T/f'{st}.done').write_text(datetime.datetime.now().astimezone().isoformat());print('stage terminal',st,flush=True)
  if len(marked)==len(futures):break
  time.sleep(10)
(T/(os.environ.get('STATE_PREFIX','')+'done')).write_text(datetime.datetime.now().astimezone().isoformat());print('parallel campaign terminal',flush=True)
