# Shared project constraints

See also `hard_guidelines.md` (native base storage, fair benchmarks, scale-safe profile-guided optimization).

Single-threaded production path per connection unless the workload explicitly documents otherwise.

- **`engine_name` is required** — never default to `duckdb` or `postgres`. Emit kernels under `src/generated/{benchmark}/q{N}/{engine}/`.
- Base tables: use **this engine’s** native scan/storage APIs. Do not bypass with unfair client SQL on the hot path. Do **not** assume a shared `DataChunk` / `ScanNativeBaseTable` portable IR.
- Aux state (when applicable): custom storage is allowed and preferred; do **not** cache final query answers / precomputed final results as aux. Intermediate aux structures may be redesigned when speedups stall. Aux modules still speak shared `MutationBatch` / `AuxQueryModule` / `AuxStateStore`.
- Base (non-aux) kernels: no persistent auxiliary state, materialized views, or accelerator-specific indexes across executions; transient state rebuilt per execution. Base consume is **engine-local**.
- Preserve generic fallbacks for unexpected layouts, nulls, and transaction-local rows.
- Do not alter query semantics for implementation convenience.
- Benchmark fairness: time the full production query path, consume all results; enforce single-threaded execution where the harness requires it (e.g. DuckDB `PRAGMA threads=1` when `engine_name=duckdb`).

TPC-H: pipeline iterates on **SF1**; production targets **SF20+** with **high speedups required at high SF**, not only on the iteration DB. See `scale_factor_contract.md` — no SF1-hardcoded capacities or full-domain work that does not scale.

See `profiling_conventions.md` for profile API rules.
