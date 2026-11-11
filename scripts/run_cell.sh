#!/bin/bash
# One experiment cell: (graph, workload, method, repeat, b0, bn) on one core.
# Emits a single JSON line to $OUT/cells.jsonl (append, flock-serialized).
set -u
REPO=${ECHO_WORK}-native
BIN=${TMPDIR}/echo-build/tools
COMMIT=${ECHO_COMMIT:-}
ECHO_BIN="$BIN/echo_main"
PQ_BIN="$BIN/perquery"
if [ -n "$COMMIT" ] && [ -x "$BIN/echo_main-$COMMIT" ]; then ECHO_BIN="$BIN/echo_main-$COMMIT"; fi
if [ -n "$COMMIT" ] && [ -x "$BIN/perquery-$COMMIT" ]; then PQ_BIN="$BIN/perquery-$COMMIT"; fi
RESULTS=$REPO/results
CELLS=$RESULTS/cells.jsonl
mkdir -p "$RESULTS"

graph=$1; graphfile=$2; workload=$3; method=$4; rep=$5; b0=$6; bn=$7; core=$8
extra=${9:-}
tstart=$(date +%s.%N)
f="$RESULTS/tmp_${graph}_${workload}_${method}_r${rep}_b${b0}_n${bn}_c${core}.out"
rc=0
case "$method" in
  perquery) timeout 3700 taskset -c "$core" prlimit --as=68719476736 "$PQ_BIN" \
      --graph "data/graphs/$graphfile.graph" --workload "data/workloads/$workload" \
      --b0 "$b0" --bn "$bn" --timeout 120 --batch_deadline 3600 > "$f" 2>"$f.err" || rc=$? ;;
  echo) timeout 3700 taskset -c "$core" prlimit --as=68719476736 "$ECHO_BIN" \
      --graph "data/graphs/$graphfile.graph" --workload "data/workloads/$workload" \
      --b0 "$b0" --bn "$bn" --repeat 1 --timeout 120 --batch_deadline 3600 > "$f" 2>"$f.err" || rc=$? ;;
  echo_noenvelope) timeout 3700 taskset -c "$core" prlimit --as=68719476736 "$ECHO_BIN" \
      --graph "data/graphs/$graphfile.graph" --workload "data/workloads/$workload" \
      --b0 "$b0" --bn "$bn" --repeat 1 --mode=1 --timeout 120 --batch_deadline 3600 > "$f" 2>"$f.err" || rc=$? ;;
  echo_nomask) timeout 3700 taskset -c "$core" prlimit --as=68719476736 "$ECHO_BIN" \
      --graph "data/graphs/$graphfile.graph" --workload "data/workloads/$workload" \
      --b0 "$b0" --bn "$bn" --repeat 1 --mode=2 --timeout 120 --batch_deadline 3600 > "$f" 2>"$f.err" || rc=$? ;;
  gup) GUP=${ECHO_WORK}-native-masc/scripts/gup_batch.py
      if [ -f "$GUP" ]; then
        timeout 3700 python3 "$GUP" --graph "data/graphs/$graphfile.graph" \
          --workload "data/workloads/$workload" --batch "$((b0 / 30))" --timeout 120 \
          --cpu "$core" > "$f" 2>"$f.err" || rc=$?
      else rc=97; fi ;;
  masc) MASC=${ECHO_WORK}-native-masc/build/masc_main
      if [ -x "$MASC" ]; then
        timeout 3700 taskset -c "$core" prlimit --as=68719476736 "$MASC" \
          --graph "data/graphs/$graphfile.graph" --workload "data/workloads/$workload" \
          --batch "$((b0 / 30))" --batch-sec 3600 > "$f" 2>"$f.err" || rc=$?
      else rc=98; fi ;;
  *) rc=99 ;;
esac
tend=$(date +%s.%N)
python3 - "$graph" "$graphfile" "$workload" "$method" "$rep" "$b0" "$bn" "$core" "$extra" "$rc" "$tstart" "$tend" "$f" <<'PYEOF'
import re, sys, json, os
(graph, graphfile, workload, method, rep, b0, bn, core, extra, rc, tstart, tend, f) = sys.argv[1:14]
vals = {}
key = ("batch_sec","total_sec","gup_sec","queries_done","timeouts","filter_sec","search_sec","peak_rss_kb",
       "envelopes","family_sec","plan_total_sec","batch_expired","solutions","load_sec","total_sec")
try:
    for line in open(f):
        m = re.match(r"^(" + "|".join(key) + r")=([\d.eE+-]+)", line)
        if m: vals[m.group(1)] = float(m.group(2))
except OSError:
    pass
# Coverage assertion: a cell must actually process bn queries. Tools that report
# queries_done do so directly; the per-query style tools are counted by their record_end lines.
done = None
try:
    lines = open(f).read().splitlines()
    rec_end = sum(1 for l in lines if l == "record_end=1")
    if rec_end:
        done = rec_end
    for l in lines:
        if l.startswith("queries_done="):
            done = int(float(l.split("=")[1]))
except OSError:
    pass
if done is not None and done != int(bn):
    rc = 96  # the cell did not cover the requested queries: treat as a failure
rec = {"exp": os.environ.get("ECHO_EXP",""), "graph": graph, "workload": workload,
       "method": method, "rep": int(rep), "b0": int(b0), "bn": int(bn), "core": int(core),
       "commit": os.environ.get("ECHO_COMMIT",""),
       "time": __import__("datetime").datetime.now().astimezone().isoformat(timespec="seconds"),
       "rc": int(rc), "wall": float(tend)-float(tstart)}
if "batch_sec" not in vals:
    qs_sum = 0.0
    try:
        for line in open(f):
            if line.startswith("query_sec="):
                qs_sum += float(line.split("=")[1])
        if qs_sum > 0:
            vals["batch_sec"] = qs_sum
    except OSError:
        pass
for k in ("batch_sec","queries_done","timeouts","filter_sec","search_sec","peak_rss_kb",
          "envelopes","family_sec","plan_total_sec","batch_expired","solutions","load_sec"):
    if k in vals: rec[k] = vals[k]
if extra: rec["extra"] = extra
out = os.environ.get("ECHO_CELLS", "${ECHO_WORK}-native/results/cells.jsonl")
with open(out + ".lock", "w") as lk:
    import fcntl; fcntl.flock(lk, 2)
    with open(out, "a") as fh: fh.write(json.dumps(rec) + "\n")
    fcntl.flock(lk, 8)
PYEOF
rm -f "$f" "$f.err"
