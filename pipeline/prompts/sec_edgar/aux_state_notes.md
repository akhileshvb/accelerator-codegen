# SEC EDGAR aux-state notes

Use these conventions when `benchmark_name: sec_edgar` and `workload_kind: aux_state`.

## Module layout (SEC EDGAR)

| Role | Path |
|------|------|
| Public aux API | `accelerators/sec_edgar/q{N}/<engine>/aux/module.hpp` |
| Aux implementation | `accelerators/sec_edgar/q{N}/<engine>/aux/module.cpp` |
| Finalize payload | `accelerators/sec_edgar/q{N}/<engine>/aux/finalize.cpp` |
| Base kernel API | `accelerators/sec_edgar/q{N}/<engine>/base/query.hpp` |
| Kernel / package | `accelerators/sec_edgar/q{N}/<engine>/base/single-threaded/` |
| Cost model JSON | `accelerators/sec_edgar/q{N}/config/sec_edgar_q{N}_aux_cost_model.json` |
| Cost-model location | `accelerators/sec_edgar/q{N}/cost_model.hpp` (`kSecEdgarQ{N}CostModel`; passed to the catalog entry / Runtime) |
| Tests | `accelerators/sec_edgar/q{N}/tests/test_sec_edgar_q{N}_aux.cpp`, `test_sec_edgar_q{N}_*.cpp` |
| Write-freq benchmark | `accelerators/sec_edgar/q{N}/bench/benchmark_q{N}_aux.cpp` |
| Calibration | `accelerators/sec_edgar/q{N}/bench/calibrate_q{N}_aux_cost_model.cpp` |

CMake:

```cmake
accel_add_aux_state_query(BENCHMARK sec_edgar QUERY_NUM N ...)
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

- Base fallback: fused scan via the package's `fused_scan` hook / `RunSecEdgarQ{N}FusedPreScan`
- Aux: `AuxQueryModule::CheckValidity` + `Execute` / `ExecuteFallback` via `accel_sec_edgar_q{N}_aux`
- Correctness: **full ordered multi-row** result match (not scalar)

## Cost model

- Calibrate with `calibrate_sec_edgar_q{N}_aux_cost_model` against SF3
- Initial JSON at `accelerators/sec_edgar/q{N}/config/sec_edgar_q{N}_aux_cost_model.json`
- `accelerators/sec_edgar/q{N}/cost_model.hpp` names that directory; pass `kSecEdgarQ{N}CostModel` as the catalog entry's `cost_model` (DuckDB) and to the Postgres `Runtime`
- Set `interference_us` using calibrated `CostModelParams.interference_*` fields in `StageMaintenance`
