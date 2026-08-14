# Shared project constraints

Single-threaded production path only. Use DuckDB native storage unchanged.

- No persistent auxiliary state, materialized views, or accelerator-specific indexes.
- No state cached across query executions; transient state rebuilt per execution.
- Preserve generic fallbacks for unexpected vector layouts, nulls, and transaction-local rows.
- Do not alter query semantics for implementation convenience.
- Benchmark fairness: time full `connection.Query()`, consume all results, `PRAGMA threads=1`.

TPC-H: pipeline iterates on **SF1**; production targets **SF20+**. See `scale_factor_contract.md` — no SF1-hardcoded capacities or full-domain work that does not scale.

See `profiling_conventions.md` for profile API rules.
