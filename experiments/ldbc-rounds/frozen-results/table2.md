# Table II — LDBC SNB speculative rounds (Exp-1, K=8)

Computed from `work/main40h/run` cells, with the retained main40 SF10 cells for unaffected rounds (`work/main41a/scripts/build_cells.py`). Ratio is method/ECHO on the same round; >1 means ECHO is faster. Each round enters as the median of its five repeats; the interval is a 95% bootstrap over rounds (5,000 draws, seed 20261109). Memory is the median of per-round median peak RSS. Completion is the candidate-weighted share of repeats that finished within the 3,600 s cap. Coverage is the share of candidates evaluated jointly; ECHO candidates in Fallback are not covered.

**Population.** SF1: 660 non-empty rounds, 3470 candidates, 3300 round-reps; the run contains 3425 jobs (685 rounds), but 25 rounds have zero candidates and are excluded from ratios and denominators. SF10: 200 rounds, 1112 candidates, 1,000 round-reps, with 8 IC9 round-reps missing from both the new and retained old cells (round-0711 rep3/4, round-0712 rep0-4, round-0715 rep0); they stay in the completion denominator and are not estimated.

**Provenance.** Merged `cells.jsonl` SHA256 `b68ef4e5d3eac6aaded765ef61bd94a2d3c63f6d13a534e6d5da8dec9eb1cab6`, 22125 rows (22,125 = 685*5*5 + 200*5*5; 40 missing placeholders = 8 IC9 round-reps * 5 methods; 921 serial-fallback rows). `inputs.sha256` covers 3790 contributing files and 21488 file lines; combined input digest `d4dc3934741b9611db434aa44cf000ce82abaf1b84ff484a7ec22e2446c269c4`. Scripts are `scripts/build_cells.py` and `scripts/summarize_table2.py`; their SHA256 values are in `table2.json`.

| Method | SF1 Time | SF1 Work | SF1 Mem. (MB) | SF1 Compl. | SF1 Cov. | SF10 Time | SF10 Work | SF10 Mem. (MB) | SF10 Compl. | SF10 Cov. |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Per-candidate | 1.397 [1.277,1.537] | 1.163 [1.125,1.204] | 54 | 94.7% | 0.0% | 1.900 [1.423,2.584] | 0.761 [0.571,0.975] | 11419 | 82.8% | 0.0% |
| VLPQ | 1.279 [1.167,1.411] | 1.163 [1.125,1.204] | 54 | 94.7% | 0.0% | 1.549 [1.146,2.136] | 0.761 [0.571,0.975] | 11419 | 82.8% | 0.0% |
| MASC (port) | 1.385 [1.264,1.523] | 1.777 [1.674,1.892] | 54 | 94.7% | 30.4% | 1.733 [1.296,2.371] | 1.547 [1.093,2.102] | 11419 | 82.8% | 37.5% |
| BatchEnum+ (port) | 1.267 [1.156,1.398] | 1.163 [1.125,1.204] | 54 | 94.7% | 0.0% | 1.556 [1.151,2.143] | 0.761 [0.571,0.975] | 11419 | 82.8% | 0.0% |
| ECHO | 1 | 1 | 56 | 94.7% | 47.4% | 1 | 1 | 11419 | 83.7% | 66.7% |

ECHO row: Time and Work are 1 by definition; Mem. is ECHO's measured median. BatchEnum+ and VLPQ have no encodable candidate on this workload; their cells are per-candidate fallback and their 0% coverage is the point of the column, not a missing measurement. MASC (port) covers only the pure-pattern candidates; its non-covered candidates (including all IC9) run as per-candidate Fallback, so its completion is the same as per-candidate.

## Secondary metrics

| SF | ECHO > per-candidate (valid pairs) | ECHO > per-candidate (all rounds) | ECHO Fallback share (ok cells) | red(R) median | red(R) pooled | red(R) p10..p90 | red(R) positive | red(R) n |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| SF1 | 299/625 = 47.8% | 299/660 = 45.3% | 50.0% (8210/16435) | 0.000 | -0.000 | 0.000..0.474 | 19.4% | 620 |
| SF10 | 97/161 = 55.9% | 97/200 = 48.5% | 20.3% (945/4655) | 0.000 | -0.039 | -0.814..0.559 | 25.5% | 157 |

