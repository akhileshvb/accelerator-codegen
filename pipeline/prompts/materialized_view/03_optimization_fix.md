# Stage 3: Optimization fix (matview)

Fix failing matview gates or regressions for **Q{{query_number}}**.

- Re-apply SQL, re-run `mv_correctness` / `mv_refresh` / `mv_bench`
- Edit only `{{matview_subdir}}/**/*.sql`
- Do not introduce C++ or non-REFRESH maintenance

{{agent_boundary}}

{{constraints}}
