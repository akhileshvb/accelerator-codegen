# Aux-state write-frequency benchmark contract

The orchestrator scores aux-state work via the `aux_bench_sweep` gate.
**This is the primary optimization signal** — not a read-only paired timing stub.

## Required binary

- The engine's shared bench harness `benchmark_<engine>_<bench>_aux` (`backends/<engine>/bench/`, made by `host_bootstrap`); query runs write no bench targets.
- CLI: `benchmark_<engine>_<bench>_aux --query <N> <write_pct>`: one write-frequency point per run, the suite from `ACCEL_Q<N>_BENCH_SUITE`.

## Invalidating writes (required)

Write-frequency / write-interval points must exercise **query-specific maintenance**:

1. Catalog `probe_select_sql` + `build_probe` must produce updates that **change the engine's `base_sql` result** for this query (same bar as incremental differential).
2. The shared sweep builds the write pool from the catalog probes, keeping only invalidating ones — candidates whose `update_sql` does not change the answer are dropped; **zero survivors fails the bench**.
3. Do **not** hardcode a non-catalog SELECT of arbitrary rows, or bump columns this query ignores (e.g. `l_extendedprice` on Q4). Use the shared invalidating-probe helper for any local precompute of write probes.
4. Multi-connection “invalidating writer” SQL in helpers is a separate stress path; the **bench write pool** is the catalog probe path above.

## Sweep protocol

The `aux_bench_sweep` gate calls the bench once per point: write frequency **0%, 1%, 10%, 50%** (`ACCEL_AUX_WRITE_FREQUENCIES`), for each suite (`lineitem_only` by default), each run on a fresh DB copy.

### Env vars (read in benchmark)

| Variable | Purpose |
|----------|---------|
| `ACCEL_TPCH_DB` | Path or connection string of the SF1 dataset copy for this point |
| `ACCEL_Q{N}_BENCH_SUITE` | e.g. `lineitem_only` |
| `ACCEL_Q{N}_WRITE_LOCALITY` | e.g. `single_month` / `single_segment` |
| `ACCEL_BENCH_WARMUP` | Warmup queries before measured loop (engine reference SQL and aux) |
| `ACCEL_BENCH_QUERIES` | Measured queries per point (used for all timed paths) |
| `ACCEL_BENCH_REF_SAMPLES` | Legacy; ignored when benchmarks use warmup + measured loop |
| `ACCEL_BENCH_BASE_WRITE_SAMPLES` | Base-path write calibration samples |
| `ACCEL_Q{N}_MAINTENANCE_MODE` | Set `hybrid` during measured workload |

Query-specific bench params (month/day_offset, segment/cutoff_date, etc.) follow the Q{N} manifest / test helpers.

## Required stdout (CSV)

Print **one header row** and **one data row** to stdout, header first column `suite`. The columns, by name:

`suite`, `write_frequency_pct`, `workload_speedup`, `cold_start_workload_speedup`, `query_only_speedup`,
`blended_query_speedup`, `cold_start_accel_workload_us`, `setup_full_build_ms`,
`accel_hit_rate`, `writes_executed`, `duckdb_workload_us` (the engine reference's workload time), `accel_workload_us`.

Optional `speedup_gate ...` and `#` lines may follow; the sweep parser ignores lines that are not the CSV.

## Measured workload

1. Register accelerators, run `accel_q{N}_aux_build()` once.
2. Warmup loop: `ACCEL_BENCH_WARMUP` untimed runs for the engine's reference SQL and the aux query.
3. Measured loop: `ACCEL_BENCH_QUERIES` timed aux iterations with optional writes. The engine reference uses the same warmup + measured counts on a separate connection.
4. Write scheduling: 1% → every 100th query; 10% → every 10th; 50% → every 2nd.

**Performance:** precompute a small pool of **invalidating** write targets at startup from the catalog probes (do not run expensive correlated subqueries per write). Keep write SQL cheap so the sweep finishes in minutes, not hours.

## Composite score (optimization)

Parsed by `pipeline/orchestrator/accel_pipeline/aux_bench.py`:

- `composite_workload_speedup` = **min** steady-state workload speedup across 0/1/10/50%
- `read_only_query_speedup` = query-only speedup at 0% (engine reference SQL vs aux hit)
- `read_only_cold_start_workload_speedup` = workload speedup at 0% including one-time `aux_build()` cost
- `composite_score` = min workload speedup + 0.1 × read-only query speedup

Higher is better. Optimization stops after 2 iterations without improving `composite_score`.

## Verification

`request_gate aux_bench_sweep` (MCP) runs the sweep for this query and reports one row per point.
