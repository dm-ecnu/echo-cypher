#!/usr/bin/env python3
"""Measure two concurrent one-round v3-only processes: legacy vs shared cache."""
import argparse, json, os, re, subprocess, time

def pss(pid):
    try:
        t=open(f"/proc/{pid}/smaps_rollup").read(); m=re.search(r"^Pss:\s+(\d+)",t,re.M)
        return int(m.group(1)) if m else 0
    except Exception: return 0
def children(pid):
    out=[]
    for d in os.listdir("/proc"):
        if not d.isdigit(): continue
        try:
            txt=open(f"/proc/{d}/stat").read(); r=txt.rfind(")")
            ppid=int(txt[r+2:].split()[1])
            if ppid==pid: out.append(int(d))
        except Exception: pass
    return out
def tree_pss(pid):
    seen=set(); todo=[pid]; total=0
    while todo:
        p=todo.pop()
        if p in seen or not os.path.exists(f"/proc/{p}"): continue
        seen.add(p); total+=pss(p); todo.extend(children(p))
    return total
def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--dataset",required=True); ap.add_argument("--old",required=True); ap.add_argument("--new",required=True)
    ap.add_argument("--sidecar",required=True); ap.add_argument("--list",required=True); ap.add_argument("--outdir",required=True)
    ap.add_argument("--cores",default="122,123"); ap.add_argument("--mem-gib",type=int,default=32); ap.add_argument("--cap",type=int,default=600)
    a=ap.parse_args(); os.makedirs(os.path.join(a.outdir,"logs"),exist_ok=True)
    cores=[int(x) for x in a.cores.split(",") if x.strip()]
    rows=[]
    for tag,b in (("old",a.old),("new",a.new)):
        procs=[]
        for i in range(2):
            log=open(os.path.join(a.outdir,"logs",f"{tag}-{i}.log"),"w")
            cmd=["taskset","-c",str(cores[i]),"prlimit",f"--as={a.mem_gib*(1<<30)}",f"--rss={a.mem_gib*(1<<30)}",b,
                 "--sidecar",a.sidecar,"--list",a.list,"--out",os.path.join(a.outdir,f"{tag}-{i}.jsonl"),
                 "--reps","1","--rep-base","0","--cell-cap",str(a.cap),"--run-id","main40h-pair-"+tag,
                 "--dataset","ldbc-"+a.dataset,"--v3-only"]
            p=subprocess.Popen(cmd,stdout=log,stderr=subprocess.STDOUT,preexec_fn=os.setsid)
            procs.append((p,log))
        peak=0; each=[0,0]; t0=time.time()
        while any(p.poll() is None for p,_ in procs):
            vals=[tree_pss(p.pid) for p,_ in procs]
            peak=max(peak,sum(vals)); each=[max(each[i],vals[i]) for i in range(2)]
            time.sleep(0.5)
        rcs=[p.wait() for p,_ in procs]
        for _,log in procs: log.close()
        row={"label":tag,"rc":rcs,"elapsed_sec":time.time()-t0,"peak_pair_pss_kb":peak,
             "peak_pair_pss_gib":round(peak/1024/1024,3),
             "peak_each_pss_gib":[round(x/1024/1024,3) for x in each]}
        rows.append(row); print(json.dumps(row),flush=True)
    json.dump(rows,open(os.path.join(a.outdir,"memory.json"),"w"),indent=1)
if __name__=="__main__": main()
