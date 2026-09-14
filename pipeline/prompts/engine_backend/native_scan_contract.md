# Engine-local base access (native scan)

There is **no** shared portable scan IR. Each engine owns a typed helper under
`runtime/{engine}/`. Do not invent a cross-engine chunk type or copy another
engine’s scan adapter.

`engine_backend` may ship an **engine-local** typed scan helper used for aux
rebuild / layer-harness seeding — not a portable `ScanNativeBaseTable` that
emits a shared chunk type.

## Requirements

1. Resolve `catalog/schema/table` (or this engine’s equivalent) from the helper’s scan/spec arguments.
2. Project the requested columns (order stable).
3. Apply filters when the native API supports pushdown; otherwise residual-filter into the engine’s native row/slot/batch type.
4. Use the engine **native storage / handler / table-AM API** — not a client SQL round-trip on the hot path.
5. Emit **engine-local** typed callbacks, slots, or batches so aux `Build` / rebuild and layer harnesses can value-check rows.

Name and signature come from workload SDK hints / analysis (e.g. slot iterators, typed row consumers). Document the symbol in `backend_analysis.json` → `native_scan_plan`.

## Aux storage

Auxiliary state images are **not** required to live in base-table storage. Prefer engine-local custom stores (typed heap, process-local memory, or other engine-native aux storage) behind `AuxStateStore`.

## Shared substrate (unchanged)

Mutation/txn/visibility still use shared `MutationBatch`, coordinator, and `AuxStateStore`. Only base-table streaming is engine-private.
