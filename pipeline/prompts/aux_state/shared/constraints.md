# Aux-state project constraints

Multi-connection production path with **single-threaded per connection** (`PRAGMA threads=1`). DuckDB native storage unchanged.

- **Persistent partitioned aux state is allowed** for this workload kind.
- State is **per-query** (`{{aux_function_name}}`); no cross-query state sharing required, but accelerators must remain correct when other queries' writes run concurrently (see `concurrency_contract.md`).
- **Fallback required**: when aux state is missing, stale, or txn-blocked, the base kernel (`{{base_function_name}}`) must produce correct results.
- Txn-safe invalidation and staged publication are mandatory.
- Irrelevant mutations must be **NoOp** — no epoch bump, no stale marks, no maintenance side effects.
- Read paths must be **bounded to the query window** — do not scan all partition slots.
- Do not alter query semantics for implementation convenience.
- Benchmark fairness: time full `connection.Query()`, consume all results, `PRAGMA threads=1`.
- TPC-H aux: iteration on SF1; state and read paths must scale to SF20+ (partition bounds, no SF1 slot counts).

See `profiling_conventions.md` for profile API rules. See `scale_factor_contract.md` for iteration vs production scale.
