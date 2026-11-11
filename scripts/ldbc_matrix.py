#!/usr/bin/env python3
"""E4/E5 driver: existing (batch,repeat) protocol, frozen native input adapter."""
import os
ECHO_WORK = os.environ.get("ECHO_WORK", ".")
TMPDIR = os.environ.get("TMPDIR", "/tmp")
import os,json,pathlib,subprocess,time,threading,queue,math,datetime,hashlib,concurrent.futures,sys,runpy,collections
R=pathlib.Path((ECHO_WORK + '-native'));D=pathlib.Path((ECHO_WORK + '-native-data'));T=pathlib.Path((TMPDIR + '/ldbc-e4-e5-input'));T.mkdir(exist_ok=True)
runner_commit=subprocess.check_output(['git','-C',str(R),'rev-parse','HEAD'],text=True).strip()
commit=sys.argv[1];B=pathlib.Path((TMPDIR + '/echo-build/tools'));O=D/f'results/ldbc-e4-e5-{commit}';O.mkdir(exist_ok=True)
ledger=O/'cells.jsonl';lock=threading.RLock();active={};reserved=0.;peaks={};completed=set();finished=0;started=time.time()
config=json.load(open(T/'gate.json'));assert config['passed'] and config['commit']==commit
for m,v in config['peak_rss_kb'].items():peaks[1,m]=v
if ledger.exists():
 for ln in ledger.read_text().splitlines():
  row=json.loads(ln);peaks[row['sf'],row['method']]=max(peaks.get((row['sf'],row['method']),0),row.get('peak_rss_kb',0))
# Only whole method sequences resume; complete method prefixes on the SAME core are preserved.
prior=collections.defaultdict(dict)
if ledger.exists():
 for ln in ledger.read_text().splitlines():
  row=json.loads(ln);prior[row['exp'],row['sf'],row['batch'],row['rep']][row['method']]=row

def available():
 for line in open('/proc/meminfo'):
  if line.startswith('MemAvailable:'):return int(line.split()[1])*1024

def ks_reservations():
 # Resource coordination only; never waits for or modifies the KS controller.
 try:
  s=json.load(open((TMPDIR + '/timeout-closeout-e34a10f/observed-state.json')))
  return float(s['reserved_GB'])*1e9,int(s['heavy_methods'])
 except (OSError,ValueError,KeyError):return 360e9,4

def acquire(key,sf,method,core):
 global reserved
 while True:
  with lock:
   estimate=max(1.5e9,peaks.get((sf,method),64*(1<<30))*1024*1.25) if (sf,method) in peaks else 64*(1<<30)*1.25
   external,heavy_external=ks_reservations();heavy=sum(v['reservation']>60e9 for v in active.values())
   if reserved+external+estimate<=360e9 and heavy+heavy_external+int(estimate>60e9)<=4 and available()-estimate>=60e9:
    active[key]=dict(sf=sf,method=method,core=core,reservation=estimate,pid=None);reserved+=estimate;return
  time.sleep(3)

def parse(path):
 d={};records=[];rec={}
 if not path.exists():return d,records
 in_stats=False
 for line in path.read_text(errors='replace').splitlines():
  if line=='ECHO_STATS_BEGIN':in_stats=True;continue
  if line=='ECHO_STATS_END':in_stats=False;continue
  if not in_stats or '=' not in line:continue
  k,v=line.split('=',1)
  try:v=float(v) if any(c in v for c in '.eE') else int(v)
  except ValueError:pass
  d[k]=v;rec[k]=v
  if k=='record_end':records.append(rec);rec={}
 return d,records

def run_method(job,method,core):
 global reserved
 exp,sf,batch,rep=job;w=D/f'data/workloads/ldbc-sf{sf}-k8-v2';batches=json.load(open(w/'batch_index.json'))['batches'];ids=batches[batch]['indices'];bn=len(ids);b0=ids[0]
 key=f'{exp}-sf{sf}-b{batch}-r{rep}-{method}';stem=O/key
 cmd=[str(B/('echo_main-'+commit if method.startswith('echo') else method+'-'+commit)),'--graph',str(D/f'data/graphs/ldbc-sf{sf}.graph'),'--workload',str(w),'--batch',str(batch),'--timeout','120','--batch_deadline','3600']
 if method.startswith('echo'):
  cmd+=['--repeat','1']
  if method=='echo_noenvelope':cmd+=['--mode','1']
  if method=='echo_nomask':cmd+=['--mode','2']
 if method=='gup':cmd=['python3',str(R/'scripts/gup_ldbc.py'),'--graph',str(D/f'data/graphs/ldbc-sf{sf}.graph'),'--workload',str(w),'--batch',str(batch),'--gup',str(B/('gup-'+commit)),'--timeout','120','--batch_deadline','3600']
 if method=='gup':
  while not pathlib.Path(str(D/f'data/graphs/ldbc-sf{sf}.graph')+'.gup.ready').exists():time.sleep(5)
 acquire(key,sf,method,core);t0=time.time()
 try:
  wrapped=['/usr/bin/time','-v','-o',str(stem)+'.memory','timeout','3700','taskset','-c',str(core),'prlimit','--as=68719476736']+cmd
  with open(str(stem)+'.out','w') as out,open(str(stem)+'.err','w') as err:
   p=subprocess.Popen(wrapped,stdout=out,stderr=err)
   with lock:active[key]['pid']=p.pid
   rc=p.wait()
  stats,records=parse(pathlib.Path(str(stem)+'.out'));peak=int(stats.get('peak_rss_kb',0))
  for ln in pathlib.Path(str(stem)+'.memory').read_text().splitlines():
   if 'Maximum resident set size' in ln:peak=max(peak,int(ln.split(':')[-1]))
  counts={str(int(x['query_index'])):int(x['embeddings']) for x in records if 'query_index' in x and 'embeddings' in x}
  stats['peak_rss_kb']=peak
  if method in ('perquery','masc'):stats['filter_sec']=sum(float(x.get('filter_sec',0)) for x in records)
  row=dict(exp=exp,graph=f'ldbc-sf{sf}',sf=sf,workload=w.name,method=method,rep=rep,batch=batch,b0=b0,bn=bn,core=core,commit=commit,runner_commit=runner_commit,input_commit=config['input_commit'],input_sha256=(w/'ready').read_text().strip(),tstart=t0,tend=time.time(),rc=rc,stdout=str(stem)+'.out',command=cmd,embedding_counts=counts,**{k:v for k,v in stats.items() if k not in ['method','record_end','query_index','embeddings','peak_rss_kb']})
  row['peak_rss_kb']=peak
  row.setdefault('queries_done',0);row.setdefault('timeouts',int(rc==124))
  if method=='masc' and 'batch_sec' not in row:row['batch_sec']=row.get('total_sec',0)
  if row['rc']==0 and (len(counts)!=bn or row.get('queries_done')!=bn) and row['timeouts']==0:row['rc']=91;row['failure']='incomplete query coverage'
  with lock:
   peaks[sf,method]=max(peaks.get((sf,method),0),peak)
   with ledger.open('a') as f:f.write(json.dumps(row,separators=(',',':'))+'\n')
  return row
 finally:
  with lock:reserved-=active.pop(key)['reservation']

