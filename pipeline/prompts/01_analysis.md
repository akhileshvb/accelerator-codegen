# Stage 1: Analysis

Analyze **{{display_name}}** query **Q{{query_number}}** for host **`{{engine_name}}`** before any implementation.

## Goal

Plan a **stateless kernel that runs at machine speed on this hardware, at SF3000**: SIMD inner loops (AVX-512/AVX2 with a scalar fallback), cache-conscious columnar layouts, branch-free predicates, prefetch, no per-row allocation, fused passes. The plan names the loops and the layout that make each phase fast; the profile-guided loop refines it later. Read `pipeline/prompts/shared/low_level_execution.md`. Correctness comes first.

This stage owns the plan only (`{{analysis_artifact_path}}`); do not implement code. `engine_name` is **required**: plan an engine-specific consume path under `accelerators/{benchmark}/q{{query_number}}/{{engine_name}}/`, with no portable chunk IR and no default host.

{{hard_guidelines}}

## Repositories

- {{repo_root}}

{{schema_reference_section}}

{{backend_api}}
{{accelerator_engine_profile}}
## Query

```sql
{{reference_sql}}
```

## Parameters

{{parameters_yaml}}

## Output schema

{{output_schema_yaml}}

Layout contract: `pipeline/prompts/shared/generated_layout.md`. Scale contract: `pipeline/prompts/shared/scale_factor_contract.md` (plan capacities from bind-time estimates).

## Task

1. Inspect similar accelerators **for this engine** for scan order, batch/row layouts, aggregation structures, tests, benchmarks.
2. Identify logical plan and simplest query-local execution strategy using **{{engine_name}}** native base access (engine-local batches / slots / fused APIs — not portable DataChunk).
3. Rank likely bottlenecks (phase-level) and list concrete `optimization_hypotheses`.
4. For GROUP BY on composite keys (multiple VARCHAR/keys): plan **build-time interning** → dense `group_id` → vector-indexed sums on consume (never `unordered_map<std::string,...>` per row).
5. For each hot phase, name the vectorization and layout choice (SIMD width and fallback, selection vectors, block size, prefetch).
6. List exact files to create under `.../q{{query_number}}/{{engine_name}}/` (kernel, package, header, catalog touch, tests, benchmark).
7. Aux (if in scope later) still speaks shared `MutationBatch` / `AuxQueryModule`; only base consume is engine-private.

{{benchmark_notes}}

## Output

Write `{{analysis_artifact_path}}` with:

- `logical_plan`, `scan_order`, `transient_structures`, `likely_bottlenecks`
- `optimization_hypotheses` (technique → expected bottleneck impact), including the low-level execution choices
- `files_to_create`
- note `engine_name: {{engine_name}}` and the engine-local consume approach

{{constraints}}
