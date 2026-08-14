# Generated code layout contract

Agent-produced query code lives under `src/generated/`. Add `src/generated` to the include path and use short includes:

```cpp
#include "tpch/q16/base/query.hpp"
#include "tpch/q1/aux/module.hpp"
#include "catalog/query_catalog.hpp"
```

Hand-written scaffolding stays in `include/accelerator_codegen/`:

| Directory | Purpose |
|-----------|---------|
| `runtime/` | DuckDB/Postgres extension host, scan adapter, register |
| `stateful/` | Aux-state substrate (`core/`, `duckdb/`, `postgres/`, `maintenance/`) |
| `harness/` | Differential + calibration helpers |

## Per-query tree

```
src/generated/{tpch|sec_edgar}/q{N}/
  base/
    query.hpp              # bind/kernel API (symbol prefix still tpch_qN / sec_edgar_qN)
    fused.hpp              # optional fused-scan API
    single-threaded/       # kernel.cpp, package.cpp, fused_scan.cpp
    parallel/              # optional parallel kernel.cpp, package.cpp, aligned_agg.hpp
  aux/
    module.hpp, module.cpp
    finalize.hpp, finalize.cpp   # optional payload helpers
```

Shared generated glue (append only — do not split per query):

- `src/generated/catalog/query_catalog.{hpp,cpp}` — engine-neutral query dispatch
- `src/generated/postgres/kernel_runner.{hpp,cpp}`, `runtime.{hpp,cpp}` — Postgres extension
- `src/generated/sec_edgar/runtime.{hpp,cpp}` — SEC fused-scan runtime hook

## Base kernel files (agents generate)

| Role | Path |
|------|------|
| Public API | `src/generated/{benchmark}/q{N}/base/query.hpp` |
| Kernel | `src/generated/{benchmark}/q{N}/base/single-threaded/kernel.cpp` |
| DuckDB package | `src/generated/{benchmark}/q{N}/base/single-threaded/package.cpp` |
| Fused scan (optional) | `.../fused_scan.cpp` + `base/fused.hpp` |
| Tests | `tests/single-threaded/test_{benchmark}_q{N}.cpp` |
| Benchmark | `benchmarks/{benchmark}/single-threaded/benchmark_q{N}.cpp` |

**Do not edit** `src/runtime/duckdb/register.cpp` — registration is wired from scaffold snippets.

`CMakeLists.txt` is editable for CMake target registration when auto-discovery is insufficient; follow `cmake/AuxState.cmake` and existing query blocks.

## Aux-state files (agents generate)

| Role | Path |
|------|------|
| Module API | `src/generated/{benchmark}/q{N}/aux/module.hpp` |
| Module impl | `src/generated/{benchmark}/q{N}/aux/module.cpp` |
| Finalize helpers | `src/generated/{benchmark}/q{N}/aux/finalize.{hpp,cpp}` |
| Cost model | `config/q{N}_aux_cost_model.json` (TPC-H) or `config/sec_edgar_q{N}_aux_cost_model.json` |
| Tests / bench | `tests/single-threaded/test_{benchmark}_q{N}_aux.cpp`, `benchmarks/.../benchmark_q{N}_aux.cpp` |

CMake: `accel_add_aux_state_query(BENCHMARK {benchmark} QUERY_NUM N ...)` in `cmake/AuxState.cmake`.

## Naming vs paths

Workload YAML still uses symbol prefixes like `tpch_q14`, `accel_tpch_q14`, `CreateTpchQ14FunctionSet`. Those are **API names**, not header filenames. The header file is always `base/query.hpp`.

## Reference implementations

| Pattern | Read first |
|---------|------------|
| Base kernel | `src/generated/tpch/q1/base/single-threaded/kernel.cpp` |
| Fused scan | `src/generated/tpch/q1/base/single-threaded/fused_scan.cpp`, `base/fused.hpp` |
| SEC runtime hook | `src/generated/sec_edgar/runtime.cpp` |
| Aux module (single partition) | `src/generated/tpch/q14/aux/module.cpp` |
| Aux module (prefix/window) | `src/generated/tpch/q1/aux/module.cpp`, `q10/aux/module.cpp` |
| Postgres byte encoding | `include/accelerator_codegen/stateful/core/byte_stream.hpp` (boundary only) |

See also `pipeline/prompts/shared/engine_interfaces.md` for DuckDB vs Postgres minimal surfaces.
