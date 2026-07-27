# Aux-state fitness criteria

Evaluate **per-query** suitability using **signals**, not hard requirements. Partition keys may come from bind parameters, grouping attributes, join keys, or other attributes.

## Good fit signals (any combination; none mandatory)

- **Stable partition grain**: Clear mapping from mutations to affected partitions.
- **Reusable partial aggregate**: Compact per-partition summary maintainable incrementally.
- **Bounded invalidation**: Mutations affect a predictable subset of partitions.
- **Incremental maintenance is plausible**: Deltas or bounded rebuilds for typical writes.
- **Read-heavy or mixed workload**: Repeated reads amortize maintenance cost.
- **Clear fallback story**: Base kernel reproduces correct results when state is missing/stale/txn-blocked.

## Weak / poor fit signals

- Predicate-dominated filtering with little cross-run reuse.
- Unbounded or global state requiring essentially raw data.
- High-cardinality partition space.
- Frequent whole-state invalidation.
- No stable grain for mutation → partition mapping.
- Semantics require full re-execution (volatile UDFs, non-deterministic logic).
- **Full-result cache** at output row grain (see `aux_state_design.md`).
- **Negligible-speedup state** that still runs most of the base kernel on the fast path.

## Per-query scope

- Design for **this query only** (`{{aux_function_name}}`).
- Do not optimize for cross-query state sharing.
- Note incidental reuse under `fitness_assessment.incidental_reuse` only.

## Analysis output

Produce `fitness_assessment` with `recommended`, `confidence`, `signals_for`, `signals_against`, `partition_key_source`, `rationale`.

If `recommended: false`, document why. Stop unless workload allows override (`allow_poor_fitness: true`).
