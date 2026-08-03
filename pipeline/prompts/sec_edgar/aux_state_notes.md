# SEC EDGAR aux-state notes

Use these conventions when `benchmark_name: sec_edgar` and `workload_kind: aux_state`.

## Module layout (SEC EDGAR)

| Role | Path |
|------|------|
| Public aux API | `include/accelerator_codegen/sec_edgar_q{N}_aux.hpp` |
| Incremental | `include/.../sec_edgar_q{N}_incremental.hpp`, `src/aux_state/sec_edgar/q{N}/q{N}_incremental.cpp` |
| Finalize payload | `src/aux_state/sec_edgar/q{N}/sec_edgar_q{N}_finalize.cpp` |
| Kernel / package | `src/aux_state/sec_edgar/q{N}/q{N}_aux_kernel.cpp`, `q{N}_aux_package.cpp` |
| Cost model JSON | `config/sec_edgar_q{N}_aux_cost_model.json` |
| Tests | `tests/single-threaded/test_sec_edgar_q{N}_aux.cpp`, `test_sec_edgar_q{N}_*.cpp` |
| Write-freq benchmark | `benchmarks/sec_edgar/single-threaded/benchmark_q{N}_aux.cpp` |
| Calibration | `benchmarks/sec_edgar/single-threaded/calibrate_q{N}_aux_cost_model.cpp` |

CMake:

```cmake
accel_add_aux_state_query(BENCHMARK sec_edgar QUERY_NUM N ...)
accel_add_aux_state_benchmarks(BENCHMARK sec_edgar QUERY_NUM N ...)
accel_add_aux_state_ctests(BENCHMARK sec_edgar QUERY_NUM N)
```

## Tables and mutations

- **Tables**: `sub` (filings), `num` (numeric facts), `pre` (presentation), `tag` (dictionary)
- **Q1** watches `pre`; typical mutations are **append** of new presentation lines for a filing
- **Q2** watches `num` + `sub`; typical mutations are **append** of new numeric facts (pure uom) for fy=2022 filings
- **Partition keys** (analysis should propose): Q1 → `(stmt, rfile)`; Q2 → e.g. `(adsh, tag)` max-value groups or fy cohort
- **Irrelevant mutation → NoOp**: updates to tables/columns not in the query's filter/join closure

## Write benchmark suite: `filing_append` (Q1) / `value_append` (Q2+)

Unlike TPC-H `lineitem_only` point updates, SEC EDGAR sweeps use workload-specific append suites:

- **Q1 (`filing_append`)**: append synthetic `pre` rows between queries
- **Q2 (`value_append`)**: append synthetic `num` rows (pure uom, fy=2022 cohort) between queries
- `ACCEL_Q{N}_WRITE_FREQUENCY_PCT` controls how often a write txn runs
- Benchmark binary must emit the standard sweep CSV (`bench_sweep_contract.md`)

Env: `ACCEL_SEC_EDGAR_DB` (not `ACCEL_TPCH_DB`).

## Read path

- Base fallback: fused scan via `sec_edgar_query_runtime.cpp` / `RunSecEdgarQ{N}FusedPreScan`
- Aux table function: `accel_sec_edgar_q{N}_aux` with epoch pin, execution gate, `MaxThreads() = 1`
- Correctness: **full ordered multi-row** result match (not scalar)

## Cost model

- Calibrate with `calibrate_sec_edgar_q{N}_aux_cost_model` against SF3
- Initial JSON at `config/sec_edgar_q{N}_aux_cost_model.json`
- Use `EstimateInterferenceUs` with calibrated `interference_*` coefficients
