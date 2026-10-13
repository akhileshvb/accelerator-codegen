# Stage 1: Materialized-view analysis (per query, SQL-only)

Design a **query-specific** Postgres materialized view accelerator for **{{display_name}}** **Q{{query_number}}** (`{{mv_function_name}}` / relation `{{mv_relation_name}}`).

{{hard_guidelines}}

## Workload

- Accelerator id: `{{accelerator_id}}`
- Engine: `postgres` (SQL-only — no C++ aux)
- Matview subdir: `{{matview_subdir}}`

{{workload_reference_section}}

{{optional_reference_accelerators}}

## Shared docs (read on demand)

- `pipeline/prompts/materialized_view/shared/matview_design.md`
- `pipeline/prompts/materialized_view/shared/agent_boundary.md`
- `pipeline/prompts/materialized_view/shared/constraints.md`
- Postgres matviews: https://www.postgresql.org/docs/current/rules-materializedviews.html

## Task

Write `{{full_analysis_path}}` per `pipeline/schemas/materialized_view_analysis.schema.json`. **Do not implement SQL yet.**

1. **Fitness** — whether an MV helps this query under REFRESH-only maintenance.
2. **Base-path cost** — what native Postgres SQL repeats that an MV can remove.
3. **Candidate search** — ≥3 query-specific candidates (different grain / defining SELECT / indexes) unless `fewer_than_three_explanation` is set. Prefer intermediate grain + residual SQL.
4. **Select** — best read speedup subject to acceptable REFRESH cost. Fill `correctness_facts` with full `create_materialized_view_sql`, `index_sql`, `residual_query_sql`, `refresh_sql`, mutation → `refresh_materialized_view`.
5. **File plan** — `create.sql`, `indexes.sql`, `query.sql`, `function.sql`, `refresh.sql` under `{{matview_subdir}}`.

{{agent_boundary}}

{{constraints}}
