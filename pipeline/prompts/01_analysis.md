# Stage 1: Analysis

Analyze **{{display_name}}** query **Q{{query_number}}** before any implementation.

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

{{kernel_inspiration_section}}

## Performance goal

Design for **faster than DuckDB** on the benchmark dataset (single-threaded). The optimizer keeps improving while gates show progress — analysis should front-load optimization opportunities.

{{base_kernel_optimization}}

## Task

1. Inspect similar accelerators for scan order, vector layouts, aggregation structures, tests, benchmarks.
2. Identify logical plan and simplest query-local execution strategy.
3. Rank likely bottlenecks (phase-level) and list concrete `optimization_hypotheses`.
4. For GROUP BY on composite keys (multiple VARCHAR/keys): plan **build-time interning** → dense `group_id` → vector-indexed sums on consume (never `unordered_map<std::string,...>` per row).
5. List exact files to create (kernel, package, header, tests, benchmark).

{{benchmark_notes}}

## Output

Write `{{artifacts_path}}/analysis.json` with:

- `logical_plan`, `scan_order`, `transient_structures`, `likely_bottlenecks`
- `optimization_hypotheses` (technique → expected bottleneck impact)
- `reference_accelerators_used`, `files_to_create`

Do not implement code in this stage.

{{constraints}}
