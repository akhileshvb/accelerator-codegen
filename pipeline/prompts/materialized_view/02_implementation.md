# Stage 2: Implementation (SQL-only matview)

Implement the selected per-query materialized view for **Q{{query_number}}** using **Postgres SQL only**.

## Emit under `{{matview_subdir}}`

1. `create.sql` — `CREATE MATERIALIZED VIEW {{mv_relation_name}} AS …` (from analysis)
2. `indexes.sql` — indexes for residual access; unique index if `REFRESH CONCURRENTLY`
3. `query.sql` — parameterized residual SELECT over the MV
4. `function.sql` — `CREATE OR REPLACE FUNCTION {{mv_function_name}}(…) … LANGUAGE sql` calling residual logic
5. `refresh.sql` — `REFRESH MATERIALIZED VIEW [CONCURRENTLY] {{mv_relation_name}}`

## Apply / verify

1. Ensure Docker Postgres: `bash scripts/ensure_accel_pg_test.sh` (TPC-H loaded).
2. `bash scripts/postgres_matview_apply.sh {{query_number}}` or MCP/`request_gate` `mv_apply`.
3. `mv_correctness` → `mv_refresh` → `mv_bench` until pass.

No C++ / aux / CMake / catalog edits.

Analysis summary:

```json
{{analysis_json}}
```

{{agent_boundary}}

{{constraints}}
