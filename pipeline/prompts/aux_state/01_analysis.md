# Stage 1: Aux-state analysis

Design a stateful (aux-state) accelerator for **{{display_name}}** **Q{{query_number}}** (`{{aux_function_name}}`). If a tool or input you need is broken or missing, call `report_blocker` with the evidence instead of investigating pipeline code.

## Goal

**Plan state that makes reads fast and maintenance cheap, built from copy-on-write primitives, at SF3000.** Do not write code; write `{{full_analysis_path}}`.

1. Pick the state that removes the base path's repeated work (never a cached final answer) and whose write cost follows touched data.
2. Build it from the primitive library: reuse catalogued primitives (`primitives_used`), or specify a bespoke, generic COW primitive for this query template's access pattern (`primitives_proposed`). Guide: `pipeline/prompts/aux_state/shared/cow_primitives.md`.
3. Name the low-level execution of the read, build and maintain loops: SIMD with a scalar fallback, cache-conscious layout, branch-free predicates, prefetch. Guide: `pipeline/prompts/shared/low_level_execution.md`.

{{hard_guidelines}}

## Workload

- Accelerator id: `{{accelerator_id}}`
- Cost model config: `{{cost_model_config}}`

{{workload_reference_section}}

{{backend_api}}
{{accelerator_engine_profile}}

## Shared docs (read on demand)

{{shared_docs_reference}}

## Revision evidence (optional)

{{analysis_revision_request}}

## Task

Write `{{full_analysis_path}}` per schema.

- **Initial run:** use the **Query** SQL and the data (measured with the **Data** tools, plus workload hints) to choose aux state — no post-implementation measurements.
- **Revision run** (non-empty evidence above): use measured bottlenecks; emit a new complete correctness contract.
- If `fitness_assessment.recommended` is false, document why and stop unless override is configured.

### 1. Fitness and base-path cost

`fitness_assessment` (`aux_state_fitness.md` if needed) and `performance_hypotheses.base_path_cost_decomposition`: the scans, joins, aggregation and sort the query repeats on every call, what depends on the parameters, and how each scales with data, parameter range and output. The goal is to move that repeated work into maintained state, never to cache final answers.

### 2. Design search (think hard here)

Taxonomy: `taxonomy_index.md`, then `aux_state_taxonomy.md` / `aux_state_design.md` for the families you shortlist.

