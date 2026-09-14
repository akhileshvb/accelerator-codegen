# Generated code layout contract

Agent-produced query code lives under `src/generated/`, **scoped by `engine_name`**. Add `src/generated` to the include path and use short includes:

```cpp
#include "tpch/q16/{{engine_name}}/base/query.hpp"
#include "tpch/q1/{{engine_name}}/aux/module.hpp"
#include "catalog/query_catalog.hpp"  // cmake exposes src/generated/catalog/{{engine_name}}/
```

Hand-written scaffolding stays in `include/accelerator_codegen/`:

| Directory | Purpose |
|-----------|---------|
| `runtime/` | Engine hosts; DuckDB-private scan under `runtime/duckdb/`; other engines under `runtime/{engine}/` |
| `stateful/` | Aux-state substrate (`core/`, `state/`, `maintenance/`) + per-engine adapters |
| `harness/` | Differential + calibration helpers |

Per-engine adapters (`runtime/{engine}/`, `stateful/{engine}/`, optional `src/generated/{engine}/`) are owned by `workload_kind: engine_backend`, not query agents.

There is **no** shared portable `DataChunk` IR. Base kernels are engine-specific; only aux/txn contracts are shared.

## Per-query tree (per engine)

```
src/generated/{tpch|sec_edgar}/q{N}/{engine}/
  base/
    query.hpp              # bind/kernel API (symbol prefix still tpch_qN / sec_edgar_qN)
    fused.hpp              # optional fused-scan API (host-dependent)
    single-threaded/       # kernel.cpp, package.cpp, fused_scan.cpp
    parallel/              # optional parallel kernel.cpp, package.cpp, aligned_agg.hpp
  aux/
    module.hpp, module.cpp
    finalize.hpp, finalize.cpp   # optional payload helpers
```

Multiple engines may coexist under the same `q{N}/` directory (e.g. `q3/duckdb/` and `q3/postgres/`).

**Hard rule for `engine_name: postgres`:** write **only** under
`src/generated/{bench}/q{N}/postgres/{base,aux}/`. Do **not** create or modify the
legacy flat `q{N}/base/` or `q{N}/aux/` trees (those are DuckDB-era). Base TPC-H tables
remain native Postgres storage; aux may use custom per-accelerator storage.

Shared generated glue (per engine — do not collapse into a single engine-neutral catalog):

- `src/generated/catalog/{engine}/query_catalog.{hpp,cpp}` — registration / dispatch for that host
- `src/generated/sec_edgar/runtime.{hpp,cpp}` — SEC fused-scan runtime hook (when used by a duckdb or fused host path)

Host glue from `engine_backend` stays at `src/generated/{engine}/` + `runtime|stateful/{engine}/`.

## Base kernel files (agents generate)

| Role | Path |
|------|------|
| Public API | `src/generated/{benchmark}/q{N}/{engine}/base/query.hpp` |
| Kernel | `src/generated/{benchmark}/q{N}/{engine}/base/single-threaded/kernel.cpp` |
| Host package | `src/generated/{benchmark}/q{N}/{engine}/base/single-threaded/package.cpp` |
| Fused scan (optional) | `.../fused_scan.cpp` + `base/fused.hpp` |
| Catalog | `src/generated/catalog/{engine}/query_catalog.{hpp,cpp}` |
| Tests | `tests/single-threaded/test_{benchmark}_q{N}.cpp` (or engine-qualified peers) |
| Benchmark | `benchmarks/{benchmark}/single-threaded/benchmark_q{N}.cpp` |

**`engine_name` is required** — never assume `duckdb` or invent a flat `q{N}/base/` path without the engine segment.

**Do not hand-edit** the host registration file when scaffold owns the snippet (duckdb: `src/runtime/duckdb/register.cpp`). For other `engine_name` values, register via that backend’s extension/SQL surface from `engine_backend` — do not invent a parallel adapter tree.

`CMakeLists.txt` is editable for CMake target registration when auto-discovery is insufficient; follow `cmake/AuxState.cmake` / `GeneratedSources.cmake` globs under `{engine}` and existing query blocks.

## Aux-state files (agents generate)

| Role | Path |
|------|------|
| Module API | `src/generated/{benchmark}/q{N}/{engine}/aux/module.hpp` |
| Module impl | `src/generated/{benchmark}/q{N}/{engine}/aux/module.cpp` |
| Finalize helpers | `src/generated/{benchmark}/q{N}/{engine}/aux/finalize.{hpp,cpp}` |
| Cost model | `config/q{N}_aux_cost_model.json` (TPC-H) or `config/sec_edgar_q{N}_aux_cost_model.json` |
| Tests / bench | `tests/single-threaded/test_{benchmark}_q{N}_aux.cpp`, `benchmarks/.../benchmark_q{N}_aux.cpp` |

CMake: `accel_add_aux_state_query(BENCHMARK {benchmark} QUERY_NUM N ...)` in `cmake/AuxState.cmake` (engine-aware discovery).

## Naming vs paths

Workload YAML still uses symbol prefixes like `tpch_q14`, `accel_tpch_q14`, `CreateTpchQ14FunctionSet`. Those are **API names**, not header filenames. The header file is always `base/query.hpp` under the `{engine}/` segment.

## Reference implementations

Prefer peers under the **same** `{engine}/` tree when present. Flat `q{N}/base/` paths are legacy and must not be reintroduced.

| Pattern | Read first |
|---------|------------|
| Base kernel | `src/generated/tpch/q*/{engine}/base/single-threaded/kernel.cpp` |
| Fused scan (duckdb) | `.../fused_scan.cpp`, `base/fused.hpp` when present for that host |
| SEC runtime hook | `src/generated/sec_edgar/runtime.cpp` |
| Aux module | `src/generated/tpch/q*/{engine}/aux/module.cpp` |
| Serialized boundary helpers | `include/accelerator_codegen/stateful/core/byte_stream.hpp` (only when store mode needs it) |

See also `pipeline/prompts/shared/engine_interfaces.md`.
