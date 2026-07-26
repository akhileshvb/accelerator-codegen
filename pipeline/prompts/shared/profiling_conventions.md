# Profiling conventions

## Production path

- `CreateQ{N}Kernel(params)` — `profile = nullptr`; no `getenv` in hot loops.

## Profiling path

- `CreateQ{N}ProfilingKernel(params, Q{N}ProfileCounters&)` — coarse phase counters only.
- Profile structs in `tpch_q{N}.hpp`; kernel branches on `if (profile)`.

## Benchmark

- Default: timed SQL `accel_tpch_q{N}(...)`.
- `ACCEL_BENCH_PROFILE=1`: direct scan + profiling kernel (see reference `benchmark_q3.cpp`).

## Experiments

- Compile-time guards only (`-DACCEL_EXPERIMENTAL_Q{N}_*`).
- Production path must not depend on ambient env vars.
