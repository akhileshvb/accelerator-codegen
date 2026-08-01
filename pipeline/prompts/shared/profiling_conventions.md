# Profiling conventions

## Production path

- `CreateQ{N}Kernel(params)` — `profile = nullptr`; no `getenv` in hot loops.

## Profiling path

- `CreateQ{N}ProfilingKernel(params, Q{N}ProfileCounters&)` — coarse phase counters only.
- Profile structs in the query header (`{{header_path}}` pattern); kernel branches on `if (profile)`.

### Phase counters (recommended)

| Counter | Meaning |
|---------|---------|
| `pre_scan_us` | Total fused prescan wall time (sub + num) |
| `sub_scan_us` | Sub / dimension build scan |
| `num_scan_us` | Num scan loop wall time (may include aggregation on fused path) |
| `num_aggregate_us` | **Per-row aggregation / consume only** (time inside ProcessNum*/chunk consume) |
| `pre_consume_us` | Chunk-path consume (when production uses ConsumeInput chunks) |
| `finalize_us` | Finalize / sort / output prep |

On **fused** kernels, always record `num_aggregate_us` separately so optimization can distinguish I/O from GROUP BY updates. Do not rely on `pre_consume_us` alone.

### Row counters (recommended)

| Counter | Meaning |
|---------|---------|
| `pre_rows` | Rows scanned on dominant table pass |
| `qualifying_rows` | Rows that updated aggregates |
| `distinct_inserts` | New aggregate **groups** first touched (not total updates) |
| `generic_fallback_rows` | Rows handled by UnifiedVectorFormat fallback |

Compare `pre_rows` to `qualifying_rows` — large gaps often mean scan amplification (filters or join order), not slow aggregation.

## Benchmark

- Default: timed reference SQL vs `{{function_name}}()` (or parameterized equivalent).
- `ACCEL_BENCH_PROFILE=1`: direct native scan + profiling kernel on the **same path as production** (fused scan for SEC-EDGAR and TPC-H Q1; chunk path only when production uses chunks).
- Profile CSV should include `num_aggregate_us` when the kernel defines it.

## Experiments

- Compile-time guards only (`-DACCEL_EXPERIMENTAL_Q{N}_*`).
- Production path must not depend on ambient env vars.
