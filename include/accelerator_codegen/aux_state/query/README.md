# Engine-agnostic auxiliary queries

The query tree is split by workload: `tpch/` and `sec_edgar/`. DuckDB
production paths use the legacy per-query typed modules while they are
migrated onto `core::AuxQueryModule`.

Shared modules follow the Q10/Q24 pattern:

- query-specific typed initial scans (`TpchQ10CustomerInput`, etc.)
- query-owned typed state in `StateImage::payload`
- `AuxRow` only at the output sink boundary
- per-query codecs only where a backend needs byte serialization (PostgreSQL)

The backend adapters own scanning, visibility, transition-row capture, and
state publication. DuckDB keeps typed state in memory; PostgreSQL encodes
state only at its shared-memory serialization boundary.

Decision CSV logging for maintenance calibration lives in
`maintenance/maintenance_decision_csv.cpp` and is enabled via per-query
`ACCEL_Q*_DECISION_CSV` environment variables.
