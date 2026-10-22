# Substrate primitives (what a module may use)

Everything below exists; use it before writing a private equivalent. Rules for *how* to use the parallel helpers (caps, slice rules, determinism) are authoritative in `engine_interfaces.md` ("Module thread rules", "Parallel builds"); large-scale design rules in `scale_factor_contract.md`. Namespace `accelerator_codegen::aux_state::core` unless noted.

## Services (`AuxEngineServices`, `substrate/aux/core/aux_types.hpp`)

| Call | Purpose / when |
|---|---|
| `WorkerCount()` | Thread cap of the current phase (read cap in `Execute`, build cap in `Build`, maintenance cap in `AccumulateMutation` / `StageMaintenance`); 1 = write the serial code path. Size every split from it. |
| `ParallelFor(n, fn)` | Fork-join `fn(i)`, `i < n`, on the backend's pool (caller participates; first exception rethrown). Serial outside a phase. Prefer the slice helpers below. |
| `ChooseMaintenance(MaintenanceRequest)` | The only way to pick a maintenance action. Fill `rows_touched`, `affected_partition_count`, `incremental_supported`, `partition_rebuild_supported`, `whole_state_required`; `data_rows_hint` defaults to `DataScaleRows()`. Never hard-wire an action. |
| `DataScaleRows()` | Rows now in the dependency tables (0 = unknown); scales the ApplyDelta cap and rebuild costs against `calibrated_scale_rows` (`aux_maintenance_policy.cpp`). Read it if the module sizes anything by data volume. |
| `StateStore()` / `CurrentVisibility()` | State reads at the reader's epoch: `CheckValidity`, `ReadPartitions`, `ReadHitPartitions` (validity + images under one observation; use it in `Execute`). Call from the calling thread, never from a slice. |
| `MaintenanceCostModel()`, `ExpectedQueriesBeforeNextWrite()` | The resolved `CostModelParams` and the reads-per-write hint, for interference terms. |

## Parallel helpers

