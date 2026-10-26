# Substrate primitives (what a module may use)

Use these before writing a private equivalent. Thread rules: `engine_interfaces.md`. Scale rules: `scale_factor_contract.md`. Namespace `accelerator_codegen::aux_state::core`.

## Services (`AuxEngineServices`, `substrate/aux/core/aux_types.hpp`)

| Call | Purpose |
|---|---|
| `WorkerCount()` | Thread cap of the current phase (read in `Execute`, build in `Build`, maintenance in `AccumulateMutation` / `StageMaintenance`); 1 = serial code path. Size every split from it. |
| `ParallelFor(n, fn)` | Fork-join `fn(i)`, `i < n`, on the backend pool (caller participates, first exception rethrown); serial outside a phase. Prefer the slice helpers. |
| `ChooseMaintenance(MaintenanceRequest)` | The only way to pick a maintenance action. Fill `rows_touched`, `affected_partition_count`, `incremental_supported`, `partition_rebuild_supported`, `whole_state_required`; `data_rows_hint` defaults to `DataScaleRows()`. |
| `DataScaleRows()` | Rows now in the dependency tables (0 = unknown); scales the ApplyDelta cap and rebuild costs against `calibrated_scale_rows`. Use it to size anything by data volume. |
| `StateStore()` / `CurrentVisibility()` | State reads at the reader's epoch: `CheckValidity`, `ReadPartitions`, `ReadHitPartitions` (validity + images under one observation; use in `Execute`). Call from the calling thread, never from a slice. |
| `MaintenanceCostModel()`, `ExpectedQueriesBeforeNextWrite()` | Resolved `CostModelParams` and reads-per-write hint, for interference terms. |

## Parallel helpers

