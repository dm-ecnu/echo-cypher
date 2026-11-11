# Table II by template (LDBC Exp-1)

Same cells and same-round five-repeat medians as `table2.md`. Ratio is per-candidate/ECHO; >1 means ECHO is faster. `n` counts rounds with at least one candidate; templates with fewer than three rounds are shown but their interval is weak. `W/L/T` counts rounds where ECHO beat / lost to / tied per-candidate. `Fallback` is the share of ECHO's completed candidate slots that went to Fallback. `red(R)` is the median per-round `1 - W(ECHO)/W(per-candidate)`. `plan` is the median MCS vertices / edges and envelope count over completed ECHO cells. `ok/to/err` counts ECHO repeats.

## SF1

| Template | n | candidates | Time (per-cand/ECHO) | Work | W/L/T | Fallback | red(R) | ECHO | per-cand | plan (V/E, env) | ECHO ok/to/err | per-cand ok/to/err |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|---|
| IC11 | 50 | 150 | 1.001 [0.997,1.005] | 1.000 | 26/24/0 | 94.7% | 0.000 | 70.5 ms | 70.8 ms | 0/0, 0 | 250/0/0 | 250/0/0 |
| IC12 | 50 | 240 | 0.999 [0.997,1.001] | 1.000 | 18/27/5 | 95.6% | 0.000 | 74.7 s | 74.9 s | 0/0, 0 | 225/0/25 | 225/0/25 |
| IC2 | 50 | 355 | 0.958 [0.948,0.968] | 0.970 | 5/45/0 | 58.2% | 0.000 | 288.6 ms | 283.0 ms | 0/0, 0 | 250/0/0 | 250/0/0 |
| IC3 | 20 | 30 | 1.002 [0.983,1.020] | 1.000 | 10/10/0 | 100.0% | 0.000 | 98.2 ms | 98.4 ms | 0/0, 0 | 100/0/0 | 100/0/0 |
| IC5 | 30 | 85 | 0.999 [0.994,1.003] | 1.000 | 16/14/0 | 100.0% | 0.000 | 121.7 ms | 122.0 ms | 0/0, 0 | 150/0/0 | 150/0/0 |
| IC6 | 50 | 235 | 0.997 [0.991,1.003] | 1.000 | 26/24/0 | 100.0% | 0.000 | 671.8 ms | 663.7 ms | 0/0, 0 | 250/0/0 | 250/0/0 |
| IC8 | 50 | 390 | 14.5 [8.415,24.4] | 3.249 | 49/1/0 | 5.4% | 0.689 | 11.1 ms | 101.5 ms | 0/0, 1 | 250/0/0 | 250/0/0 |
| IC9 | 50 | 195 | 0.728 [0.560,0.932] | 1.000 | 8/22/20 | 100.0% | 0.000 | 202.6 s | 241.3 s | 0/0, 0 | 115/20/115 | 115/34/101 |
| IS1 | 50 | 400 | 0.900 [0.850,0.946] | 1.000 | 16/34/0 | 22.5% | 0.000 | 203 µs | 191 µs | 0/0, 0 | 250/0/0 | 250/0/0 |
| IS2 | 40 | 145 | 1.000 [0.994,1.005] | 1.000 | 15/15/10 | 100.0% | 0.000 | 11.77 s | 11.55 s | 0/0, 0 | 150/0/50 | 150/0/50 |
| IS3 | 25 | 40 | 0.939 [0.883,0.983] | 1.000 | 6/19/0 | 83.3% | 0.000 | 137 µs | 135 µs | 0/0, 0 | 125/0/0 | 125/0/0 |
| IS4 | 50 | 400 | 0.981 [0.952,1.009] | 1.000 | 25/25/0 | 56.2% | 0.000 | 170 µs | 169 µs | 0/0, 0 | 250/0/0 | 250/0/0 |
| IS5 | 50 | 400 | 0.897 [0.851,0.943] | 0.982 | 13/37/0 | 21.2% | 0.000 | 170 µs | 166 µs | 0/0, 0 | 250/0/0 | 250/0/0 |
| IS6 | 50 | 275 | 1.326 [1.185,1.481] | 1.403 | 33/17/0 | 43.8% | 0.077 | 80.5 ms | 132.0 ms | 1/0, 1 | 250/0/0 | 250/0/0 |
| IS7 | 45 | 130 | 6.775 [3.712,12.6] | 1.563 | 33/12/0 | 8.7% | 0.354 | 21.4 ms | 57.7 ms | 2/2, 1 | 225/0/0 | 225/0/0 |

