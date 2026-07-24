# Shared project constraints

Single-threaded production path only. Use DuckDB native storage unchanged.

- No persistent auxiliary state, materialized views, or accelerator-specific indexes.
- No state cached across query executions; transient state rebuilt per execution.
- Preserve generic fallbacks for unexpected vector layouts, nulls, and transaction-local rows.
- Do not alter query semantics for implementation convenience.
- Benchmark fairness: time full `connection.Query()`, consume all results, `PRAGMA threads=1`.

See `profiling_conventions.md` for profile API rules.
