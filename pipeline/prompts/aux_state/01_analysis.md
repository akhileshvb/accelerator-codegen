# Stage 1: Aux-state analysis

Design an auxiliary-state accelerator for TPC-H **Q{{query_number}}** (`{{aux_function_name}}`).

## Workload

- Accelerator id: `{{accelerator_id}}`
- Base accelerator: `{{base_function_name}}`
- Cost model config: `{{cost_model_config}}`

{{workload_reference_section}}

{{optional_reference_accelerators}}

## Shared docs (read on demand)

{{shared_docs_reference}}

## Revision evidence (optional)

{{analysis_revision_request}}

## Task

Write `{{full_analysis_path}}` per schema. **Do not implement code.**

- **Initial run:** SQL/workload assumptions only — no post-implementation measurements.
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

`dependent_tables`, `partitioning`, `state_payload`, `legal_deltas`, `mutation_coverage` (exactly `insert`/`update`/`delete` per table — column detail in `notes`; see `mutation_coverage.md`), `fallback_conditions`, `maintenance_actions_used`, `cross_accelerator_concurrency` (see `concurrency_contract.md`), `state_design`, `alternatives_considered` (≥2 brief entries contrasting **different** designs from the search).

Keep prose fields concise (≤3 sentences each unless schema requires lists).

### 5. Performance hypotheses

Maintenance fan-out, likely actions, initial cost model, eligibility rules, speedup plan — hypotheses only.

### 6. File plan

`files_to_create` per `module_layout.md`.

{{agent_boundary}}

{{constraints}}