def valid(row):return row['rc']==0 and row.get('timeouts',0)==0 and row.get('queries_done')==row['bn'] and row.get('batch_sec',0)>0

def methods(exp):return ['gup','perquery','masc','echo'] if exp=='E4' else ['echo','echo_noenvelope','echo_nomask']

core_condition=threading.Condition(lock)
free_cores=set()

def run_job(job,defaultcore):
 global finished
 exp,sf,batch,rep=job;ordered=methods(exp);shift=rep%len(ordered);ordered=ordered[shift:]+ordered[:shift];old=prior[job];wanted_core=next(iter(old.values()))['core'] if old else None
 with core_condition:
  core_condition.wait_for(lambda: wanted_core in free_cores if wanted_core is not None else bool(free_cores))
  core=wanted_core if wanted_core is not None else min(free_cores);free_cores.remove(core)
 if old:assert len({x['core'] for x in old.values()})==1 and set(old)<=set(ordered)
 rows=dict(old)
 for method in ordered:
  if method not in rows:rows[method]=run_method(job,method,core)
 ref=rows['perquery'] if exp=='E4' else rows['echo']
 if valid(ref):
  for method,row in rows.items():
   if valid(row) and row['embedding_counts']!=ref['embedding_counts']:
    row=dict(row,rc=90,failure='per-query embedding count assertion',reference_method=ref['method'])
    with lock:
     with ledger.open('a') as f:f.write(json.dumps(row,separators=(',',':'))+'\n')
 with core_condition:
  finished+=1;completed.add(job);free_cores.add(core);core_condition.notify_all()

def snapshot():
 with lock:
  s=dict(time=datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=8))).isoformat(),commit=commit,controller_pid=os.getpid(),running_methods=len(active),heavy_methods=sum(x['reservation']>60e9 for x in active.values()),reserved_GB=reserved/1e9,MemAvailable_GB=available()/1e9,completed_repetitions=finished,total_repetitions=900,active=active,policy='observed peak x1.25 min1.5GB; combined KS+E4/E5 <=360GB; heavy>60GB max4; MemAvailable-new reserve >=60GB; per process64GiB;120/3600/3700s')
  tmp=T/'state.tmp';tmp.write_text(json.dumps(s,indent=1));os.replace(tmp,T/'state.json')

def main():
 cores=json.load(open(T/'cores.json'));free_cores.update(cores);jobs=[]
 for sf in [1,10]:
  n=len(json.load(open(D/f'data/workloads/ldbc-sf{sf}-k8-v2/batch_index.json'))['batches'])
  for batch in range(n):
   for rep in range(5):jobs.append(('E4',sf,batch,rep))
 for batch in range(len(json.load(open(D/'data/workloads/ldbc-sf10-k8-v2/batch_index.json'))['batches'])):
  for rep in range(5):jobs.append(('E5',10,batch,rep))
 # The original stage is queued first; E5 starts once all E4 repetitions have been dispatched.
 with concurrent.futures.ThreadPoolExecutor(max_workers=len(cores)) as pool:
  futures=[pool.submit(run_job,job,cores[i%len(cores)]) for i,job in enumerate(jobs)]
  while not all(f.done() for f in futures):snapshot();time.sleep(10)
  for f in futures:f.result()
 snapshot()
 for exp in ['E4','E5']:
  with open(O/f'{exp}.analysis.txt','w') as fo:subprocess.run(['python3',str(R/'scripts/analyze_cells.py'),str(ledger),exp],stdout=fo,check=True)
 (T/'done').write_text(datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=8))).isoformat())
 print('E4/E5 terminal',flush=True)
if __name__=='__main__':main()