| Header | Primitive | Use for |
|---|---|---|
| `core/parallel_slices.hpp` | `ParallelReduce(services, n, grain, acc, body, merge)` | Data-proportional loop with a mergeable result (slice 0 writes `acc`, others fresh partials merged in slice order: serial-equal for an associative merge). |
| | `ParallelSlices`, `ParallelEach`, `SliceCount`, `RunSlices` | Independent items / disjoint ranges; `parallel_for` hooks of COW transactions. A range under one grain, or a serial run, is one inline slice: no "small input" branch. |
| | `ParallelEachWhen(services, n, total_work, work_threshold, fn)` | A few heavy items (shards, groups): parallel only when total work reaches `work_threshold`, else inline. |
| | `RunStarts(items, key)` | Offsets of runs of equal keys in a sorted vector, with an end sentinel; one task per run via `ParallelEach`. |
| `core/partitioned_build.hpp` | `PartitionedBuild<Partial>(services, make, scan, consume, merge)`, `kFloatBuildPartitions` | A `Build` consuming a table scan: one `Partial` per scan partition, merged in partition order. Floating-point partials use `kFloatBuildPartitions`. |
| | `SingleRowBatch<Row>{row}`, `BuildPartitions(services)`, `kBuildBuckets`, `Buckets` + `AppendBuckets` | Adapt a per-row scan callback to `PartitionedBuild` (`sink(p, SingleRowBatch<Row>{row})`); 4x-workers partitions to absorb skew (`PartitionedBuild`'s own default is `WorkerCount()`); row-bucket partials that merge by appending. |
| `core/parallel_sort.hpp` | `ParallelStableSort(services, vec, less, grain)` | Large build-time sorts on the build cap; same result at any thread count. |
| `core/aux_cells.hpp` | `IntCell`, `Int32Cell`, `StringCell`, `CharCell`, `DateCell`, `DoubleCell`, `DecimalCell`, `DecimalCellAtScale`, `ExactDecimalCell`, `WideDecimalCell`, `ScaleToRaw` | Typed reads of one `AuxRow` cell in `InterpretMutation`; never re-define. |
| | `CellState {Ok, Null, Bad}` with `NullableStringCell`, `NullableIntCell` (int64 or fitting uint64), `NullableDateCell`, `NullableIntOrDateCell` (int64 or date days), `NullableInt32OrDateCell`, `NullableDecimalCell` | Tri-state reads: `Null` = ignore the row, `Bad` (unexpected payload) = rebuild (e.g. `buffer.MarkRebuild()`). Pick the reader matching the capture layout. |
| `core/query_module_helpers.hpp` | `MutationRowCount`, `NextPowerOf2(n)`, `MatchSnapshot(requested, found, out)` | `MatchSnapshot`: the image of `found` for each requested partition (nullptr if absent) after `ReadPartitions`, whatever order the store returns. |

`ScanPartitioned`, its column specs and row view come from each backend (the **scan** role; names and guarantees in `engine_interfaces.md`). `Decimal(i)` is the 64-bit unscaled cell: accumulate it in 128 bits.

## Value and state types (`aux_types.hpp`)

- `ScaledDecimal128` / `AuxType::ScaledDecimal128` (DECIMAL(38, scale), `__int128`), `ScaledDecimal128ToString`; `ScaledDecimal` is 64-bit. Use the 128-bit type for every emitted or stored sum, accumulated with overflow checks. Engine-exact numeric text, division, civil-date arithmetic and LIKE/collation come from the backend API ("value semantics" in its `api/README.md`): no local date code or epoch constants in a module. `AuxDate` is days since 1970.
- `PartitionId` (`uint64_t`), `PartitionKey` (`int64_t`), `Version`, `kNewestVersion`: 64-bit, never narrowed.
- `StateImage::Make<T>(accelerator, partition, version, shared_ptr<const T>)` / `As<T>()`: typed immutable partition payload. `StateBuilder::Write` + `Finish` in `Build` (serial, calling thread).
- `MutationBatch` (rows as `AuxValue`), `BasicAuxTxnBuffer` (`core/aux_txn_buffer.hpp`) as the commit-buffer base; `ToMutationBatch` / `kOutsideProjection` (`runtime/columnar_batch.hpp`) define what a module sees of an update.

## Maintenance outcomes and policy

- Outcomes (`aux_types.hpp`): `StagedMaintenanceOutcome`, `FinishRebuildPartition`, `FinishRebuildAll(outcome)` (or `(outcome, decision)` after `ChooseMaintenance`: keeps an `InvalidateAndRebuildOnRead` decision, else `RebuildAll`), `AllPartitionsPlan`, `PartitionSetPlanFromImages`; a partition rebuild publishes only the touched images. `InvalidationPlan` (`state/invalidation_plan.hpp`): `None` / `PartitionSet` / `AllPartitions`.
- `MaintenanceRequest` factories: `RebuildOnly(queries)`, `IncrementalSingleState(rows, queries)`, `IncrementalPartitioned(rows, partitions, queries)`, `IncrementalPartitionedNoPartitionRebuild(rows, partitions, queries)` (partitioned deltas that cannot be redone per partition). Also `SelectAuxMaintenanceAction`, `CostModelParams` (`core/aux_maintenance_policy.hpp`, `maintenance/cost_model.hpp`); `ACCEL_FORCE_MAINTENANCE_ACTION` forces an action in tests. `apply_delta_max_rows` and rebuild costs scale with `DataScaleRows() / calibrated_scale_rows`.
- Diagnostics: `MaintenanceDecisionLogEntry` (`core/aux_maintenance_decision_log.hpp`; predicted vs actual cost per commit), `ReadAuxExecuteCounts` (`core/aux_execute_stats.hpp`; hit / rebuilt-on-read / fallback counts). A silent fallback or surprise rebuild is a finding.

## Naming

- Grain: `k<Unit>Grain` (singular unit: `kDeltaGrain`), in the module's top constant block, commented "<Unit> per slice (~N us)" or "(untimed)"; never invent a number. Work threshold: `k<Unit>OpThreshold`, commented "<Unit> below (or above) which <what> runs inline (~N us)". Decimal literals (`4096`, not `1 << 12`); delta-folding loops use 4096 unless a measured reason sits beside the constant.
- Scans: `Scan<Table>(services, partitions, sink)` is partitioned (`sink(partition, row)`, merged in order); `Scan<Table>Serial(consumer)` runs on the calling thread (small tables). Point lookups and SQL pushdowns are named for what they read (`ScanEventsByAccount`), never `Serial`. Delete scan methods nothing calls.

## Containers and calibration

- Containers: all of `accelerators/prims/` (catalogue `accelerators/prims/README.md`, may be empty; additions via `primitive_publish`, `cow_primitives.md`). Pick by key space (dense vs sparse, negative, far) and value size, never by SF1 shape.
- Cost-model JSON `<stem>[_<engine>][_<scale>].json` in the `AuxCostModelLocation` directory; `LoadAuxCostModelJson`, `ResolveAuxCostModelPath` (a missing `sf3000` file falls back to the nearest smaller scale). Record `calibrated_scale_rows` in every model.
- `AuxQueryModule::CalibrationWrites()` / `CalibrationWritesRegistrar` (`core/aux_calibration.hpp`): writes of increasing size spread over the partitions, each with revert and control; a benchmark's `common/calibration_shapes.hpp` (where present) builds them from per-table range shapes. Calibrate on refresh-shaped writes (bulk, spread keys), not tiny updates. `CalibrateFromWrites` fits ApplyDelta = overhead + per row, RebuildPartition = base + per partition, RebuildAll = mean.
