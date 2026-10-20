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
3. Apply filters when the native API supports pushdown; otherwise residual-filter into the engine's native row/slot/batch type. (The module-facing build scan below is tables only: modules filter in their `consume`.)
4. Use the engine **native storage / handler / table-AM API** (or, for a server engine, native connections each scanning one range of the table) — not a single client SQL round-trip on the hot path.
5. **Partition** the table into disjoint parts scanned in parallel on the thread budget (`ThreadBudget::ParallelFor` / `services.ParallelFor`); chunk callbacks for different partitions may run concurrently.
6. Emit **engine-local** typed chunks / slots / batches so `Build` and the harnesses can value-check rows. 64-bit row counts and offsets.
7. Read **inside the build snapshot**: the scan sees exactly the commits with epoch <= `b`.

Name and signature come from the engine profile (`scan`) / analysis (e.g. slot iterators, typed chunk consumers). Document the symbol in `backend_analysis.json` -> `scan_plan`.

## Module-facing build scan: `ScanPartitioned`

Modules build in parallel through `core::PartitionedBuild` (`substrate/aux/core/partitioned_build.hpp`) over this engine's `ScanPartitioned`; the module-side rules are in `shared/engine_interfaces.md` ("Parallel builds"). The backend implements, exported in a header for the queries' scan code (`backends/{engine}/engine/build_scan.hpp` or the engine's `api/` helpers), with exactly these names (modules call them):

- `ScanPartitioned(services, <engine handle>, table, columns, partitions, sink)`; `sink(partition, batch)`, where `batch.size()` and `batch.Row(i)` give a row view.
- Column specs `IntColumn`, `DateColumn`, `DecimalColumn(name, scale)`, `DoubleColumn`, `TextColumn`, `Nullable(column)`; row accessors `Int(i)` (int64), `Date(i)` (int32 days since 1970-01-01), `Decimal(i)` (value * 10^scale in int64, failing loudly on a value that does not fit), `Double(i)`, `TextView(i)` / `Text(i)` / `Char(i)`, `IsNull(i)`. A NULL in a column not declared `Nullable` fails loudly. Typed column batches; no boxed value per cell, no per-row allocation.
- Partition `p` is a fixed set of rows determined by the table and the partition count (row-group or heap-block ranges; the same data gives the same partitions), delivered in table order, one thread per partition at a time; different partitions run concurrently on `services.ParallelFor` threads, never more than `services.WorkerCount()` (the build cap) and never on the engine's own parallel machinery or a private pool.
- Every partition reads the same build snapshot (epoch `b`), whichever connection or worker serves it.
- `partitions == 1` is the engine's serial scan on the calling thread. Partition counts above the number of natural ranges leave the extra partitions empty.
- 64-bit row counts and offsets throughout.

An engine with no partitioned scan yet keeps a serial scan and says so in `scan_plan`; it must still expose the same names with `partitions` honoured as 1.

## Aux storage

Auxiliary state images are **not** stored in the engine: the substrate keeps them (`VersionedAuxStore`) in the process that hosts the runtime.