## SF10

| Template | n | candidates | Time (per-cand/ECHO) | Work | W/L/T | Fallback | red(R) | ECHO | per-cand | plan (V/E, env) | ECHO ok/to/err | per-cand ok/to/err |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|---|
| IC11 | 14 | 44 | 1.584 [0.773,3.292] | 0.353 | 9/5/0 | 13.8% | -0.024 | 543.4 ms | 814.2 ms | 1/0, 1 | 70/0/0 | 70/0/0 |
| IC12 | 14 | 74 | 1.007 | 1.000 | 3/0/11 | 13.3% | -0.000 | 228.9 ms | 2.05 s | 1/0, 1 | 15/0/55 | 5/0/65 |
| IC2 | 14 | 100 | 1.203 [0.971,1.794] | 0.921 | 7/7/0 | 0.0% | -0.271 | 1.65 s | 1.63 s | 7883/7882, 1 | 70/0/0 | 70/0/0 |
| IC3 | 6 | 13 | 0.996 [0.977,1.019] | 1.000 | 3/3/0 | 33.3% | 0.000 | 5.27 s | 5.31 s | 14/26, 1 | 30/0/0 | 30/0/0 |
| IC5 | 9 | 25 | 0.690 [0.456,0.960] | 0.133 | 3/6/0 | 47.2% | -0.008 | 5.93 s | 1.88 s | 23/44, 1 | 45/0/0 | 45/0/0 |
| IC6 | 14 | 65 | 0.792 [0.580,1.153] | 0.178 | 3/5/6 | 21.2% | -2.738 | 7.44 s | 5.83 s | 20/38, 1 | 40/0/30 | 40/0/30 |
| IC8 | 14 | 107 | 43.8 [9.041,215] | 2.425 | 14/0/0 | 7.9% | 0.546 | 118.0 ms | 1.30 s | 1/0, 1 | 70/0/0 | 70/0/0 |
| IC9 | 15 | 66 | 0.649 [0.420,1.000] | 0.000 | 5/3/7 | 50.0% | -2616 | 3600.0 s | 3600.0 s | 19/36, 1 | 10/35/22 | 10/19/38 |
| IS1 | 15 | 120 | 0.823 [0.737,0.916] | 0.901 | 2/13/0 | 14.2% | 0.000 | 371 µs | 308 µs | 0/0, 0 | 75/0/0 | 75/0/0 |
| IS2 | 12 | 45 | 0.716 [0.532,0.955] | 1.000 | 4/5/3 | 11.1% | 0.000 | 922.7 s | 403.0 s | 1/0, 1 | 45/0/15 | 45/0/15 |
| IS3 | 15 | 106 | 0.984 [0.932,1.027] | 0.922 | 9/6/0 | 16.8% | 0.000 | 365 µs | 363 µs | 0/0, 0 | 75/0/0 | 75/0/0 |
| IS4 | 15 | 120 | 1.012 [0.981,1.042] | 1.000 | 8/7/0 | 58.3% | 0.000 | 218 µs | 221 µs | 0/0, 0 | 75/0/0 | 75/0/0 |
| IS5 | 15 | 120 | 0.952 [0.886,1.010] | 0.988 | 7/8/0 | 23.3% | 0.000 | 186 µs | 187 µs | 0/0, 0 | 75/0/0 | 75/0/0 |
| IS6 | 15 | 74 | 2.584 [1.551,4.472] | 2.087 | 7/0/8 | 13.5% | 0.103 | 502.3 ms | 1.33 s | 3/2, 2 | 35/0/40 | 35/0/40 |
| IS7 | 13 | 33 | 44.2 [10.3,189] | 1.582 | 13/0/0 | 4.1% | 0.500 | 2.3 ms | 440.5 ms | 2/2, 1 | 65/0/0 | 65/0/0 |

`red(R)` below -10 is a one-sided work blow-up, not a normal percentage: the SF10 IC9 row's -2616 comes from rounds 0702/0704, where ECHO's two-member envelope does 276,337,532 work against 105,612 per-candidate (wall 10.1 s vs 2.7 s). The SF1 IC9 row's heavy tail includes ECHO repeats at ~3,182 s while per-candidate finishes that round in ~0.5 ks.

## Templates where ECHO loses to per-candidate

