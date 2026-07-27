# Mutation coverage

For **every** `correctness_facts.dependent_tables` entry and **every** observable mutation kind (`insert`, `update`, `delete`, column-specific updates), assign exactly one behavior:

| Behavior | Meaning |
|----------|---------|
| `supported_delta` | Incremental update preserves correctness |
| `bounded_rebuild` | Rebuild affected partition(s) only |
| `full_rebuild` | Rebuild all partitions (rare; justify) |
| `invalidate_safely` | Mark stale; fall back to base kernel until repaired |

## Rules

- No table × mutation pair may be omitted.
- If you cannot assign one of the four behaviors, aux-state is not viable for this query.
- Implementation tests must cover each non-`supported_delta` behavior at least once.
