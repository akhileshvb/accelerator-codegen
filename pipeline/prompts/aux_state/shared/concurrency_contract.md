# Concurrency contract (multi-connection aux state)

Aux accelerators run in a **multi-connection** environment: one connection may execute invalidating SQL while others read via aux table functions. The hand-written substrate provides txn isolation, per-accelerator epochs, and global lazy rebuild; generated code must cooperate.

Read `substrate_contract.md` and `maintenance_framework.md` for substrate APIs. This doc states **agent obligations**.

## Scope

- **Per-query state** (`{{aux_function_name}}`) registered in `AcceleratorStateRegistry`.
- **No cross-query state sharing** — but unrelated accelerators may be active concurrently.
- **Single-threaded per connection** (`PRAGMA threads=1` in tests/benchmarks).

## Relevance and irrelevant mutations

1. **Mutation interpreter** must classify each committed mutation batch as relevant or irrelevant to this accelerator's watched columns/partitions.
2. **Irrelevant mutations** must not:
   - mark partitions stale,
   - enqueue maintenance with side effects,
   - bump `committed_mutation_epoch` via a no-op publish.
3. Return an empty maintenance plan or `MaintenanceAction::NoOp` when no partition is affected.
4. `HasPendingChanges` / incremental buffers must not treat irrelevant touches as pending work.

## Maintenance under concurrent writes

1. Use **lazy rebuild** (`RebuildPartition` / `RebuildAll`) — do not eagerly invalidate-and-publish on every write unless `invalidate_safely` is the declared behavior for that mutation.
2. **No-op publish skip**: when `MaintenanceOutcomeHasPublishEffect` is false, do not advance epochs or publish staged updates.
3. **Optimistic publish**: partition updates must pass `PublishValidatedPartitionUpdates` version checks.
4. Prefer **per-accelerator** epoch and pending-plan state; never use a global epoch that unrelated writers can advance.

## Read path under concurrent writes

1. **Statement-start epoch pin** via `ReaderMutationEpochState` — do not re-read naked epoch mid-scan.
2. **Execution gate** per partition: serve aux when partition is `Valid`, payload is readable, and txn bypass rules allow. `valid_through_mutation_epoch` is publish metadata; **do not** reject `Valid` partitions solely because `valid_through < reader_epoch` — unaffected partitions may lag epoch while remaining correct. Relevant mutations must `Invalidate` or republish before commit.
3. **Bounded iteration**: iterate only partitions overlapping the query window/range — not all slots in the partition store.
4. **Absent partitions**: skip or fall back; do not treat `Absent` as a hard error when the query window excludes that partition.
5. **Fallback** must remain correct when state is stale, missing, txn-blocked, or under repair.

## Cross-accelerator invariants (agent-generated code)

| Invariant | Requirement |
|-----------|-------------|
| Isolation | A write targeting accelerator A must not corrupt correctness of accelerator B's reads. |
| Epoch independence | Each accelerator's `committed_mutation_epoch` advances only from its own relevant maintenance. |
| No global stale gate | Do not reject all aux reads because an unrelated accelerator has pending rebuild. |
| Read-path cost | Residual read work must scale with query window, not total partition count. |

## Pipeline gates

| Gate | What it checks |
|------|----------------|
| `aux_cross_accel` | Cross-accelerator correctness + isolation tests for the current query (`test_cross_accel_correctness`, `test_cross_accel_isolation`). |
| `aux_multi_connection` | Multi-connection OLAP stress: concurrent target-query readers, background readers on other accelerators, and cross-query writers (`test_aux_multi_connection_stress`). |
| `aux_cost_model_calibrate` | Calibrate `config/q{N}_aux_cost_model.json` including non-zero `interference_*` coefficients from multi-connection measurements. |
| `aux_bench_sweep` | **Same-query** write-frequency sweep (0/1/10/50%) — primary optimization signal. |

Both cross-accel gates verify correctness under concurrent multi-accelerator load. The write-freq sweep measures end-to-end speed under mixed read/write on **this query**.

## Analysis output

In `correctness_facts.cross_accelerator_concurrency`, document:

- `irrelevant_mutation_handling` — how irrelevant writes become NoOp.
- `read_path_bounds` — how iteration is bounded to the query window.
- `maintenance_on_irrelevant_writes` — expected maintenance action (typically none).
- `fallback_conditions_under_concurrent_writes` — when aux is skipped under cross-connection interference.

## Local verification

```bash
ACCEL_CROSS_ACCEL_TEST_QUERIES={{query_number}} ctest -R test_cross_accel --output-on-failure
bash scripts/run_aux_bench_sweep.sh {{query_number}}
```
