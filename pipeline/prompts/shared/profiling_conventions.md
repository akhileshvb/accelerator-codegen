# Profiling conventions

## Kernels

- Production: `CreateQ{N}Kernel(params)` with `profile = nullptr`; no `getenv` in hot loops.
- Profiling: `CreateQ{N}ProfilingKernel(params, Q{N}ProfileCounters&)`, coarse phase counters only; the profile structs live in the query header, the kernel branches on `if (profile)`.
- Experiments: compile-time guards only (`-DACCEL_EXPERIMENTAL_Q{N}_*`); production never depends on ambient env vars.

Phase counters (recommended): `pre_scan_us` (fused prescan wall time, sub + num), `sub_scan_us` (sub/dimension build scan), `num_scan_us` (num scan loop, may include aggregation on the fused path), `num_aggregate_us` (per-row aggregation/consume only), `pre_consume_us` (chunk-path consume), `finalize_us` (finalize/sort/output prep). Fused kernels always record `num_aggregate_us` separately, to tell I/O from GROUP BY updates.

Row counters: `pre_rows` (rows scanned on the dominant pass), `qualifying_rows` (rows that updated aggregates), `distinct_inserts` (new groups first touched), `generic_fallback_rows` (rows on the UnifiedVectorFormat fallback). A large `pre_rows` / `qualifying_rows` gap means scan amplification (filters or join order), not slow aggregation.

## Benchmark

Default: timed reference SQL vs `{{function_name}}()` (or parameterized equivalent). `ACCEL_BENCH_PROFILE=1`: direct native scan + profiling kernel on the same path as production (fused scan when the host has one, chunk path only when production uses chunks); the profile CSV includes `num_aggregate_us` when defined.

## Aux-state custom profiling

During optimization you may add temporary instrumentation to editable aux modules:

- Gate on `getenv("ACCEL_AUX_CUSTOM_PROFILE")` (no-op when unset); emit one line per timed region: `ACCEL_CUSTOM_PROFILE {"phase":"sort","us":12.3,"emitted_rows":180,"groups":180}`.
- Required: `phase` + `us` (or any `*_us` key), plus work counts (`rows`, `emitted_rows`, `groups`, `items`, `bytes`, `candidates`, ...).
- Digests aggregate into `phase_breakdown` (mean/p50/n, per-phase counts), `work_counters_mean`, `dominant_phase` and `residual_vs_hit_us` vs `accel_hit_median_us`.
- After accept/restore, call `run_profile` again; digests marked `freshness.stale` must not drive hypotheses. A/B keep/regress uses `scoreboard.best_valid_score` only.

## `run_profile` on a stateful accelerator

Returns the hit path's real profile: self-time top functions and source lines folded into `own_code` (`accelerators/`, prims included), `substrate` and `engine_or_other`; hardware counters per query (IPC, cache and branch misses, from the difference of two runs so setup cancels); whether the hottest own function uses wide vector instructions; and the visited-state count the read path reports through `RecordPartitionsRead`. Start every optimization from it and name the top row you attack. A dominant function missing from `own_code` is engine or substrate time: look at what your read asks of it. `not_applicable` with a reason means the engine's hit path cannot be sampled from the bench process: use phase timers and counters.
