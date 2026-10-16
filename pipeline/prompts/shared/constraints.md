# Shared project constraints

See also `hard_guidelines.md` (native base storage, fair benchmarks, scale-safe profile-guided optimization).

Reads (`Execute`), builds (`Build`) and maintenance each parallelize on the backend's one thread budget, with one cap per phase, through `services` only (`engine_interfaces.md`, module thread rules; primitives: `substrate_primitives.md`). Work proportional to the data is split; a serial loop over it is a defect at large scale.

- **`engine_name` is required** — never default to `duckdb` or `postgres`. Emit kernels under `accelerators/{benchmark}/q{N}/{engine}/`.
- Base tables: use **this engine’s** native scan/storage APIs. Do not bypass with unfair client SQL on the hot path. Do **not** assume a shared `DataChunk` / `ScanNativeBaseTable` portable IR.
- Aux state (when applicable): custom storage is allowed and preferred; do **not** cache final query answers / precomputed final results as aux. Intermediate aux structures may be redesigned when speedups stall. Aux modules speak the shared `AuxQueryModule` contract; the runtime skeleton owns commit order, versions and builds.
- Base (non-aux) kernels: no persistent auxiliary state, materialized views, or accelerator-specific indexes across executions; transient state rebuilt per execution. Base consume is **engine-local**.
- Preserve generic fallbacks for unexpected layouts, nulls, and transaction-local rows.
- Do not alter query semantics for implementation convenience.
- Benchmark fairness: time the full production query path, consume all results; compare against the base engine at an equal core budget (same `taskset`, base-engine threads = the accelerator's thread budget).

TPC-H: pipeline iterates on **SF1**; production targets **SF20+** (benchmarks) and **SF3000** (sizing: ~8 TB RAM) with **high speedups required at high SF**, not only on the iteration DB. See `scale_factor_contract.md` (failure classes to design against and how they are tested) — no SF1-hardcoded capacities or full-domain work that does not scale.

See `profiling_conventions.md` for profile API rules.
