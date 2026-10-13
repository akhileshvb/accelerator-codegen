# Effective aux-state design

For a catalog of **26 state families** (partitioned aggregates, hierarchies, join-derived state, top-k, existence counts, hybrids, etc.) and a step-by-step selection rubric, read `aux_state_taxonomy.md`.

Aux state must **materially accelerate** the query while remaining maintainable under mutations.

## Design goal

- Fast path avoids dominant base-kernel work (large scans, joins, heavy aggregates).
- State is compact enough for tractable storage and incremental maintenance.
- Read-path work after aux lookup is **small relative to** work eliminated.

## Anti-patterns

| Anti-pattern | Why reject |
|--------------|------------|
| **Full-result cache** | Entries directly encode complete query answers or result rows with little reusable intermediate structure. |
| **Negligible-speedup state** | Fast path still runs most of the base kernel; maintenance cost exceeds benefit. |
| **Scan-for-scan swap** | Base-table scan removed but replaced by a linear scan or merge over many auxiliary partitions, buckets, or index nodes with similar asymptotic cost. |

## Parameter generality applies to every family

Whatever the family, the read algorithm must be exact for any parameter value (`hard_guidelines.md` §4). Coarse grains (months, buckets, precomputed value lists) are acceleration structure only: pair them with an exact residual (finer grain, Fenwick/range tree over the exact key, or bounded edge scan) rather than restricting the parameter domain. Key spaces are 64-bit, possibly sparse or negative; payload arithmetic is exact. Choose containers via `prims/README.md` "Which one to use" (copy-on-write, built from `prims/`).

## Prohibited full-result cache (precise definition)

A **full-result cache** stores, per partition or parameter binding, the **final answer artifact** the user would read from the query.

| Valid | Invalid |
|-------|---------|
| `month → {promo_sum, total_sum}` — sufficient statistics at partition grain; bounded final combine step | Every output row for every parameter binding |
| Per-partition scalar aggregates sufficient for the final expression | Wide rows mirroring output schema |
| Coarser join intermediates with non-trivial read-path reduction | Exact result set keyed only by parameters |

**Test:** If stored payload is isomorphic to the answer at **output row grain** with no meaningful derivation left, it is a full-result cache. Compact aggregates at **state partition grain** requiring a bounded combination step are **not** full-result caches.

## Workload hints are non-binding

`dependent_tables` and `partitioning` hints in the workload YAML are **starting points only**.

The analysis agent must independently derive:

- relevant tables and columns;
- candidate state grains;
- candidate physical organizations;
- mutation-to-state mappings;
- read/write tradeoffs.

Adopt a hint only when justified in `state_candidate_search.hints_adopted`. Record rejected or superseded hints in `hints_rejected`.

Do **not** assume a hinted flat partition grain is optimal without comparing materially different organizations.

## Required workflow

1. **Base-path cost decomposition** — analyze the base query conceptually; record dominant work and scaling in `performance_hypotheses.base_path_cost_decomposition` before proposing aux state.
2. **Candidate search** — when `fitness_assessment.recommended` is true, enumerate at least **three** materially different viable candidates in `state_candidate_search.candidates` unless `fewer_than_three_explanation` documents why fewer exist.
3. **Residual read-path analysis** — for every candidate, document composition rule, residual read algorithm, units read, and bottleneck risk. Explicitly flag cases where aux access still does O(partitions) or O(history) work.
4. **Select** — pick the best tradeoff; copy the winner's read-path summary into `correctness_facts.state_design` with `selected_candidate_id`.
5. **Prove correctness** — `correctness_facts` must be complete and provable for the selected design only.
6. Confirm design is neither full-result cache nor negligible-speedup. Document `state_grain` vs `output_grain`.

## Design families (teach patterns; do not prescribe query-specific structures)

Use `design_family` on each candidate. Candidates must differ materially in family, composition rule, or asymptotic read/write behavior. Payload-only tweaks do **not** count as distinct candidates.

| Family | Sketch |
|--------|--------|
| `flat_partitioned_sufficient_summaries` | Single-level partitions keyed by mutation grain; per-partition sufficient statistics |
| `multilevel_hierarchical_summaries` | Coarse summary plus finer children; read may prune subtrees |
| `range_prefix_composable_summaries` | Prefix/range structures where query predicate selects a composable suffix |
| `tree_or_index_aggregate_organization` | Tree, trie, or ordered index over keys with rolled-up aggregates |
| `selectively_replicated_summaries` | Duplicated aggregates at multiple grains for faster reads at cost of write fan-out |
| `compact_join_intermediate` | Narrow pre-joined or semi-joined state replacing repeated join work |
| `narrow_prefiltered_subset` | Materialized qualifying row subset or column projection |
| `hybrid_multi_grain` | Combines two or more grains or access paths (e.g. coarse buckets for interior + exact day-level edges, or daily partitions + group rollups) |

## Read-path bottleneck checks

For each candidate and for the selected design in `state_design`, assess:

- `read_path_bottleneck_risk`: `low` | `medium` | `high`
- `read_path_scaling_variable`: e.g. `number_of_partitions`, `parameter_range`, `groups`, `other`
- `estimated_state_units_read`: `{ "typical": "...", "worst_case": "..." }` — partitions, nodes, buckets, or entries touched on read
- `parameter_scaling_argument`: how read cost changes across representative parameter bindings
- `residual_read_algorithm`: exact steps to produce the answer from aux state

**High risk** when the fast path still merges or scans a large fraction of partitions/history even though the base fact scan was eliminated.

## Encouraged representations

- Per-partition aggregates feeding the final result.
- Coarser-grain join intermediates.
- Narrow prefiltered fact subsets.
- Index-like structures bounding probe work without storing result rows.
- Hierarchical or prefix-composable structures when parameter range would otherwise force wide partition merges.
