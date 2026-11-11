# ECHO

ECHO executes a round of candidate graph queries together. An LLM agent that
answers a question over a property graph often issues several Cypher
candidates that differ in a label, a hop bound, an OPTIONAL edge or a
predicate. ECHO traverses the graph once for the whole round and attaches a
query bitmask to every partial binding, so each candidate keeps its own result
semantics while the shared prefix of the traversal, the candidate space and the
variable-length expansion are computed once. A cost model partitions the round
into shared plans and candidates that run alone.

This repository is the code supplement of the ICDE 2027 submission
*ECHO: Shared Execution of Graph Queries for LLM Agents*; the paper's main LDBC
comparison is reproduced as described in
[Reproducing the main LDBC comparison](#reproducing-the-main-ldbc-comparison).
Proofs, thresholds, the full protocol and the additional results are in the
supplementary material, `supplementary/ECHO-supplementary.pdf` in this repository.

---

## Snapshot

The tree is the frozen build used for the paper's main LDBC comparison table
(run `main41a`), exported without development history:

| | |
|---|---|
| Source commit | `0915fb6aabb786c581c808fbac214f3684188661` (binary `v3_batch-0915fb6`) |
| Run driver and table builder | `experiments/ldbc-rounds/` |
| Workload | `workloads/ldbc-rounds/` (frozen candidate rounds, SF1 and SF10) |

At SF10, 703 of each method's 1,000 round runs in the paper come from the
preceding build `b737def`, which differs only in how an OPTIONAL edge between
two bound vertices is expanded; `experiments/ldbc-rounds/build_cells.py`
records the retention rule. Machine-specific absolute paths were replaced by
environment variables; the method code is unchanged. The vendored base's copy
of the SIGMOD 2020 study PDF was removed.

---

## Repository layout

```
v3/                      ECHO executor and planner (C++17)
  include/v3_ir.h          plan format of the CQ+ fragment (fixed patterns, variable-length edges, OPTIONAL)
  src/v3_align.cpp         candidate alignment and shared-plan construction
  src/v3_plan.cpp, v3_search.cpp  cost model and round partition
  src/v3_exec.cpp, v3_match.cpp   masked shared traversal, OPTIONAL padding, masked aggregation
  src/v3_vl.cpp, v3_depth_chain.cpp  depth-masked variable-length expansion
  src/v3_mcs.cpp           masked candidate space C_S (vertex, edge and depth layers)
  src/v3_masc_ldbc.cpp     MASC port on LDBC rounds
  tools/v3_batch.cpp       round driver: ECHO, per-candidate, MASC, BatchEnum+, VLPQ cells
  tools/v3_sidecar.cpp     LDBC CSV -> binary graph sidecar
  tools/v3_check.cpp       result-bag check against per-candidate evaluation
  scripts/                 CQ+ classification, round construction, sampling
src/, include/, tools/   shared base layer and the MASC port on the subgraph-matching framework
vendor/SubgraphMatching/ Sun & Luo (SIGMOD 2020) framework @ bcaa935, MIT (LICENSE kept)
vendor/nlohmann/         nlohmann/json (MIT)
patch/                   ECHO hook into the vendored base (scripts/apply_patch.sh)
scripts/                 matrix runners for the subgraph-matching and GuP comparisons
experiments/ldbc-rounds/ main-table driver (run_main40h.py), cell merge and main-LDBC-table summarizer,
                         frozen summary outputs in frozen-results/
workloads/ldbc-rounds/   candidate rounds (JSON): 720 SF1 rounds, of which the 685 listed in
                         populations/sf1-shards/ are run (635 with >= 2 distinct CQ+ candidates
                         enter the table), and the 200 sampled SF10 rounds
baselines/vlpq/          the one-line build change applied to the official VLPQ code
data/graphs/             small labeled graphs for the subgraph-matching workloads
```

---

## Build

Tested on Ubuntu 22.04 (x86-64) with GCC 11.4 and CMake >= 3.16.

```bash
sh scripts/apply_patch.sh                    # idempotent
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
cmake -S v3 -B v3/build -DV3_BASE_BUILD="$PWD/build" && cmake --build v3/build -j
```

All binaries are single-threaded. Set `-DECHO_MARCH=<arch>` when building for
a different machine than the one running the experiments.

---

## Data

- **LDBC SNB Interactive** SF1 and SF10, generated with the official
  [LDBC SNB Datagen](https://github.com/ldbc/ldbc_snb_datagen_spark)
  (CsvComposite), with the official substitution parameters at SF10. Build the
  sidecar once per scale factor:
  `v3/build/v3_sidecar --raw <ldbc-export> --ids <graph>.ids.json --out "$ECHO_ROOT/sidecar/sf10"`.
- **Candidate rounds** are included in `workloads/ldbc-rounds/`. They come from
  a frozen pool of 8,000 candidates for 200 questions (five rounds of K=8 per
  question) sampled from a locally served Qwen model with CypherBench's
  zero-shot prompt; `v3/scripts/rounds_v3.py` builds rounds from such a pool.
- **Subgraph-matching graphs** (MASC/GuP comparison): the datasets of
  [Sun & Luo](https://github.com/RapidsAtHKUST/SubgraphMatching); three small
  graphs are in `data/graphs/`.

---

## Compared systems

| System | Venue | Implementation |
|---|---|---|
| Per-candidate | — | base filter and enumerator for pure patterns, ECHO's executor with one candidate per plan otherwise (`v3_batch --serial`) |
| MASC | SIGMOD'26 | no public code; ported onto the Sun & Luo framework (`src/masc_*.cpp`, `v3/src/v3_masc_ldbc.cpp`): shared signatures, visit-once enumeration, block reuse |
| BatchEnum+ | ICDE'24 | no public code; ported into the same executor (simple-path semantics, `v3/include/v3_ir.h`) |
| VLPQ | ICDE'24 | official Graphflow code, [github.com/15197580192/jumping-wco-join-graphflow](https://github.com/15197580192/jumping-wco-join-graphflow) @ `b6abcc7233b339a7ee6784b14fee8ff9c434db38`; build change in `baselines/vlpq/` |
| GuP | SIGMOD'23 | official code, [github.com/araij/gup](https://github.com/araij/gup) @ `fbaf103967b40a5a6837e5404f61479c008586c8`; input adapter `scripts/gup_ldbc.py`, `scripts/gup-predicate-input.patch` |
| Base framework | SIGMOD'20 | [github.com/RapidsAtHKUST/SubgraphMatching](https://github.com/RapidsAtHKUST/SubgraphMatching) @ `bcaa935`, vendored |

---

## Reproducing the main LDBC comparison

Each round runs ECHO, per-candidate, MASC, BatchEnum+ and VLPQ interleaved on
one pinned core, five times, with a 3,600 s cap per round and a per-process
memory limit; graph loading is outside the timed region.

```bash
export ECHO_ROOT=/path/to/workspace          # holds sidecar/sf1i, sidecar/sf10 and work/
python experiments/ldbc-rounds/run_main40h.py --bin v3/build/v3_batch \
    --out "$ECHO_ROOT/work/main40h/run" --cores 0,1,2,3         # comma-separated physical cores; --dry-run lists the jobs
python experiments/ldbc-rounds/build_cells.py --root "$ECHO_ROOT" --out "$ECHO_ROOT/work/main41a"
python experiments/ldbc-rounds/summarize_table2.py \
    --cells "$ECHO_ROOT/work/main41a/cells.jsonl" --out "$ECHO_ROOT/work/main41a"
```

`experiments/ldbc-rounds/frozen-results/` holds the frozen summary of the
paper's table (`table2.md`, per-template `by_template.md`, and `inventory.json`
with population sizes, row counts and the 40 unrecorded cells).

---

## Status of this supplement

- `run_main40h.py` resumes from and retains earlier SF10 cells under
  `$ECHO_ROOT/work/` (the `b737def` runs); a fresh run from scratch needs the
  sidecars only, but the retention logic still expects that directory layout.
- Building the sidecar needs the `<graph>.ids.json` vertex index produced by
  the earlier export step, which is not part of this snapshot.
- The experiments other than the main LDBC comparison (scalability, MASC/GuP home graphs,
  ablation, Text2GraphQuery-Bench) were produced by other builds of the same
  code base; their drivers are in `scripts/` and `v3/scripts/`, their commits
  are not part of this snapshot.
- The ECHO code is released under the Apache License 2.0 (`LICENSE`). Vendored code keeps its
  own license (`vendor/SubgraphMatching/LICENSE`, nlohmann/json header).
