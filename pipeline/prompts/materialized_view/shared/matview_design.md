# Materialized-view design (per query, SQL-only)

Design a **query-specific** Postgres [materialized view](https://www.postgresql.org/docs/current/rules-materializedviews.html) that accelerates **this** query. Storage is native Postgres (table-like). Updates use **`REFRESH MATERIALIZED VIEW`** only (replaces contents).

## Goals

1. Maximize read-path speedup vs native Postgres SQL for this query’s bindings.
2. Keep REFRESH cost acceptable (write/maintenance axis).
3. Remain exact vs reference SQL after refresh.

## Anti-patterns

| Anti-pattern | Why reject |
|--------------|------------|
| Blind `CREATE MATERIALIZED VIEW AS <full query text>` with no residual plan | Often no better than caching answers; poor for parameterized queries |
| One shared MV for many queries | This pipeline is **per-query** |
| Custom C++ / aux structures | Out of scope — SQL only |
| Incremental MERGE/delta without REFRESH | Not used; always REFRESH |

## Preferred shapes

- Pre-aggregated summaries at a grain coarser than output, plus residual filter/agg
- Narrow join intermediates + small residual joins/filters
- Prefiltered fact subsets / covering projections with indexes matching residual predicates
- Unique index on MV when `REFRESH CONCURRENTLY` is chosen

## Required candidate search

When fitness is recommended, propose ≥3 **materially different** candidates (different grain, defining SELECT, or index set) unless `fewer_than_three_explanation` is set. Select one; document residual query and refresh mode.

## Correctness

Residual query over the MV must match reference SQL for the same parameters after a fresh build and after REFRESH following mutations on dependent tables.
