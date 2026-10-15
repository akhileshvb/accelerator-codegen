# Engine-local base access (native scan, slots S5/S8)

There is **no** shared portable scan IR. Each engine owns typed scans under
`backends/{engine}/engine/`. Do not invent a cross-engine chunk type or copy
another engine's scan.

The scan serves the accelerator **build** (`EngineSlots::RunBuild`, inside the
build snapshot) and the layer harnesses. It is not a portable
`ScanNativeBaseTable` that emits a shared chunk type.

## Requirements

1. Resolve the table (`catalog/schema/table` or the engine's equivalent) from the scan spec.
2. Project the requested columns (order stable).
3. Apply filters when the native API supports pushdown; otherwise residual-filter into the engine's native row/slot/batch type.
4. Use the engine **native storage / handler / table-AM API** (or, for a server engine, native connections each scanning one range of the table) — not a single client SQL round-trip on the hot path.
5. **Partition** the table into disjoint parts scanned in parallel on the thread budget (`ThreadBudget::ParallelFor` / `services.ParallelFor`); chunk callbacks for different partitions may run concurrently.
6. Emit **engine-local** typed chunks / slots / batches so `Build` and the harnesses can value-check rows. 64-bit row counts and offsets.
7. Read **inside the build snapshot**: the scan sees exactly the commits with epoch <= `b`.

Name and signature come from workload SDK hints / analysis (e.g. slot iterators, typed chunk consumers). Document the symbol in `backend_analysis.json` -> `scan_plan`.

## Aux storage

Auxiliary state images are **not** stored in the engine: the substrate keeps them (`VersionedAuxStore`) in the process that hosts the runtime.
