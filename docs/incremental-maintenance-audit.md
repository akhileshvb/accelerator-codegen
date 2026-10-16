# Incremental-maintenance audit (DuckDB TPC-H Q1-Q22, SEC-EDGAR)

Measured with `build-dev/audit_incremental_maintenance <db> <workload> <queries>` (eval/tools). It builds one
accelerator, commits one write at a time, reads after every commit (which waits for any rebuild), checks the
answer against the base query, and records what the runtime did: commits applied, builds, fallbacks, and the
stale reason (`AcceleratorStats::commits_stale`, `last_stale_reason`, `last_decision`). Cores 24-31.
Cells below read `incremental/full rebuilds` per workload, before -> after. "Ignored" commits (table or columns
the query does not watch; the engine delivers nothing) are neither. Fallbacks: 0 in every cell (reads wait for
the rebuild), so they are omitted. Gaps: 0 (DuckDB commits are in-process).

## Root causes

| # | Cause | Where | Fix |
|---|---|---|---|
| 1 | Default cost models price an ApplyDelta at the first-build time (calibration intercept = a rebuild): Q9 45 s, Q13 12 s, Q2 2.4 s for a refresh, so `RebuildAll` won. Official RF at SF1 rebuilt Q9 and Q13 every commit. | `accelerators/tpch/q*/config/*_aux_cost_model.json` | defaults take the RF-measured apply/rebuild costs of the `_sf1` models; Q2/Q11/Q16/Q17 (no RF model) measured with the audit tool |
| 2 | The scale hook was never wired: nothing overrode `DataScaleRows`, no model set `calibrated_scale_rows`; the ApplyDelta cap (1,048,576 rows) and rebuild costs never scaled. At SF20 official RF (30000 orders, ~120k rows per commit) 7 of 19 accelerators rebuilt. | runtime, DuckDB, Postgres, models | `EngineSlots::TableRows` -> `AcceleratorRuntime::DataScaleRows` (sum over dependency tables); DuckDB: storage row count; Postgres: `pg_class.reltuples`; `calibrated_scale_rows` in 62 DuckDB models |
| 3 | Q1/Q6 treated a shipdate update (Q6: month change, or a quantity outside the matrix domain) as unmaintainable and rebuilt. This is the gate's "86 builds": 4-9% of its lineitem updates. | `q1`, `q6` `module.cpp` | an update is -old/+new deltas (two leaves / two months); an out-of-domain row declines its month (as a build would) instead of rebuilding |
| 4 | SEC-EDGAR Q1, Q2, Q3, Q4, Q24 registered no captured tables: SQL writes never reached the accelerator, which kept answering from stale state (wrong answers; found by the audit's answer check). Only hand-built probe mutations (tests) reached them. | `accelerators/sec_edgar/q*/duckdb/aux/register.cpp` | capture lists with the column projections each module expects (Q1 `pre` {3,5,0,2}, Q3 `sub` {0,2,1,10}, Q24 `pre` {0,6,7}; others read full rows) |

Hypotheses rejected: (b) the runtime's empty-plan -> AllPartitions fallback never fired in any run; (c) a
module asking for a rebuild it does not perform: every stale commit is `pending_rebuild` set by the module, the
runtime reports it (`last_stale_reason`); (d) a build starts only after a stale commit; after fixes 1-3 the
gate rebuilds 0 times in the lineitem mix; (e) catch-up/collapse/gaps: no gap, no unexpected catch-up in DuckDB
runs; (f) projection mapping: an update of an unwatched column is delivered to the module (`updated_columns` =
{projection size}); a runtime filter for it broke the Q3/Q6/Q10 aux smoke tests and was dropped (not fixed:
nation `n_regionkey` updates still rebuild Q9/Q10/Q11/Q20/Q21).

Also added: runtime stats `commits_stale` and `last_stale_reason`; the audit tool; MySQL not measured.

## TPC-H, SF1 (commits; before -> after)

Workloads: slot-gate lineitem mix (60 commits: updates 60%, copies 15%, deletes 15%, new orders 10%); RF1/RF2
in 10-order chunks (6) and at the official 0.1% size (1500 orders, 2 commits); the differential's SQL write set
(RF, lineitem/customer updates, part/supplier insert/delete/rename; 20-27); fuzz on all tables (60).
Cells: incremental/full rebuilds.

| Accelerator | slot-gate lineitem mix | RF 10 orders x6 | RF official 1500 orders x2 | SQL write set | fuzz, all tables |
|---|---|---|---|---|---|
| tpch Q1 | 60c: 56/4 -> 60/0 | 6c: 6/0 -> 6/0 | 2c: 2/0 -> 2/0 | 20c: 8/0 -> 8/0 | 60c: 25/0 -> 25/0 |
| tpch Q2 | 60c: 0/0 -> 0/0 | 6c: 0/0 -> 0/0 | 2c: 0/0 -> 0/0 | 20c: 8/2 -> 10/0 | 60c: 17/2 -> 17/2 |
| tpch Q3 | 60c: 60/0 -> 60/0 | 6c: 6/0 -> 6/0 | 2c: 2/0 -> 2/0 | 27c: 17/0 -> 17/0 | 60c: 40/0 -> 40/0 |
| tpch Q4 | 60c: 60/0 -> 60/0 | 6c: 6/0 -> 6/0 | 2c: 2/0 -> 2/0 | 20c: 8/0 -> 8/0 | 60c: 36/0 -> 36/0 |
| tpch Q5 | 60c: 60/0 -> 60/0 | 6c: 6/0 -> 6/0 | 2c: 2/0 -> 2/0 | 20c: 16/0 -> 16/0 | 60c: 47/0 -> 47/0 |
| tpch Q6 | 60c: 55/5 -> 60/0 | 6c: 6/0 -> 6/0 | 2c: 2/0 -> 2/0 | 20c: 7/1 -> 8/0 | 60c: 25/0 -> 25/0 |
| tpch Q7 | 60c: 60/0 -> 60/0 | 6c: 6/0 -> 6/0 | 2c: 2/0 -> 2/0 | 25c: 21/0 -> 21/0 | 60c: 45/2 -> 45/2 |
| tpch Q8 | 60c: 60/0 -> 60/0 | 6c: 6/0 -> 6/0 | 2c: 2/0 -> 2/0 | 23c: 23/0 -> 23/0 | 60c: 50/2 -> 50/2 |
| tpch Q9 | 60c: 60/0 -> 60/0 | 6c: 6/0 -> 6/0 | 2c: 0/2 -> 2/0 | 20c: 18/0 -> 18/0 | 60c: 52/3 -> 52/3 |
| tpch Q10 | 60c: 60/0 -> 60/0 | 6c: 6/0 -> 6/0 | 2c: 2/0 -> 2/0 | 20c: 10/0 -> 10/0 | 60c: 35/7 -> 35/7 |
| tpch Q11 | 60c: 0/0 -> 0/0 | 6c: 0/0 -> 0/0 | 2c: 0/0 -> 0/0 | 20c: 10/0 -> 10/0 **2 wrong** | 60c: 16/2 -> 16/2 |
| tpch Q12 | 60c: 60/0 -> 60/0 | 6c: 6/0 -> 6/0 | 2c: 2/0 -> 2/0 | 25c: 13/0 -> 13/0 | 60c: 36/0 -> 36/0 |
| tpch Q13 | 60c: 8/0 -> 8/0 | 6c: 6/0 -> 6/0 | 2c: 0/2 -> 2/0 | 20c: 8/0 -> 8/0 | 60c: 22/0 -> 22/0 |
| tpch Q14 | 60c: 60/0 -> 60/0 | 6c: 6/0 -> 6/0 | 2c: 2/0 -> 2/0 | 20c: 12/0 -> 12/0 | 60c: 30/0 -> 30/0 |
| tpch Q15 | 60c: 60/0 -> 60/0 | 6c: 6/0 -> 6/0 | 2c: 2/0 -> 2/0 | 21c: 15/0 -> 15/0 | 60c: 30/0 -> 30/0 |
| tpch Q16 | 60c: 0/0 -> 0/0 | 6c: 0/0 -> 0/0 | 2c: 0/0 -> 0/0 | 21c: 11/0 -> 11/0 | 60c: 17/0 -> 17/0 |
| tpch Q17 | 60c: 60/0 -> 60/0 | 6c: 6/0 -> 6/0 | 2c: 2/0 -> 2/0 | 20c: 12/0 -> 12/0 | 60c: 30/0 -> 30/0 |
| tpch Q18 | 60c: 60/0 -> 60/0 | 6c: 6/0 -> 6/0 | 2c: 2/0 -> 2/0 | 20c: 10/0 -> 10/0 | 60c: 40/0 -> 40/0 |
| tpch Q19 | 60c: 60/0 -> 60/0 | 6c: 6/0 -> 6/0 | 2c: 2/0 -> 2/0 | 22c: 14/0 -> 14/0 | 60c: 30/0 -> 30/0 |
| tpch Q20 | 60c: 60/0 -> 60/0 | 6c: 6/0 -> 6/0 | 2c: 2/0 -> 2/0 | 23c: 21/0 -> 21/0 | 60c: 42/2 -> 42/2 |
| tpch Q21 | 60c: 60/0 -> 60/0 | 6c: 6/0 -> 6/0 | 2c: 2/0 -> 2/0 | 20c: 14/0 -> 14/0 | 60c: 41/2 -> 41/2 |
| tpch Q22 | 60c: 8/0 -> 8/0 | 6c: 6/0 -> 6/0 | 2c: 2/0 -> 2/0 | 20c: 8/0 -> 8/0 | 60c: 22/0 -> 22/0 |

Every accelerator: 100% of delivered commits incremental in the gate mix, RF (both sizes) and SQL write set
after the fixes, 0 fallbacks. `Q11` SQL write set: 2-3 answers differ by tie order only (cloned parts give
equal values, the base query orders ties arbitrarily; the state is right again after the next write); not a
state error as far as the evidence goes, not proven.

### Remaining full rebuilds (fuzz workload only) and why

| Accelerator | Write | Reason (module cannot maintain it incrementally) |
|---|---|---|
| Q2, Q8 | `nation.n_regionkey` update | region membership of every supplier/customer of the nation changes; module comments say rebuild |
| Q9, Q10, Q11, Q20, Q21 | `nation` update (any column) | `nation` mutations always `MarkRebuild`, even if no watched column changed (the update of unwatched columns is delivered); needs a per-module check of `updated_columns`. Not fixed |
| Q7, Q9, Q10 | `orders.o_custkey` / `o_orderdate` update | moves all of an order's lineitem revenue between customers/years/months; needs lineitem lookups. Q10 could take the RebuildPartition path (not done) |
| Q10 | insert of an order dated outside the imaged month range | new month/window images needed |

None occur in RF or the gate mix.

## TPC-H, SF20, official RF (30000 orders, 2 commits)

`AUDIT_RF_ORDERS=30000`, default models (no `ACCEL_AUX_BENCH_DB_LABEL`); "before" = the old models, unscaled.
Q2, Q11, Q16 are not touched by RF. Answers checked.

| | before | after |
|---|---|---|
| incremental / rebuilds per accelerator | Q3, Q9, Q13, Q14, Q18, Q20, Q21: 0/2 (cost policy: RebuildAll); Q1, Q4-Q8, Q10, Q12, Q15, Q17, Q19, Q22: 2/0 | all 19: 2/0 |

Not measured: refresh above 30000 orders (SF100+), Q19's overlay cap (300,000 touched keys: a rebuild every
~35 RF commits at SF20), Q12 RF1 at SF400, Q21 overlay flatten: module caps unrelated to the policy.

