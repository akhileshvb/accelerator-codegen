# SEC-EDGAR fused scan (production path)

The timed benchmark runs `SELECT * FROM {{function_name}}()` through `query_runtime`, which uses a **fused `ScanFilteredConsumer` path** for SEC-EDGAR queries — not the chunk `CreateNativeChunkSource` loop.

## Required files

| File | Role |
|------|------|
| `{{kernel_path}}` | `CreateQ{N}Kernel`, profiling kernel, aggregation logic |
| `{{package_path}}` | `NativeScanSpec`, bind, table function registration |
| `src/generated/sec_edgar/sec_edgar_query_runtime.cpp` | Wire fused scan into `query_runtime` (extend `IsSecEdgarFusedQuery` / `ExecSecEdgarFusedPreScan`) |
| `{{fused_scan_path}}` (optional) | Fused scan implementation — auto-compiled when present (see `cmake/BaseQuery.cmake`) |
| `{{fused_header_path}}` (optional) | Fused kernel API (mirror `tpch_q1_fused.hpp`) |

Do **not** edit `CMakeLists.txt`. Optional `q{N}_fused_scan.cpp` is picked up automatically.

## Reference implementations

Read these before implementing:

- `src/generated/tpch/single-threaded/q1_fused_scan.cpp` — fused `ScanFilteredConsumer` + `SelectProjectedColumn` per column
- `include/accelerator_codegen/tpch_q1_fused.hpp` — fused kernel API
- `src/generated/sec_edgar/sec_edgar_query_runtime.cpp` — runtime hook pattern
- `benchmarks/tpch/single-threaded/benchmark_q1.cpp` — profile path matches production fused scan

## Fused scan rules

1. Use `RunNativeFilteredScanConsumer` with a lambda that calls `row_group.SelectProjectedColumn` **per column** — never `ScanProjectedColumns` on filtered column 0 (causes count mismatch / segfaults).
2. Push filters (`stmt IS NOT NULL`, etc.) into `NativeScanSpec` when the scan adapter supports them.
3. `ConsumeInput` on the production kernel should throw or no-op after fused scan — production goes through `ExecSecEdgarFusedPreScan`.
4. Size intern tables / bitmaps from bind-time row estimates (`PrimeQ{N}RowEstimate`, `GetNativeScanTableRowCount`).
5. Profile gate (`ACCEL_BENCH_PROFILE=1`) must use the **same fused path** as production, not chunk iteration.

## Scan adapter

You may edit `src/duckdb_scan_adapter.cpp` and `include/accelerator_codegen/duckdb_scan_adapter.hpp` to add filter pushdown or row-count helpers.