| Header | Primitive | Use for |
|---|---|---|
| `core/parallel_slices.hpp` | `ParallelReduce(services, n, grain, acc, body, merge)` | A data-proportional loop with a mergeable result (slice 0 writes `acc`, the others fresh partials merged in slice order: serial-equal for an associative merge). |
| | `ParallelSlices`, `ParallelEach`, `SliceCount`, `RunSlices` | Independent items / disjoint ranges; `parallel_for` hooks of COW transactions. A slice is >= `grain` items (>= ~200 us of work); a range under one grain (or a serial run) is one slice run inline, so no caller needs its own "small input" branch. |
| | `ParallelEachWhen(services, n, total_work, work_threshold, fn)` | A few heavy items (shards, nations): parallel only when the whole work reaches `work_threshold`, else inline. Name the constant `k<Unit>OpThreshold`. |
| | `RunStarts(items, key)` | Offsets of the runs of equal keys in a sorted vector, with an end sentinel; fan out one task per run with `ParallelEach`. |
| `core/partitioned_build.hpp` | `PartitionedBuild<Partial>(services, make, scan, consume, merge)`, `kFloatBuildPartitions` | A `Build` that consumes a table scan: one `Partial` per scan partition, merged in partition order. Floating-point partials use `kFloatBuildPartitions` partitions. |
| | `SingleRowBatch<Row>{row}`, `BuildPartitions(services)`, `kBuildBuckets`, `Buckets` + `AppendBuckets` | Adapt a per-row scan callback to `PartitionedBuild` (`sink(p, SingleRowBatch<Row>{row})`); 4x workers partitions to absorb skew (`PartitionedBuild`'s own default is `WorkerCount()`); row-bucket partials that merge by appending. |
| `core/parallel_sort.hpp` | `ParallelStableSort(services, vec, less, grain)` | Sorting a large build-time vector on the build cap; same result at any thread count. |
| `core/aux_cells.hpp` | `IntCell`, `Int32Cell`, `StringCell`, `CharCell`, `DateCell`, `DoubleCell`, `DecimalCell`, `DecimalCellAtScale`, `ExactDecimalCell`, `WideDecimalCell`, `ScaleToRaw` | Typed reads of one `AuxRow` cell in `InterpretMutation`; never re-define them in a module. |
| | `CellState {Ok, Null, Bad}` + `NullableStringCell`, `NullableIntCell` (int64 or fitting uint64), `NullableDateCell` (date payload only), `NullableIntOrDateCell` (int64 or date as days), `NullableInt32OrDateCell`, `NullableDecimalCell` | Tri-state reads when a NULL cell means "ignore this row" (`Null`) and an unexpected payload means "rebuild" (`Bad`, e.g. `buffer.MarkRebuild()`). Pick the reader whose accepted payloads match the capture layout; do not re-define them. |
| `core/query_module_helpers.hpp` | `MutationRowCount`, `NextPowerOf2(n)`, `MatchSnapshot(requested, found, out)` | `MatchSnapshot`: the image of `found` for each requested partition (nullptr if absent) after `ReadPartitions`, whatever order the store returns. |

`ScanPartitioned` and its typed column specs / row view (`IntColumn`, `DateColumn`, `DecimalColumn(name, scale)`, `DoubleColumn`, `TextColumn`, `Nullable`; `Int`, `Date`, `Decimal`, `Double`, `TextView`/`Text`/`Char`, `IsNull`) are supplied by each backend (the **scan** role of the Backend API index); guarantees in `engine_interfaces.md`. `Decimal(i)` is the 64-bit unscaled cell: accumulate it in a 128-bit type.

## Value and state types (`aux_types.hpp`)

- `ScaledDecimal128` / `AuxType::ScaledDecimal128` (DECIMAL(38, scale), `__int128`), `ScaledDecimal128ToString`; `ScaledDecimal` is 64-bit. Use the 128-bit type for any emitted or stored sum, accumulated with overflow checks. Engine-exact numeric text and division come from the backend API (role "value semantics" in its `api/README.md` index), not from `prims/`.
- `AuxDate` (days since 1970); civil-date arithmetic and LIKE/collation come from the backend API ("value semantics"): no local civil-date code or epoch constants in a module.
- `PartitionId` (`uint64_t`) / `PartitionKey` (`int64_t`), `Version`, `kNewestVersion`: 64-bit identities; never narrow them.
- `StateImage::Make<T>(accelerator, partition, version, shared_ptr<const T>)` / `As<T>()`: the typed immutable payload of a partition; `StateBuilder::Write` + `Finish` in `Build` (serial, calling thread).
- `MutationBatch` (rows as `AuxValue`), `MutationRowCount` (`core/query_module_helpers.hpp`), `BasicAuxTxnBuffer` (`core/aux_txn_buffer.hpp`) as the base of a commit buffer; `ToMutationBatch` / `kOutsideProjection` (`runtime/columnar_batch.hpp`) define what a module sees of an update.

## Maintenance outcomes and policy

- `StagedMaintenanceOutcome`, `FinishRebuildPartition`, `FinishRebuildAll(outcome)` (or `FinishRebuildAll(outcome, decision)` after `ChooseMaintenance`: keeps an InvalidateAndRebuildOnRead decision, else RebuildAll), `AllPartitionsPlan`, `PartitionSetPlanFromImages` (`aux_types.hpp`): build the outcome of each action; a partition rebuild publishes only the touched images.
- `InvalidationPlan` (`state/invalidation_plan.hpp`): `None` / `PartitionSet` / `AllPartitions`.
- `MaintenanceRequest` with the factories `RebuildOnly(queries)`, `IncrementalSingleState(rows, queries)`, `IncrementalPartitioned(rows, partitions, queries)`, `IncrementalPartitionedNoPartitionRebuild(rows, partitions, queries)` (partitioned deltas that cannot be redone per partition) (no positional bool lists), `SelectAuxMaintenanceAction`, `CostModelParams` (`core/aux_maintenance_policy.hpp`, `maintenance/cost_model.hpp`); `ACCEL_FORCE_MAINTENANCE_ACTION` forces an action in tests. The ApplyDelta cap `apply_delta_max_rows` and the rebuild costs scale with `DataScaleRows() / calibrated_scale_rows`.
- `MaintenanceDecisionLogEntry` (`core/aux_maintenance_decision_log.hpp`): per-commit predicted vs actual cost; `ReadAuxExecuteCounts` (`core/aux_execute_stats.hpp`): hit / rebuilt-on-read / fallback counts. Check both in tests: a silent fallback or a surprise rebuild is a finding.

## Naming grains

A grain is `k<Unit>Grain` with a singular unit (`kDeltaGrain`, `kGroupGrain`, `kChunkGrain`), declared in the module's top constant block (never mid-file or function-local) with the comment "<Unit> per slice (~N us)"; a work threshold is `k<Unit>OpThreshold`. Write literals in decimal (`4096`, `16384`), not `1 << n`. A loop folding deltas per slice uses one value (4096) in every engine unless a measured reason is written next to the constant. Write "(untimed)" in place of "(~N us)" when the slice was not timed; never invent a number. A threshold comment reads "<Unit> below (or above) which <what> runs inline (~N us)".

## Naming scans

An `InitialScan` method that reads a whole table is named for the table: `Scan<Table>(services, partitions, sink)` is partitioned (`sink(partition, row)`, one thread per partition, partitions merged in order); `Scan<Table>Serial(consumer)` delivers rows one at a time on the calling thread (small dimension tables). Point lookups and SQL aggregate pushdowns are named for what they read (`ScanLineitemsByOrder`, `ScanMonthAggregates`), never `Serial`. A query with no `Scan<Table>` of its own scans through `PartitionedTableScan::ScanTable` and `CollectTable`. Delete a scan method nothing calls.

## Containers (copy-on-write)

All of `accelerators/prims/`: the catalogue is `accelerators/prims/README.md` (it may be empty); consult it before writing a helper, and publish reusable engine-neutral helpers with `primitive_publish` (additions only). Pick by key space (bounded and dense vs sparse, negative or far keys) and value size, never by SF1 shape: a structure for bounded non-negative ids must not be given sparse, negative or far keys.

## Calibration

- Cost-model JSON `<stem>[_<engine>][_<scale>].json` in the directory named by the query's `AuxCostModelLocation`; `LoadAuxCostModelJson`, `ResolveAuxCostModelPath` (a missing `sf3000` file falls back to the nearest smaller scale). Record `calibrated_scale_rows` in every model.
- `AuxQueryModule::CalibrationWrites()` / `CalibrationWritesRegistrar` (`core/aux_calibration.hpp`): writes of increasing size and spread over the partitions, each with revert and control. `accelerators/tpch/common/calibration_shapes.hpp` builds them from per-table range shapes. Calibrate on refresh-shaped writes (bulk, keys spread), not tiny updates.
- `CalibrateFromWrites` fits ApplyDelta = overhead + per row, RebuildPartition = base + per partition, RebuildAll = mean (`maintenance_framework.md` for how the policy uses them).