`red(R) = 1 - W(ECHO)/W(per-candidate)` per round, from the same-round five-repeat medians; the pooled value sums work over the rounds with both work values. The all-round win share counts a non-completion by one side as that side losing; the valid-pair share excludes rounds where either side has no time. The SF10 mean red(R) is -79 because one IC9 plan does 2,600× per-candidate work on two rounds; median and pooled values are shown instead of that mean. All-at-cap sensitivity (errors and missing cells also enter at 3,600 s): SF1 per-candidate 1.373, VLPQ 1.262, MASC 1.361, BatchEnum+ 1.251; SF10 per-candidate 1.846, VLPQ 1.567, MASC 1.715, BatchEnum+ 1.572. The main and all-at-cap values differ by less than 0.06.

## Failure and denominator accounting

| SF | Method | ok | timeout | error | missing | cells | completion | coverage |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| SF1 | Per-candidate | 3090 | 34 | 176 | 0 | 3300 | 94.7% (16435/17350) | 0.0% (0/17350) |
| SF1 | VLPQ | 3090 | 34 | 176 | 0 | 3300 | 94.7% (16435/17350) | 0.0% (0/17350) |
| SF1 | MASC (port) | 3090 | 34 | 176 | 0 | 3300 | 94.7% (16435/17350) | 30.4% (5275/17350) |
| SF1 | BatchEnum+ (port) | 3090 | 34 | 176 | 0 | 3300 | 94.7% (16435/17350) | 0.0% (0/17350) |
| SF1 | ECHO | 3090 | 20 | 190 | 0 | 3300 | 94.7% (16435/17350) | 47.4% (8225/17350) |
| SF10 | Per-candidate | 785 | 19 | 188 | 8 | 1000 | 82.8% (4605/5560) | 0.0% (0/5560) |
| SF10 | VLPQ | 785 | 19 | 188 | 8 | 1000 | 82.8% (4605/5560) | 0.0% (0/5560) |
| SF10 | MASC (port) | 785 | 19 | 188 | 8 | 1000 | 82.8% (4605/5560) | 37.5% (2085/5560) |
| SF10 | BatchEnum+ (port) | 785 | 19 | 188 | 8 | 1000 | 82.8% (4605/5560) | 0.0% (0/5560) |
| SF10 | ECHO | 795 | 35 | 162 | 8 | 1000 | 83.7% (4655/5560) | 66.7% (3710/5560) |

Completion counts an `ok` repeat as all K candidates finished and a timeout/error/missing repeat as zero; partial progress inside a timed-out process is not recorded. Timeout time enters the time median at 3,600 s; errors have no time and are excluded from the time ratio but stay in completion. Missing SF10 IC9 round-reps are counted in the denominator, not filled. IC9 runs only ECHO and per-candidate; the other three methods use the per-candidate cell with coverage 0 (source `serial-fallback`).

## Population check

- The paper currently states 685 SF1 rounds and 3,810 candidates. The raw main40h cells give 685 rounds and **3,470** candidates; 25 of the 685 rounds (all IS3) have zero queries, and 25 have one query. Restricting to rounds with at least two candidates leaves 635 rounds and 3,445 candidates. The 3,810 figure is not reproducible from these cells; the paper text and this table need to be reconciled before the number is frozen. MASC coverage on this run is 30.4%, not the paper's 36.1%; ECHO coverage is 47.4%.

- On the paper's own ≥2-candidate rule (635 SF1 rounds, 3,445 candidates) the time ratios are per-candidate 1.418 [1.293,1.560], MASC 1.406 [1.280,1.549], BatchEnum+ 1.287, VLPQ 1.297; ECHO coverage 47.8%, MASC coverage 30.5%. `table2.json` stores this variant as `sf1_strict_k2`.

- ECHO and per-candidate `solutions` agree on every paired ok cell (SF1 3085/3085, SF10 785/785). MASC's adapter reports a different `solutions` value on 250 SF1 and 75 SF10 paired cells, all IS4; this is the adapter's match-count semantics, not a row-set mismatch in ECHO. The MASC time/work cells are kept, but its `solutions` column is not used as a correctness check.

- SF1 peak RSS is measured under the shared read-only graph cache; it is much smaller than the old per-process-load cells and is not comparable to them in absolute terms. All methods in this table use the same cache and the same sidecar, so the relative column is comparable.

