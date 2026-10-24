# Stage 1: Aux-state analysis

Design a stateful (aux-state) accelerator for **{{display_name}}** **Q{{query_number}}** (`{{aux_function_name}}`).

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

### 1. Fitness

Output `fitness_assessment`. Read `aux_state_fitness.md` if needed.

**Goal:** maximize **read-path** speedup by eliminating repeated base-path work — **not** by caching final query answers.

### 2. Base-path cost decomposition

In `performance_hypotheses.base_path_cost_decomposition`: dominant scans/joins/agg/sort, parameter-dependent work, scaling with cardinality/range/output, work remaining after aux replacement. Label as hypotheses.

Ask explicitly: *What work does the query repeat on every invocation that we can move into maintained state?*

### 3. State candidate search (when recommended) — think hard here

Read `taxonomy_index.md` first; open `aux_state_taxonomy.md` and `aux_state_design.md` for families you shortlist.

**Read-path objective (required):**
- Pick designs that **minimize read-time work** (scans eliminated, joins folded into state, output-sized residual work only).
- **Forbidden:** full-result caches — storing final answer rows or output-grain payloads with no meaningful derivation left (see `aux_state_design.md` prohibited full-result cache).
- **Allowed:** compact intermediate state at **partition/state grain** that still requires a bounded, documented read algorithm (filter, combine, sort output-sized data).
- For each candidate, state what base-path operators are eliminated and what **residual read algorithm** remains.

**Diversity (required):**
- Propose ≥3 **materially different** viable candidates unless `fewer_than_three_explanation` is non-empty.
- "Materially different" means at least one of: `design_family`, `physical_organization`, `composition_rule`, or asymptotic read behavior must differ — not just naming tweaks on the same layout.
- Prefer spanning different taxonomy families (e.g. pre-aggregated summaries vs compact join intermediates vs selective indexes vs dense layouts).
- Reject near-duplicates; use `viability: rejected` with `rejection_reason` for variants you considered but ruled out.

**Bound check (required for every viable candidate, before selecting):**
- **Any binding.** For each template parameter, name the binding that maximizes read work (widest range, a value at the end of the domain, a pattern with no literal, a value matching most rows) and the one that minimizes it. Estimate state entries read for both at SF1 and SF3000 from numbers you measured with the data tools (`rows_qualifying` at each extreme, `join_fanout`), and record the numbers in `estimated_state_units_read` (`typical`, `worst_case`).
- **Pruning follows the predicate.** A bound used to skip state (max/min/count summaries, ranges) must tighten with the bound parameter values: an entry that cannot contribute for a binding is skipped without visiting it. A bound that is the same for every binding (an entry's total over all parameter values) prunes nothing where it matters. Organize state so a binding's qualifying data is a contiguous range or a few partitions.
- **Data narrows the range, never assumed.** Spreads and correlations between columns the predicates combine (two date columns of joined tables, a value bounded by another; measure them with `column_spread`) can shrink the candidate range. Derive such a bound from the state itself and maintain it under writes (a write that widens the spread widens the bound), so arbitrary data stays exact.
- **Heavy writes.** Estimate per-commit cost at the write mix's highest write frequency and at refresh size: a commit touches state in proportion to changed rows times their real fan-out; it never re-sorts, re-scans or rebuilds unchanged data, and dimension-table writes reach only actual dependents.
- Revise or reject a candidate whose worst-case read for some valid binding scans state in proportion to a table when another design reads in proportion to the qualifying rows or the output.

**Evidence (required):** `analysis.json` `evidence` lists each measured figure the bound check or a design choice relies on: `figure` (what it is), `value`, and `tool_call` (the call that produced it, as made). Every entry must match a logged call in `data_queries.jsonl`.

**Selection:**
- Exactly one `selected: true` — the best **worst-case** read over all valid bindings (then typical), subject to cheap maintenance under the heavy write mix.
- Flag scan-for-scan swaps on the selected design (`read_path_bottleneck_risk` should reflect this).

**Tiered detail:** selected candidate = full schema fields; viable alternates = `id`, `design_family`, `state_grain`, `physical_organization`, `composition_rule`, `operators_eliminated`, `read_path_bottleneck_risk`, `best_regime` (1 sentence); rejected = `id`, `design_family`, `state_grain`, `physical_organization`, `rejection_reason`.

### 4. Correctness facts (selected design)

`dependent_tables`, `partitioning`, `state_payload`, `legal_deltas`, `mutation_coverage` (exactly `insert`/`update`/`delete` per table — column detail in `notes`; see `mutation_coverage.md`), `fallback_conditions`, `maintenance_actions_used`, `cross_accelerator_concurrency` (see `concurrency_contract.md`), `scale_plan` (below), `state_design`, `alternatives_considered` (≥2 brief entries contrasting **different** designs from the search).

Keep prose fields concise (≤3 sentences each unless schema requires lists).

**Generality in the design (required):** for the selected candidate, list in `fallback_conditions`/`notes` (a) each template parameter and how an arbitrary value (incl. NULL, off-domain) is answered exactly and fast when not precomputed, (b) key/offset width and capacity at SF1000+, (c) bytes/row estimate and chosen `prims/` primitives (`prims/README.md` "Which one to use"; services and helpers in `substrate_primitives.md`), (d) per-write cost for each mutation, (e) how a schema-valid write that duplicates a key the query joins or groups on is answered exactly (handled, or declined to the SQL fallback while the keys are not unique). Reject candidates that need a parameter-domain enumeration, narrow key packing, or whole-state copy/rebuild per write.

**Scale and parallelism (required, `correctness_facts.scale_plan`).** Design for SF3000 (`scale_factor_contract.md`: ~2e10 fact rows, ids beyond 2^32, sums beyond 2^63, refreshes of 10^5-10^7 rows, all state in memory). Choose state whose size, build, refresh and read costs follow touched/selected keys, not total rows. Fill `failure_classes` with one `{class, handling}` per row of the contract's failure-class table (state `n/a` with a reason only when it cannot apply), and say how each phase splits over its thread cap using the substrate helpers (`substrate_primitives.md`): `read_parallelism` (what `Execute` splits, or why a read is output-bound and stays one slice), `build_parallelism` (the `PartitionedBuild` `Partial` and its merge; a floating-point partial uses `kFloatBuildPartitions`), `maintenance_parallelism` (which per-key work splits after the serial engine lookups). Reject candidates whose build or refresh is inherently one serial pass over rows, whose merge is not associative, or whose partials are not cheap to create.

**Primitives (optional fields `primitives_used[]`, `primitives_proposed[]`).** List each catalogued primitive the design uses with `name` and `why`. For a container no primitive covers, list a `primitives_proposed[]` entry: `name` (generic, by what it is), `description` (generic, no table/column/query words), `api_sketch`, `closest_existing` and `why_not_closest`. Implementation publishes it with `primitive_publish` before use. Workload-specific structures stay in the module (still COW, built from primitives) and are not proposed.

### 5. Performance hypotheses

Maintenance fan-out, likely actions, initial cost model, eligibility rules, speedup plan, and the low-level execution plan (vector width and fallback, layout, prefetch) for the read path: hypotheses only.

### 6. File plan

`files_to_create` per `module_layout.md`.

{{agent_boundary}}

{{constraints}}
