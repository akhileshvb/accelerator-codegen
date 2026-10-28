# Effective aux-state design

Families and selection rubric: `aux_state_taxonomy.md`; `design_family` names: `taxonomy_index.md`; fit signals: `aux_state_fitness.md`.

Aux state must materially accelerate the query and stay maintainable: the fast path avoids the engine plan's dominant work, the state is compact, and read work after the lookup is small relative to the work removed.

## Reject

| Anti-pattern | Why |
|---|---|
| Full-result cache | Entries hold the answer for particular parameter bindings: they serve only those bindings, and covering every binding would multiply the state. |
| Negligible-speedup state | The fast path still does most of the query's work. |
| Scan-for-scan swap | The base scan is replaced by a linear scan or merge over many partitions, buckets or nodes of similar asymptotic cost. |

**Full-result cache test:** the state answers some bindings and not others: result sets keyed by parameter values, rows computed for a default or recently seen binding, or anything a new binding would have to add. Valid: one binding-independent state that serves every binding, with the parameter-dependent work (filter, range seek, grouping, top-k, final combine) done at read time. That state may sit at output-row grain: finished per-group aggregates that every binding selects from are allowed; decomposing them into partials only to avoid that grain adds work and buys nothing. Document `state_grain` vs `output_grain`.

## Parameter generality (every family)

The read algorithm is exact for any parameter value (`hard_guidelines.md` section 4). Coarse grains and precomputed value lists only accelerate: pair them with an exact residual (finer grain, Fenwick/range tree over the exact key, bounded edge scan) instead of restricting the domain. Keys are 64-bit, possibly sparse or negative; payload arithmetic is exact. Build containers from `prims/` (`cow_primitives.md`).

## Workload hints

`dependent_tables` and `partitioning` in the workload YAML are starting points. Derive independently: relevant tables and columns, candidate grains and physical organizations, mutation-to-state mappings, read/write trade-offs. Record adopted hints in `state_candidate_search.hints_adopted`, rejected ones in `hints_rejected`. Do not assume a hinted flat grain is optimal without comparing materially different organizations.

## Workflow

1. **Base-path cost decomposition** in `performance_hypotheses.base_path_cost_decomposition` (dominant work and scaling) before proposing state.
2. **Candidate search** (when `recommended`): at least three materially different candidates in `state_candidate_search.candidates`, each with a `design_family`, or `fewer_than_three_explanation`.
3. **Residual read path** per candidate: composition rule, residual read algorithm, units read, bottleneck risk; flag any O(partitions) or O(history) work.
4. **Select**; copy the winner's read-path summary into `correctness_facts.state_design` with `selected_candidate_id`.
5. **Prove correctness** for the selected design only (`correctness_facts` complete).

Per candidate and for the selected design:

- `read_path_bottleneck_risk`: `low` | `medium` | `high` (high: the fast path still merges or scans a large fraction of partitions/history)
- `read_path_scaling_variable`: `number_of_partitions`, `parameter_range`, `groups`, `other`
- `estimated_state_units_read`: `{typical, worst_case}` (partitions, nodes, buckets or entries)
- `read_work_ratio` (selected): `{typical, worst, basis}` = base-path work / design work per read; `typical` >= 100 or an `unmet_justification`
- `parameter_scaling_argument`: read cost across representative bindings
- `residual_read_algorithm`: exact steps from state to answer

Encouraged: per-partition aggregates feeding the final result, coarser join intermediates, narrow prefiltered subsets, index-like structures bounding probe work without storing result rows, hierarchical or prefix-composable structures when the parameter range would force wide merges.
