# Constraints: materialized_view workloads

- **`engine_name` must be `postgres`.** Refuse other engines.
- Artifacts: SQL under `accelerators/{benchmark}/q{N}/postgres/matview/` only.
- Maintenance: `REFRESH MATERIALIZED VIEW` (optionally `CONCURRENTLY`) only — replaces MV contents.
- No C++ accelerators, AuxQueryModule, or extension kernels for this kind.
- Per-query MV: do not invent a shared multi-query matview.
- Fair benches: time full SQL path (including refresh on write mixes); consume all results.
- Docker host: `accel_pg_test` via existing Postgres Docker helpers.
