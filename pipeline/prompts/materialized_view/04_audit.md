# Stage 4: Audit (matview)

Audit the **Q{{query_number}}** Postgres matview accelerator.

## Checklist

- [ ] Per-query MV (not shared / not blind full-query cache without rationale)
- [ ] SQL-only artifacts under `{{matview_subdir}}`
- [ ] Residual query matches reference after create and after REFRESH
- [ ] Indexes justified; CONCURRENTLY only with unique index
- [ ] Gates: `mv_correctness`, `mv_refresh`, `mv_bench` pass
- [ ] No C++ / aux substrate edits

Write `final_audit.md` under query artifacts.

{{agent_boundary}}

{{constraints}}
