# Stage 1: Aux-state analysis

Design an auxiliary-state accelerator for TPC-H **Q{{query_number}}** (`{{aux_function_name}}`).

## Workload

- Accelerator id: `{{accelerator_id}}`
- Base accelerator: `{{base_function_name}}`
- Cost model config: `{{cost_model_config}}`

## Query

```sql
{{reference_sql}}
```

## Parameters

{{parameters_yaml}}

## Dependent tables (hints only — not binding)

{{dependent_tables_yaml}}

## Partitioning (hints only — not binding)

{{partitioning_yaml}}

{{optional_reference_accelerators}}

## Fitness criteria

{{aux_state_fitness}}

## Effective aux-state design

{{aux_state_design}}

## Mutation coverage rules

{{mutation_coverage}}

## Substrate and maintenance (reference)

{{substrate_contract}}

{{maintenance_framework}}

## Task

Produce `pipeline/artifacts/q{{query_number}}/analysis.json` following the aux-state analysis schema.

**Do not implement code.** Do **not** include post-implementation measurements.

If `fitness_assessment.recommended` is false, document why and stop unless override is configured.

### 1. Fitness assessment

Decide if aux-state is appropriate; output `fitness_assessment`.

### 2. Base-path cost decomposition (before proposing state)

In `performance_hypotheses.base_path_cost_decomposition`, analyze the **base** query path conceptually:

- dominant scans, joins, aggregation/grouping, sorting/top-k;
- parameter-dependent work and work repeated across invocations;
- how cost scales with table cardinality, parameter range, and output cardinality;
- which work might **remain expensive** after replacing base operators with aux-state access.

Label uncertain claims as hypotheses, not measurements.

### 3. State candidate search (when `recommended` is true)

Independently derive tables, columns, grains, and organizations. Do **not** stop at the first correct flat partitioned aggregate.

Populate `state_candidate_search`:

- `hints_adopted` / `hints_rejected` — how workload hints were used or overridden;
- `candidates` — at least **three** **materially different** viable candidates unless `fewer_than_three_explanation` is provided.

Each candidate must differ in `design_family`, `physical_organization`, or asymptotic read/write behavior. Changing only payload fields or bucket sizes is **not** a distinct candidate.

For **every** candidate, document:

- `state_grain`, `output_grain`, `physical_organization`, `state_payload`;
- `composition_rule` — how the final answer is derived from stored state;
- `operators_eliminated` — dominant base operators removed on the fast path;
- `residual_read_algorithm` — exact read steps after aux lookup;
- `estimated_state_units_read` (`typical`, `worst_case`);
- `read_path_bottleneck_risk`, `read_path_scaling_variable`, `parameter_scaling_argument`;
- read/update/storage complexity, `mutation_fanout`, `rebuild_locality`;
- `parameter_binding_behavior`, `history_growth_behavior`, `best_regime`;
- `viability` (`viable` | `rejected`) and `rejection_reason` when not viable.

Explicitly detect **scan-for-scan swaps**: aux state that removes the base-table scan but replaces it with a large linear merge over partitions or history.

Mark exactly one viable candidate `selected: true`.

### 4. Correctness facts (selected design only)

For the **selected** candidate, output complete `correctness_facts`:

- `dependent_tables`, `partitioning`, `state_payload`, `legal_deltas`;
- exhaustive `mutation_coverage`, `fallback_conditions`, `maintenance_actions_used`;
- `state_design` including `selected_candidate_id`, chosen representation, correctness/speedup arguments, and the same read-path bottleneck fields as candidates (`read_path_bottleneck_risk`, `read_path_scaling_variable`, `estimated_state_units_read`, `parameter_scaling_argument`, `residual_read_algorithm`);
- `alternatives_considered` — summary of non-selected candidates (at least two entries; prefer one per rejected viable candidate).

### 5. Performance hypotheses

Fan-out, likely maintenance actions, initial cost model, eligibility rules, speedup verification plan. Explicitly labeled as hypotheses to calibrate later.

### 6. File plan

`files_to_create` per `module_layout` conventions.

{{agent_boundary}}

{{constraints}}

{{module_layout}}

{{registration_checklist}}
