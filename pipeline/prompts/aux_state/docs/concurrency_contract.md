# Concurrency contract (multi-connection aux state)

One connection may run invalidating SQL while others read through aux table functions. The substrate provides txn isolation, per-accelerator epochs and staged publication (`maintenance_framework.md`, `engine_interfaces.md`); generated modules must cooperate. The module is per query (`{{aux_function_name}}`), shares no state with other queries, and is registered on every shared surface peers use (`registration_checklist.md`). Threads: `Execute`, `Build` and maintenance split data-proportional work through `services` (`WorkerCount()`; rules in `engine_interfaces.md`).

## Writes

1. `InterpretMutation` classifies each committed batch as relevant or irrelevant to the watched columns/partitions. Irrelevant: empty `InvalidationPlan` or `NoOp`; no stale marks, no side-effect maintenance, no epoch bump, no pending work in txn buffers.
2. Prefer lazy rebuild (`RebuildPartition` / `RebuildAll`) via `StageMaintenance` over eager invalidation; a selected rebuild resyncs payloads.
3. Maintenance with no publish effect advances no version and publishes nothing.
4. Optimistic publish: partition updates carry `expected_version`; the versioned store rejects a mismatch and the partition is invalidated.
5. Invalidation scope is per accelerator; never a global epoch unrelated writers can advance.

## Reads

1. `StateStore().CheckValidity` at statement start with only the window's partitions.
2. Serve aux when validity passes and txn rules allow; otherwise fall back. `ExecuteFallback` stays correct when state is stale, missing, txn-blocked or under repair.
3. Read only partitions overlapping the query window, never all slots. An `Absent` partition outside the window is not an error; inside it, skip or fall back.
4. Residual read work scales with the query window, not total partition count.

## Invariants

- A write for accelerator A never corrupts B's reads.
- Each accelerator's validity advances only from its own relevant maintenance.
- No global stale gate: an unrelated accelerator's pending rebuild never rejects aux reads.

## Gates and analysis output

Gates: `aux_cross_accel` (cross-accelerator correctness and isolation), `aux_multi_connection` (multi-connection stress), `aux_cost_model_calibrate` (including `interference_*`), `aux_bench_sweep` (write-frequency sweep 0/1/10/50%, primary optimization signal).

Analysis records `correctness_facts.cross_accelerator_concurrency`: `irrelevant_mutation_handling`, `read_path_bounds`, `maintenance_on_irrelevant_writes` (typically none), `fallback_conditions_under_concurrent_writes`.

Local check: MCP `run_correctness` and `run_multi_connection`; `ACCEL_CROSS_ACCEL_TEST_QUERIES={{query_number}} ctest -R test_cross_accel --output-on-failure`; `bash scripts/run_aux_bench_sweep.sh {{query_number}}`.
