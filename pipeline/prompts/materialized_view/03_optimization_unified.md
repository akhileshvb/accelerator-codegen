# Stage 3: Optimization (per-query matview SQL)

Optimize **Q{{query_number}}** matview SQL with measured evidence in Docker Postgres.

## Allowed edits

Only files under `{{matview_subdir}}`: `create.sql`, `indexes.sql`, `query.sql`, `function.sql`, `refresh.sql`.

## Loop

1. `EXPLAIN (ANALYZE, BUFFERS)` residual query and native reference SQL.
2. A/B: change MV grain / defining SELECT / indexes / residual; re-`mv_apply`.
3. Accept if dual-axis improves: read latency vs native SQL, and write-mix including `REFRESH` cost.
4. Prefer structural MV redesign over micro-tweaks when stalled.

Still SQL-only; maintenance remains `REFRESH` only.

{{retrieval_brief}}

{{agent_boundary}}

{{constraints}}
