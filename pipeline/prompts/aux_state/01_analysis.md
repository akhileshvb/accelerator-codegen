# Stage 1: Aux-state analysis

Design an auxiliary-state accelerator for **{{display_name}}** **Q{{query_number}}** (`{{aux_function_name}}`).

{{hard_guidelines}}

## Workload

- Accelerator id: `{{accelerator_id}}`
- Base accelerator: `{{base_function_name}}`
- Cost model config: `{{cost_model_config}}`

{{workload_reference_section}}

{{backend_api}}
{{accelerator_engine_profile}}

## Shared docs (read on demand)

{{shared_docs_reference}}

## Revision evidence (optional)

{{analysis_revision_request}}

## Task

Write `{{full_analysis_path}}` per schema. **Do not implement code.**

- **Initial run:** use the **Query** SQL and **Data shape** section (plus workload hints) to choose aux state — no post-implementation measurements.
- **Revision run** (non-empty evidence above): use measured bottlenecks; emit a new complete correctness contract.
- If `fitness_assessment.recommended` is false, document why and stop unless override is configured.

### 1. Fitness

Output `fitness_assessment`. Read `aux_state_fitness.md` if needed.

**Goal:** maximize **read-path** speedup by eliminating repeated base-path work — **not** by caching final query answers.

### 2. Base-path cost decomposition

In `performance_hypotheses.base_path_cost_decomposition`: dominant scans/joins/agg/sort, parameter-dependent work, scaling with cardinality/range/output, work remaining after aux replacement. Label as hypotheses.

Ask explicitly: *What work does the base kernel repeat on every invocation that we can move into maintained state?*

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

**Selection:**
- Exactly one `selected: true` — the candidate with the best **read-path** upside subject to maintainable write path.
- Flag scan-for-scan swaps on the selected design (`read_path_bottleneck_risk` should reflect this).

**Tiered detail:** selected candidate = full schema fields; viable alternates = `id`, `design_family`, `state_grain`, `physical_organization`, `composition_rule`, `operators_eliminated`, `read_path_bottleneck_risk`, `best_regime` (1 sentence); rejected = `id`, `design_family`, `state_grain`, `physical_organization`, `rejection_reason`.

### 4. Correctness facts (selected design)

`dependent_tables`, `partitioning`, `state_payload`, `legal_deltas`, `mutation_coverage` (exactly `insert`/`update`/`delete` per table — column detail in `notes`; see `mutation_coverage.md`), `fallback_conditions`, `maintenance_actions_used`, `cross_accelerator_concurrency` (see `concurrency_contract.md`), `scale_plan` (below), `state_design`, `alternatives_considered` (≥2 brief entries contrasting **different** designs from the search).

Keep prose fields concise (≤3 sentences each unless schema requires lists).

**Generality in the design (required):** for the selected candidate, list in `fallback_conditions`/`notes` (a) each template parameter and how an arbitrary value (incl. NULL, off-domain) is answered exactly and fast when not precomputed, (b) key/offset width and capacity at SF1000+, (c) bytes/row estimate and chosen `prims/` primitives (`prims/README.md` "Which one to use"; services and helpers in `substrate_primitives.md`), (d) per-write cost for each mutation. Reject candidates that need a parameter-domain enumeration, narrow key packing, or whole-state copy/rebuild per write.

**Scale and parallelism (required, `correctness_facts.scale_plan`).** Design for SF3000 (`scale_factor_contract.md`: ~2e10 fact rows, ids beyond 2^32, sums beyond 2^63, refreshes of 10^5-10^7 rows, all state in memory). Choose state whose size, build, refresh and read costs follow touched/selected keys, not total rows. Fill `failure_classes` with one `{class, handling}` per row of the contract's failure-class table (state `n/a` with a reason only when it cannot apply), and say how each phase splits over its thread cap using the substrate helpers (`substrate_primitives.md`): `read_parallelism` (what `Execute` splits, or why a read is output-bound and stays one slice), `build_parallelism` (the `PartitionedBuild` `Partial` and its merge; a floating-point partial uses `kFloatBuildPartitions`), `maintenance_parallelism` (which per-key work splits after the serial engine lookups). Reject candidates whose build or refresh is inherently one serial pass over rows, whose merge is not associative, or whose partials are not cheap to create.

**Primitives (optional fields `primitives_used[]`, `primitives_proposed[]`).** Consult the `prims/README.md` catalogue (it may be empty) first. List each existing primitive the design uses with `name` and `why`. If the state needs a container no primitive covers, list a `primitives_proposed[]` entry: `name` (generic, by what it is), `description` (generic, no table/column/query words), `api_sketch`, `closest_existing` (which existing primitive is nearest) and `why_not_closest`. Proposed primitives are written and published (`primitive_publish`) in implementation, before use. Workload-specific structures stay in the module (still COW, built from primitives) and are not proposed.

### 5. Performance hypotheses

Maintenance fan-out, likely actions, initial cost model, eligibility rules, speedup plan — hypotheses only.

### 6. File plan

`files_to_create` per `module_layout.md`.

{{agent_boundary}}

{{constraints}}