## SEC-EDGAR (sec_edgar_sf3)

Probes = the catalog's invalidating probes (3 per query, 3 rounds), applied as hand-built mutations; "as SQL"
= the same probes as plain SQL writes; mix = update/insert/delete on `num`, `sub`, `pre`, `tag`.
| Accelerator | hand-built probe mutations (9 commits) | probes as SQL writes (6 commits), before | after | insert/update/delete mix on num, sub, pre, tag (14 commits), after |
|---|---|---|---|---|
| sec_edgar Q1 | 9 incremental, 0 rebuilds, 0 never delivered, ? wrong answers -> 9 incremental, 0 rebuilds, 0 never delivered, 0 wrong answers | 0 incremental, 0 rebuilds, 6 never delivered, 3 wrong answers | 6 incremental, 0 rebuilds, 0 never delivered, 0 wrong answers | 4 incremental, 0 rebuilds, 10 never delivered, 0 wrong answers |
| sec_edgar Q2 | 9 incremental, 0 rebuilds, 0 never delivered, ? wrong answers -> 9 incremental, 0 rebuilds, 0 never delivered, 0 wrong answers | 0 incremental, 0 rebuilds, 6 never delivered, 3 wrong answers | 6 incremental, 0 rebuilds, 0 never delivered, 0 wrong answers | 7 incremental, 0 rebuilds, 7 never delivered, 0 wrong answers |
| sec_edgar Q3 | 9 incremental, 0 rebuilds, 0 never delivered, ? wrong answers -> 9 incremental, 0 rebuilds, 0 never delivered, 0 wrong answers | 0 incremental, 0 rebuilds, 6 never delivered, 0 wrong answers | 6 incremental, 0 rebuilds, 0 never delivered, 0 wrong answers | 7 incremental, 0 rebuilds, 7 never delivered, 0 wrong answers |
| sec_edgar Q4 | 9 incremental, 0 rebuilds, 0 never delivered, ? wrong answers -> 9 incremental, 0 rebuilds, 0 never delivered, 0 wrong answers | 0 incremental, 0 rebuilds, 6 never delivered, 2 wrong answers | 6 incremental, 0 rebuilds, 0 never delivered, 0 wrong answers | 12 incremental, 0 rebuilds, 2 never delivered, 0 wrong answers |
| sec_edgar Q6 | 9 incremental, 0 rebuilds, 0 never delivered, ? wrong answers -> 9 incremental, 0 rebuilds, 0 never delivered, 0 wrong answers | 6 incremental, 0 rebuilds, 0 never delivered, 0 wrong answers | 6 incremental, 0 rebuilds, 0 never delivered, 0 wrong answers | 11 incremental, 0 rebuilds, 3 never delivered, 0 wrong answers |
| sec_edgar Q24 | 9 incremental, 0 rebuilds, 0 never delivered, ? wrong answers -> 9 incremental, 0 rebuilds, 0 never delivered, 0 wrong answers | 0 incremental, 0 rebuilds, 6 never delivered, 3 wrong answers | 6 incremental, 0 rebuilds, 0 never delivered, 0 wrong answers | 8 incremental, 0 rebuilds, 6 never delivered, 0 wrong answers |