A template is listed when the point estimate of per-candidate/ECHO is below 1 (ECHO slower). Section A is the systematic set (95% interval below 1); section B is the near-tie set (interval includes 1). The clues are measurements, not causal claims. `red(R)` below -10 marks a one-sided work blow-up and is not a normal percentage.

### SF1

**A. Interval below 1 (systematic)**

- **IC9** (n=50, 195 candidates): per-cand/ECHO 0.728 [0.560,0.932], median 0.992, ECHO W/L/T 8/22/20. Clues: Fallback share 100%; most candidates are evaluated alone; median plan has no MCS/envelope (0 vertices, 0 envelopes); ECHO errors more (115 vs 101 error repeats); heavy tail: slowest ECHO round is 18× its median; gmean is below the median (median 0.992); a few rounds drive the loss; marginal median wall (not paired) 202.6 s vs 241.3 s.
- **IS5** (n=50, 400 candidates): per-cand/ECHO 0.897 [0.851,0.943], median 0.962, ECHO W/L/T 13/37/0. Clues: sub-10 ms point/index lookup; no shared expansion to amortise; Fallback share 21%; median plan has no MCS/envelope (0 vertices, 0 envelopes); ECHO does more work (work ratio 0.982 < 1); gmean is below the median (median 0.962); a few rounds drive the loss; marginal median wall (not paired) 170 µs vs 166 µs.
- **IS1** (n=50, 400 candidates): per-cand/ECHO 0.900 [0.850,0.946], median 0.947, ECHO W/L/T 16/34/0. Clues: sub-10 ms point/index lookup; no shared expansion to amortise; Fallback share 22%; median plan has no MCS/envelope (0 vertices, 0 envelopes); marginal median wall (not paired) 203 µs vs 191 µs.
- **IS3** (n=25, 40 candidates): per-cand/ECHO 0.939 [0.883,0.983], median 0.979, ECHO W/L/T 6/19/0. Clues: sub-10 ms point/index lookup; no shared expansion to amortise; Fallback share 83%; most candidates are evaluated alone; median plan has no MCS/envelope (0 vertices, 0 envelopes); heavy tail: slowest ECHO round is 197× its median; gmean is below the median (median 0.979); a few rounds drive the loss; marginal median wall (not paired) 137 µs vs 135 µs.
- **IC2** (n=50, 355 candidates): per-cand/ECHO 0.958 [0.948,0.968], median 0.969, ECHO W/L/T 5/45/0. Clues: Fallback share 58%; most candidates are evaluated alone; median plan has no MCS/envelope (0 vertices, 0 envelopes); ECHO does more work (work ratio 0.970 < 1); marginal median wall (not paired) 288.6 ms vs 283.0 ms.

**B. Interval includes 1 (near tie)**

- **IS4** (n=50, 400 candidates): per-cand/ECHO 0.981 [0.952,1.009], median 0.996, ECHO W/L/T 25/25/0. Clues: sub-10 ms point/index lookup; no shared expansion to amortise; Fallback share 56%; most candidates are evaluated alone; median plan has no MCS/envelope (0 vertices, 0 envelopes); marginal median wall (not paired) 170 µs vs 169 µs.
- **IC6** (n=50, 235 candidates): per-cand/ECHO 0.997 [0.991,1.003], median 1.000, ECHO W/L/T 26/24/0. Clues: Fallback share 100%; most candidates are evaluated alone; median plan has no MCS/envelope (0 vertices, 0 envelopes); heavy tail: slowest ECHO round is 7× its median; marginal median wall (not paired) 671.8 ms vs 663.7 ms.
- **IC5** (n=30, 85 candidates): per-cand/ECHO 0.999 [0.994,1.003], median 1.000, ECHO W/L/T 16/14/0. Clues: Fallback share 100%; most candidates are evaluated alone; median plan has no MCS/envelope (0 vertices, 0 envelopes); heavy tail: slowest ECHO round is 227× its median; marginal median wall (not paired) 121.7 ms vs 122.0 ms.
- **IC12** (n=50, 240 candidates): per-cand/ECHO 0.999 [0.997,1.001], median 0.999, ECHO W/L/T 18/27/5. Clues: Fallback share 96%; most candidates are evaluated alone; median plan has no MCS/envelope (0 vertices, 0 envelopes); heavy tail: slowest ECHO round is 15× its median; marginal median wall (not paired) 74.7 s vs 74.9 s.
- **IS2** (n=40, 145 candidates): per-cand/ECHO 1.000 [0.994,1.005], median 1.000, ECHO W/L/T 15/15/10. Clues: Fallback share 100%; most candidates are evaluated alone; median plan has no MCS/envelope (0 vertices, 0 envelopes); heavy tail: slowest ECHO round is 21× its median; marginal median wall (not paired) 11.77 s vs 11.55 s.

