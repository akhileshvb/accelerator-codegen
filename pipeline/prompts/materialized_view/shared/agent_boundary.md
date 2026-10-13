# Agent boundary: Postgres SQL-only materialized views

## Agent may generate (query-specific)

Under `src/generated/{benchmark}/q{N}/postgres/matview/` **only**:

| File | Role |
|------|------|
| `create.sql` | `CREATE MATERIALIZED VIEW … AS …` for **this** query |
| `indexes.sql` | Indexes on that MV tailored to residual access |
| `query.sql` | Parameterized residual `SELECT` over the MV (and tiny joins if needed) |
| `function.sql` | Optional `CREATE FUNCTION … LANGUAGE sql` wrapper (`{{mv_function_name}}`) |
| `refresh.sql` | `REFRESH MATERIALIZED VIEW [CONCURRENTLY] …` |
| `reference.sql` | Optional copy/adapted reference SQL for gates |

## Forbidden

- C++ (`module.cpp`, kernels, AuxQueryModule, extension SRFs) for this workload
- Editing `aux/`, `base/`, `stateful/`, `runtime/`, catalog, or CMake for matview work
- Incremental delta maintenance other than Postgres `REFRESH` (which replaces MV contents)
- Shared cross-query matviews (one MV suite per query)
- Non-Postgres engines

## Maintenance

On relevant base-table mutations: run `refresh.sql` only. No ApplyDelta / custom stores.

## Registration / apply

Apply SQL into Docker Postgres (`accel_pg_test`) via `scripts/postgres_matview_apply.sh` / gates `mv_apply`. Do not invent a second adapter tree.
