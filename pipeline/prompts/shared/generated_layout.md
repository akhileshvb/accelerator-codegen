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
    single-threaded/register.cpp   # duckdb: BaseFunctionSetRegistrar for this query's table function
  aux/
    module.hpp, module.cpp
    finalize.hpp, finalize.cpp   # optional payload helpers
    register.cpp           # duckdb: catalog entry + table function + binding (registrars)
src/generated/{tpch|sec_edgar}/q{N}/layout.hpp   # the query's state layout (slot ids), shared by its engines
```

## Registration: the query registers itself (backends name no query)

Every engine backend exposes **registries**; a query plugs in with static registrars
from **its own directory**. Adding a query never edits a backend file, a catalog
array, a dispatch switch, or a function list; a tree with every
`src/generated/<bench>/q<N>/` removed still builds the backends.

| Engine | The query's registration files | Backend registry (generic) |
|---|---|---|
| duckdb | `q{N}/duckdb/aux/register.cpp`: catalog entry (`SharedQueryRegistrar`) + `AuxQueryRegistrar` (table function, argument types, `MakeBinding<Scan, Module>` with the query's InitialScan); `q{N}/duckdb/base/single-threaded/register.cpp`: `BaseFunctionSetRegistrar{&Create…FunctionSet}` (parallel variant in `base/parallel/register.cpp`); fused base scans via `QueryPackage::create_fused_kernel` / `fused_scan` in `package.cpp` | `catalog/duckdb/query_catalog.hpp`, `runtime/duckdb/aux_query_registry.hpp` (+ `scan::` helpers), `runtime/duckdb/register.hpp` |
| postgres | `q{N}/postgres/aux/catalog.cpp` (`PostgresQueryCatalogRegistrar`); `q{N}/postgres/extension/scan.cpp` (SPI scans, scan class, runtime, argument decoding; `PostgresQueryExecutorRegistrar`, variant `aux`); `q{N}/postgres/extension/functions.cpp` (`PG_FUNCTION_INFO_V1` shims calling `ExecutePostgresQuery(bench, N, variant, fcinfo)` + the query's `CREATE FUNCTION` DDL via `PostgresQuerySqlRegistrar`); `q{N}/postgres/base/base.cpp` (variant `base`) | `catalog/postgres/query_catalog.hpp`, `src/generated/postgres/runtime.hpp` (+ `engine_helpers.hpp`, `engine_runtime.hpp`); `SELECT accel_postgres_register_queries();` creates every registered query's SQL functions |
| mysql | `q{N}/mysql/aux/catalog.cpp` (`MysqlQueryCatalogRegistrar`); `q{N}/mysql/plugin/runtime.cpp` (scans + `MysqlUdfRegistrar{MysqlUdfEntry{name, arg_types, execute}}`); `q{N}/mysql/plugin/udf.cpp` (`ACCEL_MYSQL_QUERY_UDF(<name>)`: the UDF symbols); `q{N}/mysql/plugin/functions.sql` (its `CREATE FUNCTION`) | `catalog/mysql/query_catalog.hpp`, `src/generated/mysql/runtime.hpp`, `src/generated/mysql/udf_shim.hpp`; `scripts/mysql_register_functions.sh` assembles the SQL |
| Verus (any engine the kit supports) | `q{N}/verus_kit/<crate>/`: glue (`kit::Registrar`), C header, kernel crate in `crate/` | `include/accelerator_codegen/verus/verus_kit.hpp` |

Backend files (`src/runtime/{engine}/`, `src/stateful/{engine}/`, `src/generated/{engine}/`,
`src/generated/catalog/{engine}/`) are the engine_backend's. A query may add an
**engine-level** helper there when it needs one (generic: no query names, recorded
as a backend addition), never per-query code.

Multiple engines may coexist under the same `q{N}/` directory (e.g. `q3/duckdb/` and `q3/postgres/`).

**Hard rule for `engine_name: postgres`:** write **only** under
`src/generated/{bench}/q{N}/postgres/{base,aux}/`. Do **not** create or modify the
legacy flat `q{N}/base/` or `q{N}/aux/` trees (those are DuckDB-era). Base TPC-H tables
remain native Postgres storage; aux may use custom per-accelerator storage.

Shared generated glue (per engine; generic, from `engine_backend`):

- `src/generated/catalog/{engine}/query_catalog.{hpp,cpp}` — the catalog registry for that host
- `src/generated/{engine}/` + `runtime|stateful/{engine}/` — registries, generic dispatch, engine helpers
- benchmark-level helpers shared by a benchmark's queries (e.g. `src/generated/sec_edgar/duckdb_probes.hpp`)

## Base kernel files (agents generate)

| Role | Path |
|------|------|
| Public API | `src/generated/{benchmark}/q{N}/{engine}/base/query.hpp` |
| Kernel | `src/generated/{benchmark}/q{N}/{engine}/base/single-threaded/kernel.cpp` |
| Host package | `src/generated/{benchmark}/q{N}/{engine}/base/single-threaded/package.cpp` |
| Fused scan (optional) | `.../fused_scan.cpp` + `base/fused.hpp`; the package sets `create_fused_kernel` / `fused_scan` (duckdb) |
| Registration | the query's registrar file(s), table above |
| Tests | `tests/single-threaded/test_{benchmark}_q{N}.cpp` (or engine-qualified peers) |
| Benchmark | `benchmarks/{benchmark}/single-threaded/benchmark_q{N}.cpp` |

**`engine_name` is required** — never assume `duckdb` or invent a flat `q{N}/base/` path without the engine segment.

**Do not edit** a backend's registration code (duckdb: `src/runtime/duckdb/register.cpp`, the catalog, the table-function file): scaffold writes the query's own `register.cpp`. For other `engine_name` values, register through that backend's registries from the query's directory — do not invent a parallel adapter tree.

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
| Fused scan hook (duckdb) | a peer `package.cpp` setting `create_fused_kernel` / `fused_scan` |
| Registration | a peer's registrar file(s) (table above) |
| Aux module | `src/generated/tpch/q*/{engine}/aux/module.cpp` |
| Serialized boundary helpers | `include/accelerator_codegen/stateful/core/byte_stream.hpp` (only when store mode needs it) |

See also `pipeline/prompts/shared/engine_interfaces.md`.
