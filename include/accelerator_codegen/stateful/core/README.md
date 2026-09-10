# Shared auxiliary-state calibration

The shared core carries immutable, query-owned state images as typed payloads.
Engine adapters decide store mode:

- **typed_in_process** — publish typed `StateImage` payloads directly (no
  serialize/deserialize on the hot path).
- **serialized_boundary** — store bytes at a process/SHM boundary and decode
  before exposing the same typed `StateSnapshot` to a module (`byte_stream.hpp`).

`aux_calibration.hpp` contains the engine-neutral calibration runner. A backend
supplies callbacks that perform the real operation against its own base tables
and state store:

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

Adapters typically accept a `CostModelCallback` and/or an env var such as
`ACCELERATOR_CODEGEN_{ENGINE}_AUX_COST_MODEL` (see the active host adapter).
Missing files intentionally fall back to default parameters.

The maintenance selector also consumes the adapter's estimate of queries
before the next write (`WorkloadCallback` or an engine-specific expected-queries
env/GUC).
