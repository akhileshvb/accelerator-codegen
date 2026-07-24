# Stage 1: Analysis

Analyze TPC-H query **Q{{query_number}}** before any implementation.

## Repositories

- {{repo_root}}
- {{duckdb_root}}

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

## Task

1. Inspect similar accelerators for join state, scan order, vector layouts, tests, benchmarks.
2. Identify logical plan and simplest query-local execution strategy.
3. Rank likely bottlenecks (phase-level).
4. List exact files to create (kernel, package, header, tests, manifest, benchmark).

## Output

Write `pipeline/artifacts/q{{query_number}}/analysis.json` with:

- `logical_plan`, `scan_order`, `transient_structures`, `likely_bottlenecks`
- `reference_accelerators_used`, `files_to_create`

Do not implement code in this stage.

{{constraints}}
