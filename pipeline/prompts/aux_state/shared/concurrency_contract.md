# Concurrency contract (multi-connection aux state)

Aux accelerators run in a **multi-connection** environment: one connection may execute invalidating SQL while others read via aux table functions. The hand-written substrate provides txn isolation, per-accelerator epochs, and staged publication; generated `AuxQueryModule` code must cooperate.

Read `substrate_contract.md` and `maintenance_framework.md` for substrate APIs. This doc states **agent obligations**.

## Scope

- **Per-query module** (`{{aux_function_name}}`) registered on every shared catalog / allowlist / TF surface that peer queries use (see `registration_checklist.md`).
- **No cross-query state sharing** — but unrelated accelerators may be active concurrently.
- **Threads**: `Execute`, `Build` and maintenance each split data-proportional work through `services` (`WorkerCount()` is the current phase's cap; rules in `engine_interfaces.md`, helpers in `substrate_primitives.md`).

## Relevance and irrelevant mutations

1. **`InterpretMutation`** must classify each committed mutation batch as relevant or irrelevant to this accelerator's watched columns/partitions.
2. **Irrelevant mutations** must not mark partitions stale, enqueue maintenance with side effects, or publish no-op epoch bumps.
3. Return an empty `InvalidationPlan` or `MaintenanceAction::NoOp` when no partition is affected.
4. Txn buffers must not treat irrelevant touches as pending work.

## Maintenance under concurrent writes

1. Use **lazy rebuild** (`RebuildPartition` / `RebuildAll`) via `StageMaintenance` — do not eagerly invalidate on every write unless required.
2. When rebuild is selected, **resync/rebuild** partition payloads for later aux hits — do not only mark stale.
3. **No-op publish skip**: when maintenance has no publish effect, do not advance versions or publish staged updates.
4. **Optimistic publish**: partition updates carry `expected_version`; the runtime publishes through the versioned store and a mismatch invalidates the partition.
5. Prefer **per-accelerator** invalidation scope; never use a global epoch unrelated writers can advance.

## Read path under concurrent writes

1. **`CheckValidity`** at statement start — use `StateStore().CheckValidity` with only window partitions.
2. Serve aux when validity passes and txn rules allow; fall back otherwise.
3. **Bounded iteration**: read only partitions overlapping the query window — not all slots.
4. **Absent partitions**: skip or fall back; do not treat `Absent` as a hard error when the window excludes that partition.
5. **`ExecuteFallback`** must remain correct when state is stale, missing, txn-blocked, or under repair.

## Cross-accelerator invariants (agent-generated code)

| Invariant | Requirement |
|-----------|-------------|
| Isolation | A write targeting accelerator A must not corrupt correctness of accelerator B's reads. |
| Epoch independence | Each accelerator's validity advances only from its own relevant maintenance. |
| No global stale gate | Do not reject all aux reads because an unrelated accelerator has pending rebuild. |
| Read-path cost | Residual read work must scale with query window, not total partition count. |

## Pipeline gates

| Gate | What it checks |
|------|----------------|
| `aux_cross_accel` | Cross-accelerator correctness + isolation tests |
| `aux_multi_connection` | Multi-connection OLAP stress |
| `aux_cost_model_calibrate` | Calibrate `accelerators/tpch/q{N}/config/q{N}_aux_cost_model.json` including `interference_*` |
| `aux_bench_sweep` | Write-frequency sweep (0/1/10/50%) — primary optimization signal |

## Analysis output

In `correctness_facts.cross_accelerator_concurrency`, document:

- `irrelevant_mutation_handling` — how irrelevant writes become NoOp.
- `read_path_bounds` — how iteration is bounded to the query window.
- `maintenance_on_irrelevant_writes` — expected maintenance action (typically none).
- `fallback_conditions_under_concurrent_writes` — when aux is skipped under cross-connection interference.

## Harness wiring (required for new queries)

`aux_cross_accel` / `aux_multi_connection` need this query present on **every** shared registration and helper surface that peer aux queries use (SQL helpers, invalidating writer cases, DuckDB TF registration, shared-query allowlists). Discover current files by searching a neighboring query id; append matching cases. Missing wiring fails with `unsupported …` / `Table Function … does not exist`.

## Local verification

```bash
# Prefer MCP during implementation:
#   run_correctness
#   run_multi_connection
ACCEL_CROSS_ACCEL_TEST_QUERIES={{query_number}} ctest -R test_cross_accel --output-on-failure
bash scripts/run_aux_bench_sweep.sh {{query_number}}
```