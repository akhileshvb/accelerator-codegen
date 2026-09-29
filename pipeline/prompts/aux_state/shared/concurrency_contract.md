# Concurrency contract (multi-connection aux state)

Aux accelerators run in a **multi-connection** environment: one connection may execute invalidating SQL while others read via aux table functions. The hand-written substrate provides txn isolation, per-accelerator epochs, and staged publication; generated `AuxQueryModule` code must cooperate.

Read `substrate_contract.md` and `maintenance_framework.md` for substrate APIs. This doc states **agent obligations**.

## Scope

- **Per-query module** (`{{aux_function_name}}`) registered on every shared catalog / allowlist / TF surface that peer queries use (see `registration_checklist.md`).
- **No cross-query state sharing** — but unrelated accelerators may be active concurrently.
- **Single-threaded per connection** (`PRAGMA threads=1` in tests/benchmarks).

## Relevance and irrelevant mutations

1. **`InterpretMutation`** must classify each committed mutation batch as relevant or irrelevant to this accelerator's watched columns/partitions.
2. **Irrelevant mutations** must not mark partitions stale, enqueue maintenance with side effects, or publish no-op epoch bumps.
3. Return an empty `InvalidationPlan` or `MaintenanceAction::NoOp` when no partition is affected.
4. Txn buffers must not treat irrelevant touches as pending work.

## Maintenance under concurrent writes

1. Use **lazy rebuild** (`RebuildPartition` / `RebuildAll`) via `StageMaintenance` — do not eagerly invalidate on every write unless required.
2. When rebuild is selected, **resync/rebuild** partition payloads for later aux hits — do not only mark stale.
3. **PRE_COMMIT invalidate**: when Affects_Q, Invalidate readable aux in
   PRE_COMMIT **before** PostgreSQL heap visibility; Commit TryPublish of
   prepared state (stale CAS must fail after version bump).
4. **No-op publish skip**: when maintenance has no publish effect, do not advance versions or publish staged updates.
5. **Optimistic publish**: partition updates must pass `TryPublish` version checks on `AuxStateStore`.
6. Prefer **per-accelerator** invalidation scope; never use a global epoch unrelated writers can advance.

## Read path under concurrent writes

1. **LocalWriteSafe**: if `StateStore().IsTransactionDirty()`, Execute must
   fallback (same-transaction relevant writes are not in published aux).
2. **TokenForSnapshot**: pin `CurrentVisibility()` to the exact active PG
   snapshot for the statement (not one token per transaction under READ
   COMMITTED).
3. **ReadHit**: pin that token once; `CheckValidity` + `ReadPartitions` must
   linearize (immutable handles from the checked cut). Do not
   CheckValidity then later reload mutable store with a different token.
4. **ConsistentObservation**: required partitions must all represent the same
   logical `DB_t` or fallback — do not use equal generation counters as the
   semantic criterion.
5. Serve aux when validity passes and txn rules allow; fall back otherwise.
6. **Bounded iteration**: read only partitions overlapping the query window.
7. **`ExecuteFallback`** must remain correct when state is stale, missing,
   dirty, txn-blocked, or under repair.

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
| `aux_cost_model_calibrate` | Calibrate `config/q{N}_aux_cost_model.json` including `interference_*` |
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