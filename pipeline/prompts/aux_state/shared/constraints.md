# Aux-state project constraints

Single-threaded production path. DuckDB native storage unchanged.

- **Persistent partitioned aux state is allowed** for this workload kind.
- State is **per-query** (`{{aux_function_name}}`); no cross-query sharing required.
- **Fallback required**: when aux state is missing, stale, or txn-blocked, the base kernel (`{{base_function_name}}`) must produce correct results.
- Txn-safe invalidation and staged publication are mandatory.
- Do not alter query semantics for implementation convenience.
- Benchmark fairness: time full `connection.Query()`, consume all results, `PRAGMA threads=1`.

See `profiling_conventions.md` for profile API rules.
