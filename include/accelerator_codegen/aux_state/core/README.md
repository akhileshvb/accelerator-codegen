# Shared auxiliary-state calibration

The shared core carries immutable, query-owned state images as typed in-process
payloads. DuckDB publishes those payloads directly, so its read and
maintenance paths never serialize or deserialize state. PostgreSQL's shared
memory adapter is the serialization boundary: it stores byte images and
deserializes them before exposing the same typed `StateSnapshot` to a module.

`aux_calibration.hpp` contains the engine-neutral calibration runner used by
the DuckDB and PostgreSQL adapters. A backend supplies callbacks that perform
the real operation against its own base tables and state store:

- build the initial image;
- execute the auxiliary and fallback paths;
- apply a delta batch of a requested size;
- rebuild one partition and the complete state; and
- invalidate the state.

The runner uses warmups and median wall-clock samples. It fits the complete
shared-interface delta path, including state materialization, the maintenance
kernel, output refresh, and publication preparation. The fitted intercept is
stored in `apply_delta_overhead_us`, the per-row slope in
`apply_delta_per_row_us`, and the compatibility field
`apply_delta_base_us` is set to zero so the intercept is not charged twice by
the selector. The result is written as flat JSON consumed by
`LoadAuxCostModelJson`.

The engine adapter should seed calibration from its existing query-specific
configuration so thresholds and confidence parameters survive calibration.
Only the measured timing fields are replaced. This keeps calibration from
silently changing the query module's correctness or safety limits.

`DuckDBAuxBackend` accepts a `CostModelCallback`; when omitted it reads
`ACCELERATOR_CODEGEN_DUCKDB_AUX_COST_MODEL`. The PostgreSQL SQL-callable
runtime reads `ACCELERATOR_CODEGEN_POSTGRES_Q10_COST_MODEL` or
`ACCELERATOR_CODEGEN_POSTGRES_Q24_COST_MODEL` when it constructs the backend.
Missing files intentionally fall back to default parameters.

The maintenance selector also consumes the adapter's estimate of queries
before the next write. DuckDB's shared benchmark supplies this value from its
configured reader rate and write interval; standalone adapters can provide a
`WorkloadCallback`, or use
`ACCELERATOR_CODEGEN_DUCKDB_EXPECTED_QUERIES_BEFORE_WRITE` and
`ACCELERATOR_CODEGEN_POSTGRES_EXPECTED_QUERIES_BEFORE_WRITE` respectively.