("never delivered" = the engine captured nothing; for the mix this is correct when the query does not read the
table or the column.) Before the fix 4 of 6 accelerators returned wrong answers after SQL writes. 0 rebuilds,
0 fallbacks throughout. Not measured: SEC-EDGAR bulk loads, parallel (`_aux_parallel`) variants.

## Slot gates and tests

Not run on the final tree: the full ctest and the slot gates (the full ctest was interrupted; before the filter was
dropped, only the Q3/Q6/Q10 aux smokes failed because of it). Run on the merged tree. The 22-query differential was
run on the final tree (result in the commit message). An earlier `hits_under_writes` gate run (before the module
fixes, Q1/Q6/Q14, 20 s) saw 13 builds in 220 commits.

## Postgres and MySQL (not run)

- Shared runtime causes: stale-reason stats apply to both. `DataScaleRows` is wired for Postgres (`reltuples`,
  estimate; -1/0 before ANALYZE = unknown); MySQL still reports 0 (no statistic is wired; the cap stays
  unscaled there).
- Cost models: Postgres models are sane (ApplyDelta cheaper than RebuildAll at 7500 rows for all 22). MySQL
  models are not: ApplyDelta is priced at 47-240 s for a 7500-row refresh against 0.1-0.8 s for a rebuild, so
  MySQL accelerators will rebuild every refresh until recalibrated (not measured on MySQL).
- Module causes in `accelerators/tpch/q*/` common code (Q6 domain, Q1 shipdate) are shared only where the
  module is shared; the DuckDB modules fixed here are DuckDB-specific.