### SF10

**A. Interval below 1 (systematic)**

- **IC5** (n=9, 25 candidates): per-cand/ECHO 0.690 [0.456,0.960], median 0.998, ECHO W/L/T 3/6/0. Clues: Fallback share 47%; ECHO does more work (work ratio 0.133 < 1); heavy tail: slowest ECHO round is 135× its median; gmean is below the median (median 0.998); a few rounds drive the loss; marginal median wall (not paired) 5.93 s vs 1.88 s.
- **IS2** (n=12, 45 candidates): per-cand/ECHO 0.716 [0.532,0.955], median 0.999, ECHO W/L/T 4/5/3. Clues: Fallback share 11%; gmean is below the median (median 0.999); a few rounds drive the loss; marginal median wall (not paired) 922.7 s vs 403.0 s.
- **IS1** (n=15, 120 candidates): per-cand/ECHO 0.823 [0.737,0.916], median 0.941, ECHO W/L/T 2/13/0. Clues: sub-10 ms point/index lookup; no shared expansion to amortise; Fallback share 14%; median plan has no MCS/envelope (0 vertices, 0 envelopes); ECHO does more work (work ratio 0.901 < 1); marginal median wall (not paired) 371 µs vs 308 µs.

**B. Interval includes 1 (near tie)**

- **IC9** (n=15, 66 candidates): per-cand/ECHO 0.649 [0.420,1.000], median 1.000, ECHO W/L/T 5/3/7. Clues: Fallback share 50%; most candidates are evaluated alone; one-sided work blow-up: red(R) median -2616, i.e. ECHO's shared envelope does orders of magnitude more work than per-candidate on some rounds; ECHO hits the cap more often (35 vs 19 timeout repeats); gmean is below the median (median 1.000); a few rounds drive the loss; marginal median wall (not paired) 3600.0 s vs 3600.0 s.
- **IC6** (n=14, 65 candidates): per-cand/ECHO 0.792 [0.580,1.153], median 0.638, ECHO W/L/T 3/5/6. Clues: Fallback share 21%; ECHO does more work (work ratio 0.178 < 1); marginal median wall (not paired) 7.44 s vs 5.83 s.
- **IS5** (n=15, 120 candidates): per-cand/ECHO 0.952 [0.886,1.010], median 0.996, ECHO W/L/T 7/8/0. Clues: sub-10 ms point/index lookup; no shared expansion to amortise; Fallback share 23%; median plan has no MCS/envelope (0 vertices, 0 envelopes); ECHO does more work (work ratio 0.988 < 1); marginal median wall (not paired) 186 µs vs 187 µs.
- **IS3** (n=15, 106 candidates): per-cand/ECHO 0.984 [0.932,1.027], median 1.015, ECHO W/L/T 9/6/0. Clues: sub-10 ms point/index lookup; no shared expansion to amortise; Fallback share 17%; median plan has no MCS/envelope (0 vertices, 0 envelopes); ECHO does more work (work ratio 0.922 < 1); marginal median wall (not paired) 365 µs vs 363 µs.
- **IC3** (n=6, 13 candidates): per-cand/ECHO 0.996 [0.977,1.019], median 0.994, ECHO W/L/T 3/3/0. Clues: Fallback share 33%; marginal median wall (not paired) 5.27 s vs 5.31 s.

## Reading the two SFs together

- The systematic ECHO losses on SF1 are the point-lookup templates (IS1, IS3, IS4, IS5), IC2's mostly-Fallback rounds, and IC9's heavy tail. SF1 wins are IC8 (shared range/count) and IS6/IS7 (topology).

- On SF10 the losing set is IC5/IC6/IC9 plus the small point templates; IC8, IS6 and IS7 are the clear wins. IC2 and IC12 are near ties with large plans or almost complete Fallback.

- A ratio above 1 in `table2.md` is a geometric mean and can coexist with a below-50% round win share: ECHO wins a few heavy rounds by a lot and loses many small rounds by a little. The W/L/T columns make that split explicit.

