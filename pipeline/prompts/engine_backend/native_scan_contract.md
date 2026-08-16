# Native scan contract

Every engine adapter implements the shared streaming contract from `runtime/native_scan.hpp`:

```
ScanNativeBaseTable(spec, consumer) -> repeatedly invoke consumer with DataChunks
```

## Requirements

1. Resolve `catalog/schema/table` from `NativeScanSpec`.
2. Project columns listed in `projected_columns` (order stable).
3. Apply filters when the native API supports pushdown; otherwise residual-filter into chunks.
4. Use the engine **native storage / table AM / handler API** — not a client SQL round-trip on the hot path.
5. Emit DuckDB `DataChunk` as the portable IR so generated kernels stay engine-neutral.

## Aux storage

Auxiliary state images are **not** required to live in base-table storage. Prefer engine-local custom stores (typed heap, SHM blobs, aux tables) behind `AuxStateStore`.
