# Aux-state write-frequency benchmark contract

The orchestrator scores aux-state work via `aux_bench_sweep` (`scripts/run_aux_bench_sweep.sh <query>`).
**This is the primary optimization signal** — not a read-only paired timing stub.

## Required binary

- Target: `benchmark_tpch_q{{query_number}}_aux` (from `benchmarks/tpch/single-threaded/benchmark_q{{query_number}}_aux.cpp`)
- Declared in the main `CMakeLists.txt` / workload benchmark targets (see `accel_add_aux_state_query` in `cmake/AuxState.cmake`)

## Sweep protocol

The sweep script runs **four** points: write frequency **0%, 1%, 10%, 50%** (`lineitem_only` suite by default).
Each point sets `ACCEL_Q{{query_number}}_WRITE_FREQUENCY_PCT` and uses a fresh DB copy.

### Env vars (read in benchmark)

| Variable | Purpose |
|----------|---------|
| `ACCEL_TPCH_DB` | Path to SF1 DuckDB copy for this point |
| `ACCEL_Q{N}_WRITE_FREQUENCY_PCT` | 0, 1, 10, or 50 |
| `ACCEL_Q{N}_BENCH_SUITE` | e.g. `lineitem_only` |
| `ACCEL_Q{N}_WRITE_LOCALITY` | e.g. `single_month` / `single_segment` |
| `ACCEL_BENCH_WARMUP` | Warmup queries before measured loop (DuckDB reference, base kernel, and aux) |
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

Copy the column layout from `benchmarks/tpch/single-threaded/benchmark_q14_aux.cpp` or `benchmark_q1_aux.cpp`.

Optional `speedup_gate ...` lines may follow the CSV row (for audit gates); the sweep parser ignores non-CSV lines.

## Measured workload

1. Register accelerators, run `accel_q{N}_aux_build()` once.
2. Warmup loop: `ACCEL_BENCH_WARMUP` untimed runs for DuckDB reference SQL, base kernel, and aux query.
3. Measured loop: `ACCEL_BENCH_QUERIES` timed aux iterations with optional writes. DuckDB reference and base kernel use the same warmup + measured counts on separate connections.
4. Write scheduling: 1% → every 100th query; 10% → every 10th; 50% → every 2nd.

**Performance:** precompute a small pool of write targets at startup (do not run expensive correlated subqueries per write). Keep write SQL cheap so the sweep finishes in minutes, not hours.

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
