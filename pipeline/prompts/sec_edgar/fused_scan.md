# SEC-EDGAR fused scan (production path)

The timed benchmark runs `SELECT * FROM {{function_name}}()` through `query_runtime`, which uses the package's **fused `ScanFilteredConsumer` path** (`QueryPackage::create_fused_kernel` / `fused_scan`) for SEC-EDGAR queries — not the chunk `CreateNativeChunkSource` loop.

## Required files

| File | Role |
|------|------|
| `{{kernel_path}}` | `CreateQ{N}Kernel`, profiling kernel, aggregation logic |
| `{{package_path}}` | `NativeScanSpec`, bind, table function; sets `package.create_fused_kernel` / `package.fused_scan` (the runtime calls them; no shared file to edit) |
| `{{fused_scan_path}}` (optional) | Fused scan implementation — auto-compiled when present (see `cmake/BaseQuery.cmake`) |
| `{{fused_header_path}}` (optional) | Fused kernel API (mirror `accelerators/sec_edgar/q1/duckdb/base/fused.hpp`) |

Do **not** edit `CMakeLists.txt`. Optional `fused_scan.cpp` under `base/single-threaded/` is picked up automatically.

## Reference implementations

Read these before implementing:

- `accelerators/sec_edgar/q2/duckdb/base/single-threaded/kernel.cpp` — fused `ScanFilteredConsumer` + `SelectProjectedColumn` per column
- `accelerators/sec_edgar/q1/duckdb/base/fused.hpp` — fused kernel API
- `accelerators/sec_edgar/q1/duckdb/base/single-threaded/package.cpp` — fused hook pattern (`create_fused_kernel` / `fused_scan`)
- `accelerators/tpch/q1/bench/benchmark_q1.cpp` — profile path matches production fused scan

## Fused scan rules

1. Use `RunNativeFilteredScanConsumer` with a lambda that calls `row_group.SelectProjectedColumn` **per column** — never `ScanProjectedColumns` on filtered column 0 (causes count mismatch / segfaults).
2. Push filters (`stmt IS NOT NULL`, etc.) into `NativeScanSpec` when the scan adapter supports them.
3. `ConsumeInput` on the production kernel should throw or no-op after fused scan — production goes through the package's `fused_scan`.
4. Size intern tables / bitmaps from bind-time row estimates (`PrimeQ{N}RowEstimate`, `GetNativeScanTableRowCount`).
5. Profile gate (`ACCEL_BENCH_PROFILE=1`) must use the **same fused path** as production, not chunk iteration.

## Scan adapter

You may edit `backends/duckdb/engine/duckdb_scan_adapter.cpp` and `backends/duckdb/engine/duckdb_scan_adapter.hpp` to add filter pushdown or row-count helpers.

Layout reference: `pipeline/prompts/shared/generated_layout.md`.
