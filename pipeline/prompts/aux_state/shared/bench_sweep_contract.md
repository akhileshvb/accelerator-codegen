# Aux-state write-frequency benchmark contract

The orchestrator scores aux-state work via `aux_bench_sweep` (`scripts/run_aux_bench_sweep.sh <query>`).
**This is the primary optimization signal** — not a read-only paired timing stub.

## Required binary

- Target: `benchmark_tpch_q{{query_number}}_aux` (from `accelerators/tpch/q{{query_number}}/bench/benchmark_q{{query_number}}_aux.cpp`)
- Declared in the main `CMakeLists.txt` / workload benchmark targets (see `accel_add_aux_state_query` in `cmake/AuxState.cmake`)

## Invalidating writes (required)

Write-frequency / write-interval points must exercise **query-specific maintenance**:

1. Catalog `probe_select_sql` + `build_probe` must produce updates that **change the engine's `base_sql` result** for this query (same bar as incremental differential).
2. The shared sweep builds the write pool from the catalog probes, keeping only invalidating ones — candidates whose `update_sql` does not change the answer are dropped; **zero survivors fails the bench**.
3. Do **not** hardcode a non-catalog SELECT of arbitrary rows, or bump columns this query ignores (e.g. `l_extendedprice` on Q4). Use the shared invalidating-probe helper for any local precompute of write probes.
4. Multi-connection “invalidating writer” SQL in helpers is a separate stress path; the **bench write pool** is the catalog probe path above.

## Sweep protocol

The sweep script runs **four** points: write frequency **0%, 1%, 10%, 50%** (`lineitem_only` suite by default).
Each point sets `ACCEL_Q{{query_number}}_WRITE_FREQUENCY_PCT` and uses a fresh DB copy.

### Env vars (read in benchmark)

| Variable | Purpose |
|----------|---------|
| `ACCEL_TPCH_DB` | Path or connection string of the SF1 dataset copy for this point |
| `ACCEL_Q{N}_WRITE_FREQUENCY_PCT` | 0, 1, 10, or 50 |
| `ACCEL_Q{N}_BENCH_SUITE` | e.g. `lineitem_only` |
| `ACCEL_Q{N}_WRITE_LOCALITY` | e.g. `single_month` / `single_segment` |
| `ACCEL_BENCH_WARMUP` | Warmup queries before measured loop (engine reference, base kernel, and aux) |
| `ACCEL_BENCH_QUERIES` | Measured queries per point (used for all timed paths) |
| `ACCEL_BENCH_REF_SAMPLES` | Legacy; ignored when benchmarks use warmup + measured loop |
| `ACCEL_BENCH_BASE_WRITE_SAMPLES` | Base-path write calibration samples |
| `ACCEL_Q{N}_MAINTENANCE_MODE` | Set `hybrid` during measured workload |

Query-specific bench params (month/day_offset, segment/cutoff_date, etc.) follow the Q{N} manifest / test helpers.

## Required stdout (CSV)

Print **one header row** and **one data row** to stdout. The row must include at least:

`write_frequency_pct`, `workload_speedup`, `cold_start_workload_speedup`, `query_only_speedup`,
`blended_query_speedup`, `cold_start_accel_workload_us`, `setup_full_build_ms`,
`accel_hit_rate`, `writes_executed`, `duckdb_workload_us`, `accel_workload_us`.

Copy the column layout from a peer's `bench/benchmark_q*_aux.cpp`.

Optional `speedup_gate ...` lines may follow the CSV row (for audit gates); the sweep parser ignores non-CSV lines.

## Measured workload

1. Register accelerators, run `accel_q{N}_aux_build()` once.
2. Warmup loop: `ACCEL_BENCH_WARMUP` untimed runs for the engine's reference SQL, base kernel, and aux query.
3. Measured loop: `ACCEL_BENCH_QUERIES` timed aux iterations with optional writes. The engine reference and base kernel use the same warmup + measured counts on separate connections.
4. Write scheduling: 1% → every 100th query; 10% → every 10th; 50% → every 2nd.

**Performance:** precompute a small pool of **invalidating** write targets at startup from the catalog probes (do not run expensive correlated subqueries per write). Keep write SQL cheap so the sweep finishes in minutes, not hours.

## Composite score (optimization)

Parsed by `pipeline/orchestrator/accel_pipeline/aux_bench.py`:

- `composite_workload_speedup` = **min** steady-state workload speedup across 0/1/10/50%
- `read_only_query_speedup` = query-only speedup at 0% (base kernel vs aux hit)
- `read_only_cold_start_workload_speedup` = workload speedup at 0% including one-time `aux_build()` cost
- `composite_score` = min workload speedup + 0.1 × read-only query speedup

Higher is better. Optimization stops after 2 iterations without improving `composite_score`.

## Verification

After implementing the benchmark:

```bash
cmake --build build-release --target benchmark_tpch_q3_aux
bash scripts/run_aux_bench_sweep.sh 3
```

Expect **4 data rows** in `build-release/q3_aux_bench_sf1/results.csv`. Gate `aux_bench_sweep` fails if fewer rows are parsed.
At startup the bench should log `invalidating_write_probes=N` with `N >= 1`.
