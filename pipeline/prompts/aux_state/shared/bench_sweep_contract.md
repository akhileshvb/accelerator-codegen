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
3. Do **not** hardcode a non-catalog SELECT of arbitrary rows, or bump columns this query ignores (e.g. a column of another table that the query ignores). Use the shared invalidating-probe helper for any local precompute of write probes.
4. Multi-connection “invalidating writer” SQL in helpers is a separate stress path; the **bench write pool** is the catalog probe path above.

## Sweep protocol

The `aux_bench_sweep` gate calls the bench for each suite (the workload's `aux_bench_sweep.suites`): once with all the points of all the repeats when the host says `bench_multi_point` (one build, each point starting from the same data), else once per point. The write frequencies are drawn on every run from the query's frozen `write_mix.yaml` (0% always, then its sampled range; seed recorded in the gate result) and the sweep repeats `test_suite.bench_repeats` times with fresh draws: the points are in `ACCEL_AUX_WRITE_FREQUENCIES` (a `bench_multi_point` bench gets no percent argument and runs every point of the list; any other bench gets one point, as its percent argument), and the score is the median over the repeats with [min-max]. Without a suite, `ACCEL_AUX_WRITE_FREQUENCIES` (0, 1, 10, 50) is the fixed list.

### Env vars (read in benchmark)

| Variable | Purpose |
|----------|---------|
| the dataset env var of the host contract (`dataset_env`) | Path or connection string of the iteration dataset copy for this point |
| `ACCEL_Q{N}_BENCH_SUITE` | a suite name from `aux_bench_sweep.suites` |
| `ACCEL_Q{N}_WRITE_LOCALITY` | a locality name of the query's write mix |
| `ACCEL_BENCH_WARMUP` | Warmup queries before measured loop (engine reference SQL and aux) |
| `ACCEL_BENCH_QUERIES` | Measured queries per point (used for all timed paths) |
| `ACCEL_BENCH_REF_SAMPLES` | Legacy; ignored when benchmarks use warmup + measured loop |
| `ACCEL_BENCH_BASE_WRITE_SAMPLES` | Base-path write calibration samples |
| `ACCEL_Q{N}_MAINTENANCE_MODE` | Set `hybrid` during measured workload |
| `ACCEL_PROFILE_CONTROL` | Comma-separated `perf --control` fifos set by `run_profile` (clause `bench-profile-window`) |

Query-specific bench params (the template's own parameters) follow the Q{N} manifest / test helpers.

## Required stdout (CSV)

Print **one header row** (first column `suite`) and **one data row per entry of `ACCEL_AUX_WRITE_FREQUENCIES`** (see
the sweep protocol for how the points reach the bench). The columns, by name:

`suite`, `write_frequency_pct`, `workload_speedup`, `cold_start_workload_speedup`, `query_only_speedup`,
`blended_query_speedup`, `cold_start_accel_workload_us`, `setup_full_build_ms`,
`accel_hit_rate`, `writes_executed`, `duckdb_workload_us` (the engine reference's workload time), `accel_workload_us`,
`num_queries`, `accel_fallback_reads`, `state_builds`, `engine_ref_query_median_us`, `accel_hit_median_us`.

`query_only_speedup` is the read axis: the engine's own SQL for the same binding over the accelerator hit
(`engine_ref_query_median_us / accel_hit_median_us`). Both are wall-clock medians of fully consumed results, taken on
connections with the same settings and the same thread budget, in interleaved pairs so load affects both alike. There is
no stateless kernel to compare against: never time or register one. Recommended: `fallback_median_us` (median read
latency on the fallback). Optional `speedup_gate ...` and `#` lines may follow; the sweep parser ignores lines that are
not the CSV.

The measurement columns come from the runtime's own statistics of the whole run, never from an assumption; they are how
the orchestrator tells a bench that timed your accelerator from one that timed the fallback ("Trusting a benchmark
number" in the optimization prompt):

- `accel_hit_rate` = reads served from the state / all measured reads (a real ratio in [0, 1], not a constant);
- `num_queries` = measured reads;
- `accel_fallback_reads` = measured reads that ran the fallback SQL instead of the state;
- `state_builds` = states built during the whole run, warmup included (the first read builds one).

## Conformance clauses

The harness conformance check (`harness_conformance`, the gate host_bootstrap passes before it finishes) runs the bench
on a registered query with `ACCEL_AUX_WRITE_FREQUENCIES=0,0` (read-only points, so no write is needed), once with every
read forced to the fallback SQL (`ACCEL_AUX_FORCE_FALLBACK=1`, which the substrate honours) and once with reads hitting.
A failure names its clause:

- **`bench-run`**: the bench exits 0 on a read-only run, beside the other harnesses on the same instance.
- **`bench-rows`**: one header and exactly one row per entry of `ACCEL_AUX_WRITE_FREQUENCIES`.
- **`bench-columns`**: every one of `suite`, `write_frequency_pct`, `workload_speedup`, `query_only_speedup`,
  `accel_hit_rate`, `num_queries`, `accel_fallback_reads`, `state_builds`, `engine_ref_query_median_us`,
  `accel_hit_median_us` is in the row.
- **`bench-state-builds`**: a run whose reads hit a state reports `state_builds >= 1`.
- **`bench-hit-rate`**: that run's `accel_hit_rate` exceeds the forced run's by at least 0.4; a bench that reports the
  same rate both ways assumes it. (With only the declining module registered nothing hits and the two are not compared.)
- **`bench-profile-window`**: the bench wraps its measured read loop in `driver::ProfileWindow`
  (`eval/bench/driver/profile_window.hpp`; `run_profile` sets `ACCEL_PROFILE_CONTROL` and samples only inside it).

## Measured workload

1. Register accelerators, run `accel_q{N}_aux_build()` once.
2. Warmup loop: `ACCEL_BENCH_WARMUP` untimed runs for the engine's reference SQL and the aux query.
3. Measured loop: `ACCEL_BENCH_QUERIES` timed aux iterations with optional writes. The engine reference uses the same warmup + measured counts on a separate connection.
4. Write scheduling: 1% → every 100th query; 10% → every 10th; 50% → every 2nd.

**Performance:** precompute a small pool of **invalidating** write targets at startup from the catalog probes (do not run expensive correlated subqueries per write). Keep write SQL cheap so the sweep finishes in minutes, not hours.

## Composite score (optimization)

Parsed by `pipeline/orchestrator/accel_pipeline/optimization/bench.py`:

- `composite_workload_speedup` = **min** steady-state workload speedup across 0/1/10/50%
- `read_only_query_speedup` = query-only speedup at 0% (engine reference SQL vs aux hit)
- `read_only_cold_start_workload_speedup` = workload speedup at 0% including one-time `aux_build()` cost
- `composite_score` = min workload speedup + 0.1 × read-only query speedup

Higher is better. Optimization stops after 2 iterations without improving `composite_score`.

## Verification

`request_gate aux_bench_sweep` (MCP) runs the sweep for this query and reports one row per point.
