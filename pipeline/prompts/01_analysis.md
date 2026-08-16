# Stage 1: Analysis

Analyze **{{display_name}}** query **Q{{query_number}}** before any implementation.

{{hard_guidelines}}

## Repositories

- {{repo_root}}
- {{duckdb_root}}

{{schema_reference_section}}

## Query

```sql
{{reference_sql}}
```

## Parameters

{{parameters_yaml}}

## Output schema

{{output_schema_yaml}}

## Reference accelerators

Inspect these existing implementations (read on demand): {{reference_accelerators}}

Layout contract: `pipeline/prompts/shared/generated_layout.md`

{{kernel_inspiration_section}}

## Performance goal

Design for **faster than the baseline engine path** on the benchmark dataset (single-threaded where required). Front-load optimization opportunities for the later profile-guided loop.

**Scale:** TPC-H iterates on SF1 but production targets SF20+ — plan with bind-time estimates (`scale_factor_contract.md`).

{{base_kernel_optimization}}

## Task

1. Inspect similar accelerators for scan order, vector layouts, aggregation structures, tests, benchmarks.
2. Identify logical plan and simplest query-local execution strategy (native base scans via `NativeScanSpec`).
3. Rank likely bottlenecks (phase-level) and list concrete `optimization_hypotheses`.
4. For GROUP BY on composite keys (multiple VARCHAR/keys): plan **build-time interning** → dense `group_id` → vector-indexed sums on consume (never `unordered_map<std::string,...>` per row).
5. List exact files to create (kernel, package, header, tests, benchmark).

{{benchmark_notes}}

## Output

Write `{{analysis_artifact_path}}` with:

- `logical_plan`, `scan_order`, `transient_structures`, `likely_bottlenecks`
- `optimization_hypotheses` (technique → expected bottleneck impact)
- `reference_accelerators_used`, `files_to_create`

Do not implement code in this stage.

{{constraints}}