1. **Go wide.** One candidate per applicable family: state partitioned or ordered by the predicate's columns; pre-aggregation at a coarser grain with exact refinement; data-derived bounds that tighten with the parameter; for `ORDER BY ... LIMIT k`, bound-ordered access (a maintained upper bound of the ranking value per block or group, visited best-first, stopping when the next bound cannot beat the k-th result). At least three materially different candidates (family, organization, composition or asymptotic read differ), or `fewer_than_three_explanation`. Never a full-result cache (`aux_state_design.md`).
2. **Measure each on the data.** Write the candidate's read as one `query_data` statement over the base tables that counts what it would visit (partitions or ranges selected, entries its bound fails to prune, entries before its stopping rule fires). Run it at the typical binding, at each parameter's extremes (the binding that maximizes read work and the one that minimizes it), and under a write that loosens the bound or widens the range. Record the statement as the candidate's `units_probe`, exactly as run, and what it returned as `measured_units` (validation matches the probe against `data_queries.jsonl`). A count you did not run is not evidence.
3. **Refine the leader.** Simulate variants that attack what its counts expose (per-entry instead of global bounds, bounds that depend on the parameter, a finer or coarser grain, another sort key, two families combined) until a variant stops improving materially; list them as candidates. A bound must tighten with the bound parameter values: one that is the same for every binding prunes nothing where it matters. Spreads between columns the predicates combine (`column_spread`) can narrow a range, but only as a bound derived from the state and maintained under writes.
4. **Price the read in time.** Base path: about 1 ns per row and column it touches (`rows_qualifying`, `join_fanout`, table sizes; `query_data` also reports the engine's own time). Design: nanoseconds per compact or SIMD-scanned entry; 300 ns per tree node, hash probe or pointer-reached entry unless you measured less. At SF3000 the typical read must be at least ~100x cheaper than the base path and the worst case sublinear in table size or output-bounded. Record `correctness_facts.state_design.read_work_ratio`: `typical`, `worst` (numbers), `basis` (units and per-unit costs on both sides); below 100 typical also needs `unmet_justification` and `families_evaluated` (`family`, numeric `typical_ratio`, `why_short`) for each family tried. A candidate that misses is revised here, not selected.
5. **Price the writes.** At the write mix's highest frequency and at refresh size, a commit touches state in proportion to changed rows times their real fan-out: no re-sort, re-scan or rebuild of unchanged data; dimension writes reach only actual dependents.
6. **Select.** Exactly one `selected: true`. Compare candidates in time, not entry counts: worst-case read over all valid bindings, then typical, then throughput under the write mix. Reads that differ by less than about 1 µs, or that all beat the base path by more than 1000x, are a tie: then the cheaper writes win, then the smaller state. Otherwise, among candidates within about 2x on those, the smaller state. Never trade a large speedup for memory, and never trade memory for nanoseconds. Selected candidate: full schema fields; others: `id`, `design_family`, `state_grain`, `physical_organization`, `composition_rule`, `units_probe`, `measured_units`, and `rejection_reason` when rejected.

**Evidence:** every figure the design relies on goes in `evidence` (`figure`, `value`, `tool_call` as made, matching `data_queries.jsonl`). Call `validate_analysis` before ending a turn. Revise with `update_analysis` (JSON Patch on what a critique or validation error names); never regenerate the file.

### 3. Correctness contract (selected design)

`correctness_facts`: `dependent_tables`, `partitioning`, `state_payload`, `legal_deltas`, `mutation_coverage` (insert/update/delete per table, `mutation_coverage.md`), `fallback_conditions`, `maintenance_actions_used`, `cross_accelerator_concurrency` (`concurrency_contract.md`), `state_design`, `alternatives_considered`. Prose fields: three sentences at most.

- **Generality:** in `fallback_conditions`, how every value of each parameter's type (NULL, off-domain) is answered exactly; key and sum widths at SF3000; and how a schema-valid write that duplicates a join or group key is answered exactly (handled, or SQL fallback while keys are not unique). No parameter enumeration, narrow key packing, or whole-state copy per write.
- **Build only on what exists.** Every facility the design relies on (store and runtime calls, scan roles, maintenance actions, memory or version limits) must already exist in `substrate/`, `backends/<engine>/api/` or the primitive library; name the header for each. New generic COW containers are fine (`primitives_proposed`); new runtime mechanisms (admission control, memory reservation or accounting, draining or pinning versions) are not: plan memory and recovery with what the store already offers.
- **Scale and parallelism** (`scale_plan`, `scale_factor_contract.md`: ~2e10 fact rows, ids beyond 2^32, sums beyond 2^63, refreshes of 10^5-10^7 rows): `failure_classes` (one `{class, handling}` per row of the contract's table); `read_parallelism`, `build_parallelism` (the `PartitionedBuild` partial and its associative merge) and `maintenance_parallelism` under the thread caps (`substrate_primitives.md`). Build, read and maintenance run in parallel and thread-safe; a design whose build or refresh is one serial pass is rejected.
- **Primitives:** `primitives_used[]` (`name`, `why`) from `prims/README.md`; for a container none covers, `primitives_proposed[]` (`name` and `description` generic, `api_sketch`, `closest_existing`, `why_not_closest`), published by implementation before use.
- **State storage** (`state_design.state_storage[]`, validated): per component (every map, index, run array, counter and integrity map) `component`, `primitive` (the narrowest generic one: a directory is a COW map, run pages a paged COW array), `key`, `value` (types and widths), `read_access`, `write_access`, `cow_unit` (what one write copies), and numbers `entries` (measured), `bytes_per_entry`, `target_bytes` (SF3000). Store only what reads and writes need, in the narrowest exact type, one copy per fact, plain records in shared pages. **Memory at scale:** sum `target_bytes` and compare it with the base tables the state summarizes at SF3000. State grows with the data, so a footprint near or above those tables (a copy of every raw value, several fact-grain indexes) blows up at large scale factors: shrink the largest components or select a smaller candidate, and keep a fact-grain component only when no smaller layout reaches the same read and write cost class. Weigh memory against speed, without a fixed budget.

### 4. Hypotheses and files

`performance_hypotheses`: maintenance fan-out, likely actions, initial cost model, eligibility rules, and the low-level plan for the read path (vector width and fallback, layout, prefetch). `files_to_create` per `generated_layout.md`.

{{agent_boundary}}

{{constraints}}
