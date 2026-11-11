#!/bin/bash
# Overlap-0 <=1.05x gate (PLAN item 2). One core. The batch cost = the batch_sec reported by
# the tool (algorithm time; graph load excluded for both methods). A/B/B/A on the same batch.
set -u
BIN=${TMPDIR}/echo-build/tools
CORE=${CORE:-32}
OUT=${OUT:-${ECHO_WORK}-native/results/gate105}
mkdir -p "$OUT"
run_one () { # $1 graph $2 workload $3 method $4 batch $5 out
  local bin="$3"; [ "$3" = echo ] && bin=echo_main
  taskset -c "$CORE" prlimit --as=68719476736 "$BIN/$bin" \
    --graph "data/graphs/$1.graph" --workload "data/workloads/$2" --batch "$4" --repeat 1 --timeout 120 \
    > "$5.out" 2>"$5.err"
  echo "rc=$?" >> "$5.out"
}
g=$1; w=$2; nb=${3:-10}
REPS=${REPS:-3}
for b in $(seq 0 $((nb-1))); do
  tag="${g}_${w}_b$(printf %02d $b)"
  for rep in $(seq 0 $((REPS-1))); do
    for m in perquery echo echo perquery; do
      f="$OUT/${tag}_rep${rep}_${m}"
      [ -s "$f.out" ] && continue
      run_one "$g" "$w" "$m" "$b" "$f"
    done
  done
  echo "done $tag"
done
echo ALL_DONE
