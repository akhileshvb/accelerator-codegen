# SF3000 audit of the DuckDB accelerators

Status: measurements and findings only; no accelerator was changed. Done 2026-10-15 on branch `skel/audit`.

Scope: the 22 TPC-H accelerators (`accelerators/tpch/q*/duckdb/aux`) and the 6 SEC-EDGAR ones. The question is whether each works at SF3000 (lineitem 1.8e10 rows, orders 4.5e9, partsupp 2.4e9, part 6e8, customer 4.5e8, supplier 3e7, `o_orderkey` up to 1.8e10, sparse) held fully in memory. The earlier audit (`/home/akhilvb/accelerator-codegen/docs/sf3000-readiness-audit.md`, estimates only) is re-checked finding by finding in the last section.

## How this was measured

- Databases: SF1 and SF20 (existing files), SF5 and SF10 generated with DuckDB's `dbgen` and loaded into the SF1 DDL (BIGINT keys, DECIMAL(15,2), NOT NULL, no indexes, same table order), plus two SF1 variants: `shift` (every key shifted so its maximum equals SF3000's: orderkey 1.8e10, partkey 6e8, suppkey 3e7, custkey 4.5e8) and `big` (every summed money column x3000, the sum magnitudes of SF3000).
- Driver: `eval/compare/run_compare.py read|refresh` over `aux_compare` (new `refresh` mode, `read` now reports the state size). One process per accelerator, `taskset -c 12-23`, DuckDB threads 12. The whole machine was loaded by other jobs (load average 40-65 on 40 cores) during the runs, so timings are upper bounds; the Q9 SF20 build (368 s) is the one that visibly suffered, so Q9's build is fitted on SF1-10.
- Build: `Build()` timed, single-threaded by construction (one streaming scan, one callback thread).
- State size: settled RSS after the build minus settled RSS after the accelerator is destroyed (DuckDB's buffer cache dropped, heap trimmed). `substrate/aux/state/aux_state_footprint.cpp` is a stub (`ValidPartitionCount() * 64`), so it cannot be used. Precision is about +-30 MB; "0" below means below that. Growth is fitted as a line through SF5/10/20 (MB per SF unit); the base database is 270 MB per SF unit.
- Reads: median over 6 random parameter sets per accelerator (fresh draw, TPC-H qgen domains, Q11 fraction 0.0001/SF), 3 repetitions each, answers checked against the DuckDB query (0 mismatches at SF1-20, 0 fallbacks).
- Refresh: one RF1 + RF2 pair x2, one commit each, maintenance time = `AcceleratorStats::apply_us` (PRE_COMMIT). SF1-20 use the official `dbgen -U` update sets (0.1% of orders: 1500 x SF orders and their lineitems); SF5 and SF10 sets were generated with `scripts/gen_tpch_refresh_sets.sh`. `ApplyDelta` is forced (`ACCEL_FORCE_MAINTENANCE_ACTION`) because the cost models exist only for SF1 and SF20 and the default model often picks "invalidate" (see "Maintenance policy"). Two further SF20 runs clone existing orders in batches of 150k and 600k orders: the refresh size of SF100 and SF400 (clone mode over-represents very large orders, which inflates Q18 only; its SF100/400 points are ignored).
- Projection to SF3000: power-law fit in SF for build and read; state is linear. Refresh: 7.5 x the SF400 time (linear in rows). That is a floor for the fixed-shard copy-on-write structures, whose cost per insert also grows with shard size (see Q4/Q8 below).
- Raw results: `eval/bench/`-style JSON, not committed (scratch only); rerun with the commands at the end.

## Already broken

| What | Evidence | From SF |
|---|---|---|
| Q1 `sum_charge` truncated to its low 64 bits | `big` database: expected 167727195668483.076004, accelerator 1706499005097.111456. `module.cpp:262,738-744` (`HugeintLower`); `core::ScaledDecimal128` exists in `aux_types.hpp:54` but Q1 does not use it | ~80 (N/O group; scale-6 int64 holds 9.2e12) |
| Q2 silently drops every part/supplier key >= 32,000,000 | `shift` database: Q2 returns 0 rows, expected 100. `module.cpp:126-130`, skips at 454, 483, 511, 716, 736, 753, 1271-1328. Wrong answer, no error and no fallback | 160 |
| Q3 throws on orderkey > 2^32 | `shift` database: "TPC-H Q3 orderkey exceeds uint32 packed range" (`module.hpp:121`); `BuildState` also skips such lineitems (`module.cpp:1627`) | 716 |
| Q20 `series_off` / `name_off` are `uint32_t`, written with unchecked `static_cast` (`module.cpp:442-443, 463`) | read; wraps at 4.3e9 entries / 4.3e9 name bytes: silent corruption | name arena ~215, series ~715 |
| Q13 allocates arrays up to the maximum custkey | `shift` database (SF1 data, custkey up to 4.5e8): 56 GB RSS, OOM-killed once next to other jobs. A dense per-key layout, not a bug at real SF3000 (32 GB there) but see Q13 | any sparse/offset key space |
| Maintenance cap 1,048,576 changed rows not scaled | `aux_maintenance_policy.cpp:149-157` scales it by `data_rows_hint / calibrated_scale_rows`, but `AuxEngineServices::DataScaleRows()` is never overridden (returns 0) and no cost model sets `calibrated_scale_rows`. SF20, 600k-order refresh (3M rows), default policy: 19 of 22 accelerators did not apply the delta (state invalidated, rebuilt on the next read), Q5 chose RebuildAll | RF exceeds the cap from SF ~140 (Q15 ~59: it triples deltas) |
| Driver limits | `aux_compare` refused keys >= 2e9 (removed here); it also sorts whole tables at setup (`ORDER BY ... OFFSET count/2` on orders, customer, part, supplier; `GROUP BY l_orderkey`) | ~300 |

On the `shift` database the other 19 accelerators pass the 22-query differential (read-only, incremental, adversarial writes), Q13 excepted for memory. A pass can still hide a SQL fallback (Q9 does per-order point lookups for orderkeys >= 2^28 and passes). On the `big` database all pass except Q1; Q8 reported "no probe candidate changed the result", a harness limit under inflated values, not a mismatch.

## Summary

Classes: **Fine** = bounded state, flat costs. **Grows-OK** = linear and fits (state <= ~5% of base data = 13 MB per SF unit = 40 GB at SF3000, refresh apply <= ~10 s, reads <= ~100 ms). **Doesn't scale** = anything else, including wrong answers.

Total state of the 22 accelerators at SF3000 (fitted): **3.5 TB, 4.4x the base database** (~0.8 TB). Single-threaded builds add up to ~140 hours (largest: Q3, Q5, Q8 ~16-17 h, Q9 ~13 h, Q17 11 h).

| Q | State today at SF3000 (MB/SF, % of base) | Build, 1 thread | Read | Refresh (est.) | Class | Why |
|---|---|---|---|---|---|---|
| 1 | ~0 | 0.2 h | 0.9 ms (SF20) | 12 s | Doesn't scale (wrong answer, trivial fix) | sum_charge overflow; refresh cost is per-row (all lineitem accelerators pay ~2.5 us per changed row) |
| 2 | 149 GB (51, 19%) | 2.8 h | 1.2 ms | none | Doesn't scale | wrong answer at SF>=160; state 19% of base |
| 3 | 422 GB (144, 53%) | 15.7 h | 0.7 ms | ~17 s | Doesn't scale | uint32 orderkey; state; fixed 2^18 shards |
| 4 | 113 GB (39, 14%) | 3.6 h | 0.7 ms | ~17 s (+ quadratic memmove) | Doesn't scale | state; 2^16 shards of sorted vectors |
| 5 | 160 GB (54, 20%) | 16.4 h | 0.7 ms | ~17 s | Doesn't scale | per-order index; 2^17 shards |
| 6 | ~0 | 0.1 h | 0.6 ms | 6 s | Fine | |
| 7 | 83 GB (28, 11%) | 0.6 h | 0.7 ms | ~12 s | Doesn't scale (memory) | per-order custkey map |
| 8 | 127 GB (43, 16%) | 17.3 h | 0.6 ms | ~29 s | Doesn't scale | per-order map; dense limit 2^27 (parts, customers beyond it: binary search per lineitem); super-linear RF |
| 9 | 669 GB (228, 85%) | ~13 h (SF1-10 fit) | 0.9 ms | ~120 s (cliff at 600k: 16 s) | Doesn't scale | state; keys >= 2^28 fall to per-row SQL; 4096 fixed slots |
| 10 | 244 GB (83, 31%) | 0.4 h (SQL-parallel) | 0.7 ms | ~28 s | Doesn't scale | customer strings in an `unordered_map`; 41 + 43 windows touched per refresh |
| 11 | 296 GB (101, 37%) | 3.0 h | 1.3 ms (output-bound) | none | Doesn't scale | state; unused by refresh |
| 12 | 96 GB (33, 12%) | 4.5 h | 0.6 ms | ~4 s (RF1 at SF400 falls back: state invalidated) | Doesn't scale | zigzag-dense array 24 B per order |
| 13 | 366 GB (125, 46%) | 5.0 h | 0.5 ms (tracked patterns) | ~18 s | Doesn't scale | every order comment kept; token index disabled from SF~715 |
| 14 | ~0 (+23 GB overflow map above SF80) | 0.1 h | 0.5 ms | 4 s | Grows-OK | partkey >= 2^24 goes to an `unordered_map` (5.8e8 entries) |
| 15 | 23 GB (8, 3%) | 1.2 h | 0.6 ms | ~12 s | Grows-OK (borderline refresh) | 86 windows x dense suppkey array |
| 16 | 70 GB (24, 9%) | 0.3 h (sorted-vector insert is O(group)) | 5.7 ms, 16 ms (SF3000) | none | Doesn't scale (mild) | memory 9%; build super-linear in group size |
| 17 | 408 GB (139, 52%) | 11.3 h | 0.6 ms | ~10 s | Doesn't scale | 470 B per part |
| 18 | ~0 | 0.1 h | 1.0 ms (11 ms SF3000) | 3 s | Fine | per-order SQL lookups only for orders crossing the 300-quantity line (real RF: negligible) |
| 19 | 15 GB (5, 2%) | 0.1 h | 0.6 ms | rebuild | Doesn't scale (maintenance) | overlay cap 300,000 (`module.cpp:927`, counts touched parts plus qualifying-lineitem deltas, ~420 x SF per RF) forces a RebuildAll every ~300k/(420 x SF) commits (every ~35 commits at SF20) and every commit above SF~700; dense dim limit 2^26 (sparse `unordered_map` beyond) |
| 20 | 228 GB (78, 29%) | 1.6 h | 3.9 ms (SF20), ~290 ms (SF3000) | ~16 s | Doesn't scale | read linear in SF; uint32 offsets; 8 B per lineitem |
| 21 | 55 GB (19, 7%) | 0.6 h | 0.6 ms | ~94 s (cliff 12.6 s at SF400) | Doesn't scale (maintenance) | overlay flatten at 200k orders deep-copies the whole map |
| 22 | 15 GB (5, 2%) | 0.1 h | 0.6 ms | ~1 s (+~18 GB shard clone/RF above SF335) | Grows-OK | custkey > 2^26 goes to a 64-shard hash map; fix is small |

Rows are ordered by query. The per-SF measurements behind them follow.

## Measured: TPC-H, per SF

Build s, state MB, read ms (median over random parameters), refresh ms (average RF1/RF2 PRE_COMMIT, `ApplyDelta`), and refresh seconds at the size of SF100 and SF400 (`*`: the SF400 RF1 invalidated the state, so only RF2 is averaged). Q2, Q11, Q16 read only tables the refresh functions do not touch.

| Q | build s SF1/5/10/20 | state MB SF5/10/20 | read ms SF1/5/10/20 | refresh ms SF1/5/10/20 | refresh s SF100 / SF400 |
|---|---|---|---|---|---|
| 1 | 0.1/0.2/0.4/2.8 | 7/10/1 | 0.44/0.50/0.62/0.87 | 7/26/48/67 | 0.4 / 1.7 |
| 2 | 0.4/2.5/5.4/23.4 | 236/492/999 | 0.59/0.77/0.79/1.20 | - | - |
| 3 | 3.8/24.0/49.1/154.0 | 707/1414/2867 | 0.57/0.65/0.71/0.68 | 12/137/85/193 | 0.7 / 2.3 |
| 4 | 2.0/12.6/23.2/55.4 | 178/374/760 | 0.47/0.65/0.57/0.67 | 6/51/61/174 | 1.1 / 2.3 |
| 5 | 2.6/21.9/44.6/114.7 | 256/527/1073 | 0.49/0.68/0.62/0.74 | 10/66/113/244 | 1.3 / 2.3 |
| 6 | 0.1/0.6/0.8/2.6 | 0/-7/-4 | 0.32/0.81/0.48/0.59 | 2/17/15/54 | 0.3 / 0.8 |
| 7 | 0.5/6.0/4.9/13.1 | 140/278/565 | 0.57/0.83/0.53/0.65 | 4/42/54/105 | 0.6 / 1.6 |
| 8 | 3.2/29.8/48.1/137.1 | 178/383/825 | 0.49/0.69/0.53/0.57 | 15/100/181/516 | 1.3 / 3.9 |
| 9 | 6.0/35.2/79.8/367.6 | 1112/2243/4535 | 0.59/0.57/0.58/0.93 | 15/58/119/269 | 0.5 / 16.1 |
| 10 | 0.8/3.1/7.2/13.6 | 392/794/1640 | 0.58/0.62/0.64/0.71 | 19/123/163/325 | 0.9 / 3.7 |
| 11 | 0.9/4.6/15.6/27.8 | 482/993/1999 | 0.36/0.61/0.89/1.29 | - | - |
| 12 | 3.0/15.6/37.7/73.6 | 167/333/661 | 0.49/0.52/0.55/0.62 | 3/23/19/32 | 0.1 / 0.6* |
| 13 | 3.4/16.9/41.9/82.6 | 660/1309/2536 | 0.45/0.48/0.56/0.53 | 7/60/105/290 | 0.7 / 2.5 |
| 14 | 0.1/0.5/1.3/2.2 | 0/-2/-1 | 0.40/0.45/0.46/0.48 | 1/9/13/22 | 0.1 / 0.5 |
| 15 | 0.4/2.5/6.3/12.5 | 33/75/153 | 0.42/0.59/0.50/0.57 | 4/52/59/120 | 0.4 / 1.6 |
| 16 | 0.5/1.8/4.9/9.0 | 111/225/467 | 3.33/4.41/6.71/5.71 | - | - |
| 17 | 3.4/20.3/56.1/109.4 | 623/1333/2713 | 0.45/0.47/0.69/0.63 | 7/40/58/133 | 0.5 / 1.3 |
| 18 | 0.2/0.5/1.5/2.5 | -4/-30/-32 | 0.47/0.52/0.89/0.99 | 1/12/12/21 | (clone artifact) |
| 19 | 0.2/0.7/2.5/2.8 | 33/52/110 | 0.51/0.43/0.63/0.59 | 2/15/13/28 | 0.2 / 0.7 |
| 20 | 1.1/4.4/15.0/26.0 | 388/766/1552 | 0.71/1.20/2.58/3.91 | 5/52/80/181 | 0.6 / 2.2 |
| 21 | 0.4/1.7/5.3/9.9 | 58/148/337 | 0.51/0.51/0.69/0.61 | 5/35/54/160 | 1.3 / 12.6 |
| 22 | 0.2/0.7/1.6/3.5 | 19/44/94 | 0.49/0.47/0.68/0.56 | 1/6/11/21 | 0.1 / 0.1 |

Growth, fitted on SF1-20 (and SF100/400 for refresh): build exponent 0.9-1.3 (linear, Q3/Q5/Q8/Q2 ~1.2: hash and sort costs); state linear; refresh exponent ~0.8-1.1 with the cliffs noted (Q9 and Q21 at SF400, Q12 RF1 at SF400, Q18 only in clone mode); read flat for 17 accelerators, linear for Q20 (0.7 -> 3.9 ms, exponent 0.85) and growing slowly for Q16 (3.3 -> 5.7 ms), Q18, Q11. Reads at 100 ms: only Q20 is over.

Two behaviours that do not show up in the exponents:
- **Copy-on-write saturation.** Q3, Q4, Q5, Q7, Q8, Q12, Q13 keep orders in fixed-count shards or pages (Q4 2^16, Q3 2^18, Q5 2^17, Q8 2^16, Q7 2^18). At SF400 an RF already touches every shard, so cost per refresh is "copy the whole structure": linear in state (SF3000: 70-140 GB per refresh for Q3/Q4/Q5/Q7/Q8; Q12 42 GB). The sorted-vector shard also memmoves O(shard) per insert, shard size grows with SF, so the true SF3000 cost is above the 7.5x extrapolation (quadratic in SF: Q4/Q7/Q8 ~3.6 TB moved per RF by the code's own arithmetic).
- **Policy-selected action.** With the default cost model a commit often *invalidates* instead of applying (Q3 at SF10, Q9 at SF5, Q13, Q18...), so state is rebuilt on the next read. At SF3000 every commit is over the cap (see above) and no cost model exists; this is unmeasured but follows from the code.

## SEC-EDGAR (6 accelerators)

SEC-EDGAR has no scale factor; `sec_edgar_sf3` (3 years of filings, 86,135 filings, num 41.2M, pre 9.6M, tag 1.07M rows, 1.7 GB) was cut to 1/3 and 2/3 of the filings (by hash of `adsh`) and measured with `bench_sec_edgar_batch_apply` (single thread, as the tool pins it) and `measure_shared_sec_edgar_aux_memory` (RSS delta). Parameters: 8 random draws per accelerator from `eval/bench/sec_edgar/params/q*.json`. Maintenance: UPDATE batches of 500, 5,000, 50,000 rows applied as one batch.

| Q | build s (1/3, 2/3, full) | state MB (1/3, 2/3, full) | state per base row | read ms (median over draws, full) | apply us per changed row (full) |
|---|---|---|---|---|---|
| 1 | 0.5, 0.2, 0.5 | 18, 43, 50 | 5 B / pre row | 0.34 | ~2 |
| 2 | 2.3, 5.2, 11.7 | 376, 745, 1573 | 38 B / num row | 0.56 | 4-6 |
| 3 | 1.2, 2.1, 6.9 | 182, 365, 943 | 23 B / num row | 0.55 | 5-6 |
| 4 | 2.8, 7.1, 12.4 | 785, 1373, 2224 | 54 B / num row | 0.54 | 4-5 |
| 6 | 6.8, 13.6, 22.7 | 860, 1659, 2548 | 62 B / num row | 0.64 | 4-5 |
| 24 | 3.8, 7.7, 10.2 | 459, 947, 1762 | 43 B / num row | 0.46 | 4 |

Build and state are linear in rows (exponent ~1.0-1.5); reads and per-row maintenance are flat; the state equals 0.5-1.5x the base data (not "a few %"). Projection to `k` times the data: state `k` x the full-column above (Q2 1.6 GB, Q4 2.2 GB, Q6 2.5 GB, Q24 1.8 GB, Q3 0.9 GB), build `k` x (Q6 23 s single-threaded). At 100x that is 150-250 GB per accelerator, a build of 20-40 min single-threaded; the 32-bit ids (`uint32_t` dictionary ids, group ids, `ChunkedCowArray<uint32_t>` in Q2/Q3/Q24, e.g. `q2/common/state.hpp:126-130`, `q3/duckdb/aux/module.hpp:125-170`) hold about 4e9 distinct values, i.e. ~100x the present data. **Class: Grows-OK up to ~10-30x; Doesn't scale in memory beyond ~100x (state ~ base size, 32-bit ids).** SEC-EDGAR was not given a redesign: no scale target exists and the structures are already compact (5-60 B per base row). Not measured: refresh as insert of new filings (the tool applies row UPDATEs), SEC accelerators in the shifted-key or inflated-value databases.

## Cross-cutting findings

1. **Builds are single-threaded.** `scan.hpp:96` `ScanQueryStreaming` is one `SendQuery` stream consumed by one callback thread, one lock per connection, a boxed `duckdb::Value` per cell (`chunk.GetValue`). Measured throughput is ~0.6 us per lineitem row for scan-heavy builds. At SF3000 that is 3-17 hours per accelerator and ~140 hours for all 22 in sequence. Every redesign below needs a key-range-parallel build (N connections, `WHERE key BETWEEN` or hash partition, per-range partial state merged at the end) and a vectorized chunk reader.
2. **Fixed shard / page counts.** `ShardedSortedMap` (<= 2^18 shards, shard = sorted `std::vector`, `Upsert` is a memmove), `ShardedCowHashMap<_, 6..8>` (64-256 shards), 512-slot / 2048-record pages. At SF3000 shards hold 17k-69k entries (0.3-1.6 MB) and one refresh touches all of them. A structure whose copy-on-write granularity is not per row or small fixed bytes copies itself on every refresh.
3. **Per-order state.** Q3, Q4, Q5, Q7, Q8, Q12, Q21 each keep a 16-24 B entry per order (+ index slack) in order to find an order's attributes when its lineitems change. The refresh functions deliver whole orders, so most of it is avoidable (Q5) or compressible to ~5 B per order (Q4, Q7, Q12).
4. **Maintenance policy** (the cap above) needs `DataScaleRows()` implemented by the DuckDB backend plus `calibrated_scale_rows` in the cost models, or a cap proportional to table size. Otherwise every refresh from SF~140 invalidates and rebuilds.
5. **Measurement tooling.** The footprint accounting in the substrate is a stub; sizes here come from settled RSS differences. A real byte accounting per partition (`ComputeAuxStateFootprintBytes`) would make every future audit exact.

## Ranked redesign list

Ranked by what each blocks at SF3000 (wrong answers first, then bytes removed, then maintenance). Effort in person-days for an engineer who knows the codebase; the per-query designs were drafted by reading the code (not prototyped), SF20 read latency is expected to be within 10% of today (reads still hit a small published structure) unless noted.

| # | Item | Fixes | Effort |
|---|---|---|---|
| 1 | Parallel, vectorized build framework (key-range partitioned scans on N connections, DataChunk reader, partial-state merge) | Build 3-17 h -> minutes at 40 threads for all queries; prerequisite of items 5-18 | 8-10 |
| 2 | Maintenance policy: implement `DataScaleRows`, scale the 1,048,576 cap (and rebuild costs) with data size, add SF-scaled cost models | Every refresh from SF~140 is invalidate/rebuild | 2 |
| 3 | Correctness fixes: Q1 128-bit output cell (`ScaledDecimal128`), Q20 64-bit offsets, Q2 dense-key skips -> sparse fallback, Q3 64-bit orderkey, Q9 `kQ9MaxId` | Wrong answers/crashes at SF>=80/160/215/716 | 0.5 + 1 + 2 + 2 + 2 (interim; superseded by items 4/6/10/12) |
| 4 | **Q9**: drop the per-part profit pairs; state = part name tokens (5 B), 4 partsupp slots (32 B), order year bytes, supplier nation, and the published token x nation x year cube; exact rule: cube[token] if exactly one vocabulary token contains the needle else SQL; lineitem delta = 3 array lookups + a cube add. 669 GB -> ~27 GB (9 MB/SF). Loses incremental part/supplier/partsupp updates (rebuild or SQL) | 85% of base data; keys >= 2^28; 4096 fixed slots | 12-15 |
| 5 | **Q17**: per part keep `sum_qty`, `cnt`, price sums for quantities 1..9 only (`qty < 0.2*avg <= 0.2*max_qty`), 46 B per part, in a chunked dense array mutated in place; publish only the per-(brand, container) totals. 408 GB -> ~28 GB. Requires integer quantity in 1..50 (checked at build, else rebuild/SQL) | 52% of base data; 33 GB copy per RF | 8 |
| 6 | **Q3**: keep only orders whose revenue bound can reach the top 10 for any cutoff (verified threshold `T`, SQL fallback when the 10th revenue falls below `T`), 64-bit keys, 2^22 shards, dense 1-byte segment per customer; 422 GB -> ~25-45 GB. Optional domain variant: cutoffs 1995-03-01..31 only | uint32 key; 72 GB + 50 GB copied per RF | 10-14 |
| 7 | **Q13**: no order store; per custkey `mult`, `total`, and one 8-bit match counter per tracked pattern (16 TPC-H patterns): 18 B per customer, histogram + sorted rows published as today. 366 GB -> 8 GB. Untracked patterns go to SQL (arbitrary-pattern exactness in memory needs the comments: >= 75-100 GB) | 46% of base data; `uint16` token index disabled from SF~715; O(shard) delete scan | 6-8 |
| 8 | **Q11**: dense per-part record (4 x {suppkey u32, value u32}), per-nation candidate index of entries >= `c` x spec threshold (`c ~ 0.8`), int128 threshold compare; slow path = parallel rescan when the requested threshold is under the floor. 296 GB -> ~23 GB | 37% of base data, build 2x peak | 8-10 |
| 9 | **Q10**: customer-major compact revenue rows (6 B per customer-month), per-window top-4096 list with a floor (answer only when the 20th entry is above the floor, else rebuild that window), customer strings cached only for list members. 244 GB -> ~14 GB | strings in an `unordered_map` (160 GB), 70-80 GB of window copies per RF | 10-14 |
| 10 | **Q20**: per-partsupp-row yearly sums for 1993-1997 (u8 + overflow), parts ordered by name word, 56 B part record; falls back to SQL outside the spec start dates; 228 GB -> ~41 GB (13-14 MB/SF, on the budget line; exact-for-any-date cannot go below 48 MB/SF). Read becomes a sequential 360 MB scan (~50 ms) instead of ~0.3 s | read linear in SF; uint32 offsets | 12-15 |
| 11 | **Shared primitive `BitmapChunkColumn<V>`** (256-key chunks: popcount bitmap + packed values, COW directory) and its users **Q4** (24 B -> 5 B per order, 113 -> 23 GB), **Q7** (83 -> 23.5 GB), **Q12** (96 -> 9.6 GB, 1 byte per order), **Q21** (tracked-order payload + dense numwait + per-nation `CowSortedBlocks`: 55 -> 9 GB, removes the flatten deep copy) | 4 queries at 83-113 GB and the O(state) copy per refresh | 6 (primitive) + 3 + 5 + 3 + 9 |
| 12 | **Q5**: drop the order index; keep the date tree plus `cust_nation` (1 B per customer) and `supp_nation`; RF carries both sides of every join. 160 GB -> 0.5 GB; refresh 1-2 s | per-order 144 GB map, 144 GB cloned per RF | 5-8 |
| 13 | **Q8**: cube unchanged; part type / customer nation / supplier nation as byte arrays; order table only for orders in 1995-96 (6 B per order). 127 GB -> 9.5 GB | dense limit 2^27, 108 GB clone per RF | 7-10 |
| 14 | **Q2**: per (p_size, region, type) top-200 candidate lists (read = heap merge of the lists matching the LIKE), dense 36 B part record, supplier arena; repair rule for lists that drain. 149 GB -> ~27 GB, exact for any pattern. Interim: a 1-day change that sends keys >= 32M to a sparse map | wrong answer at SF>=160 | 10-12 |
| 15 | **Q16**: per-group u32 supplier multiset in COW sorted blocks, dense 3-byte part record, 4 partsupp slots, bitmap for eligibility; parallel radix-bucketed build (removes the O(group) insert per row). 70 GB -> 22 GB | build super-linear; 23 MB group pages | 6-8 |
| 16 | **Q19**: drop the per-part lineitem index; keep dim codes (4 B per part, chunked dense) and the brand x family x quantity aggregates; a part write re-derives its lineitem contribution with one parallel SQL scan | overlay cap 300,000 forces RebuildAll every commit from SF~700 (every ~35 at SF20) | 4 |
| 17 | **Q22**: split the customer record: dense u8 order count (450 MB) in 4 KB pages for the RF-hot field, 10 B static prefix + balance in a separate array; dense limit raised | 64-shard hash map cloned (~18 GB) per RF above SF335 | 3 |
| 18 | **Q14 / Q15 / Q18**: Q14 raise `kMaxDensePartkey` to a chunked array (0.6 GB); Q15 keep, optionally limit window rescans; Q18 batch the threshold-crossing lookups into one SQL per commit | overflow map; rescans; per-order SQL | 0.5 / 1 / 2 |

Sum of state after items 4-17: ~0.3 TB (from 3.5 TB). Items 4-14 each state "exact for every parameter" except the noted fallbacks (Q9 needles that are not a single vocabulary token, Q13 untracked patterns, Q20 start dates outside 1993-1997, Q3's optional domain variant); all of them drop incremental maintenance of rarely changing dimension tables in favour of a parallel SQL rescan, which RF1/RF2 never trigger. The main shared risk is the in-place mutation with an undo log that items 4, 5, 7 use (the substrate must allow a private mutable structure behind one immutable published image).

## Re-check of the earlier audit

| Earlier finding | Now |
|---|---|
| `aux_compare` aborts for keys > 2e9 | Confirmed; check removed in this branch |
| ApplyDelta cap 1,048,576 rows; all accelerators rebuild | Still true (scale hook not wired; shown at SF20 with a 600k-order refresh). The ~37 h rebuild figure is not reproduced: not measured |
| RebuildAll runs the full `Build()` inside PreCommit, readers wait | Not re-checked (deferred rebuild on read exists in the runtime: the commit invalidates, the next read rebuilds) |
| Q1 `sum_charge` truncated | Confirmed (differential on the `big` database) |
| Q2 keys >= 32M dropped | Confirmed (0 rows on the `shift` database); state is 51 MB/SF, not "18 GB subset" |
| Q20 uint32 offsets | Confirmed by reading; `PairKeyFits` is now 32/32 and exact (its header comment is stale) |
| Q3 orderkey > uint32 throws | Confirmed on the `shift` database |
| Q9 order years only below 2^28 | Confirmed by reading (falls back to per-row SQL; still passes the differential) |
| Q17 double-rounded threshold | Not re-checked |
| Memory ~3.2-3.9 TB (C++) | Reproduced: 3.5 TB fitted from SF5/10/20 |
| Per-query states (Q3 360-450 GB, Q9 650-750, Q13 330, Q17 285, Q10 310, Q11 240, Q20 240, Q2 190-250, Q5 152-230, Q8 125, Q4 108-200, Q12 108, Q7 80) | Reproduced within ~25% for all but Q17 (408 vs "285", build "700 GB" not measured) and Q9/Q8/Q22 as listed above |
| Builds single-threaded with boxed `Value` per cell | Confirmed (`scan.hpp`); measured ~0.6 us per lineitem row |
| Fixed shard counts copy whole maps per refresh | Confirmed by reading and by the SF400-size refresh (all shards touched); the cost at SF3000 itself was not measured |
| Q11 fraction fixed at SF1's 0.0001 | Fixed in the harness (`gen_tpch_params.py` takes the SF) |
| Harness answer check through `%.4f` double | Still present (`aux_compare.cpp` `Canon`) |
| Verus kits | Out of scope here (no kit libraries are built in this worktree) |

## Reproducing

```
cmake -S . -B build-dev -G Ninja ...   # as build-dev of the main worktree
ninja -C build-dev aux_compare test_tpch_shared_differential bench_sec_edgar_batch_apply measure_shared_sec_edgar_aux_memory
export ACCEL_DUCKDB_ROOT=/home/akhilvb/duckdb-dbcapture ACCEL_COMPARE_DUCKDB_THREADS=12
python3 eval/compare/run_compare.py read    --db DB --work W --out O --bin build-dev/aux_compare --systems cpp --sets 6 --repeats 1 --reps 3 --sf SF --baseline-sql-dir workloads/tpch
python3 eval/compare/run_compare.py refresh --db DB --work W --out O --bin build-dev/aux_compare --sf SF --rf-dir UPDATE_SETS --pairs 2 --force-action ApplyDelta
python3 eval/compare/run_compare.py refresh ... --batch-orders 150000   # clone mode: refresh size of SF100 (600000: SF400)
build-dev/test_tpch_shared_differential SHIFTED_DB 3
```
