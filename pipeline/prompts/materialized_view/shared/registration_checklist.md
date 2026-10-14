# Registration / apply checklist (matview)

1. Emit SQL under `accelerators/{bench}/q{N}/postgres/matview/`.
2. Run `bash scripts/postgres_matview_apply.sh {N}` (or gate `mv_apply`) against `accel_pg_test`.
3. Verify `mv_correctness` (reference SQL vs residual / function).
4. Mutate a dependent table row, run `refresh.sql`, re-check correctness (`mv_refresh`).
5. Bench read-only and write-mix (`mv_bench`).

Do not register C++ catalog entries or AuxState CMake targets for this workload.
