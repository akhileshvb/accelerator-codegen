# Mutation coverage

For **every** `correctness_facts.dependent_tables` entry and **every** mutation kind (`insert`, `update`, `delete`), assign exactly one behavior:

**`mutation` must be exactly `insert`, `update`, or `delete`** (lowercase). Put column-specific cases in `notes`, not in `mutation`.

| Behavior | Meaning |
|----------|---------|
| `supported_delta` | Incremental update preserves correctness |
| `bounded_rebuild` | Rebuild affected partition(s) only |
| `full_rebuild` | Rebuild all partitions (rare; justify) |
| `invalidate_safely` | Mark stale; answer from SQL until repaired |

## Rules

- No table × mutation pair may be omitted.
- If you cannot assign one of the four behaviors, aux-state is not viable for this query.
- Implementation tests must cover each non-`supported_delta` behavior at least once.

## Irrelevant mutations (concurrent workload)

Mutations on watched tables that **do not affect this query's partitions or semantics** are not a fifth behavior — they must produce **no maintenance side effects**:

- empty maintenance plan or `MaintenanceAction::NoOp`;
- no partition invalidation, no epoch advance, no staged publish;
- `HasPendingChanges` must remain false for irrelevant touches.

Document the relevance predicate in `correctness_facts.cross_accelerator_concurrency` (see `concurrency_contract.md`).
