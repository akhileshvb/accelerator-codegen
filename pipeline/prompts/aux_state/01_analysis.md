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

### 2. Base-path cost decomposition

In `performance_hypotheses.base_path_cost_decomposition`: dominant scans/joins/agg/sort, parameter-dependent work, scaling with cardinality/range/output, work remaining after aux replacement. Label as hypotheses.

### 3. State candidate search (when recommended)

Read `taxonomy_index.md` first; open `aux_state_taxonomy.md` only for families you shortlist.

- ≥3 **materially different** candidates (`design_family`, `physical_organization`, or asymptotic behavior) unless `fewer_than_three_explanation`.
- **Tiered detail:** selected candidate = full schema fields; viable alternates = `id`, `design_family`, `state_grain`, `physical_organization`, `composition_rule`, `operators_eliminated`, `read_path_bottleneck_risk`, `best_regime` (1 sentence); rejected = `id`, `design_family`, `state_grain`, `physical_organization`, `rejection_reason`.
- Exactly one `selected: true`. Flag scan-for-scan swaps on the selected design.

### 4. Correctness facts (selected design)

`dependent_tables`, `partitioning`, `state_payload`, `legal_deltas`, `mutation_coverage`, `fallback_conditions`, `maintenance_actions_used`, `cross_accelerator_concurrency` (see `concurrency_contract.md`), `state_design`, `alternatives_considered` (≥2 brief entries).

Keep prose fields concise (≤3 sentences each unless schema requires lists).

### 5. Performance hypotheses

Maintenance fan-out, likely actions, initial cost model, eligibility rules, speedup plan — hypotheses only.

### 6. File plan

`files_to_create` per `module_layout.md`.

{{agent_boundary}}

{{constraints}}
