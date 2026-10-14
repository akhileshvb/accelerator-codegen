# Generated code layout contract

The repository has one directory per layer, and each layer has one write rule:

| Layer | Path | Who writes it |
|---|---|---|
| Substrate | `substrate/**` (`aux/{core,state,maintenance}`, `verus/{common,store,kit,sql2verus}`) | **nobody** (read-only to every agent) |
| Engine backend | `backends/{engine}/**` (`state/`, `engine/`, `api/` incl. `api/catalog/`, `tests/`, `bench/`) | that engine's `workload_kind: engine_backend` run only |
| Query | `accelerators/{bench}/q{N}/**` | that query's run only |
| Primitives library | `accelerators/prims/**` | only through `primitive_publish` |
| Proof library | `accelerators/prooflib/**` | only through `prooflib_publish` |
| Workloads | `workloads/**` (query SQL, schemas) | **nobody** (read-only) |

Agent-produced query code lives under `accelerators/{bench}/q{N}/`, **scoped by `engine_name`**.
The include roots are the repo root and `accelerators/`; use short includes:

```cpp
#include "tpch/q16/{{engine_name}}/base/query.hpp"   // accelerators/ root
#include "tpch/q1/{{engine_name}}/aux/module.hpp"
#include "prims/sorted_count_sum.hpp"               // accelerators/prims/
#include "substrate/aux/core/aux_types.hpp"         // repo root
#include "catalog/query_catalog.hpp"  // cmake exposes backends/{{engine_name}}/api/catalog/
```

Headers sit next to their sources (there is no `include/` tree).

There is **no** shared portable `DataChunk` IR. Base kernels are engine-specific; only aux/txn contracts are shared.

## Per-query tree

Everything a query run owns is under one directory, so one glob covers it:

```
accelerators/{tpch|sec_edgar}/q{N}/
  layout.hpp                 # the query's state layout (slot ids), shared by its engines
  cost_model.hpp             # AuxCostModelLocation k<Bench>Q<N>CostModel{"accelerators/{bench}/q{N}/config", "<stem>"}
  {engine}/
    base/
      query.hpp              # bind/kernel API (symbol prefix still tpch_qN / sec_edgar_qN)
      fused.hpp              # optional fused-scan API (host-dependent)
      single-threaded/       # kernel.cpp, package.cpp, fused_scan.cpp
      single-threaded/register.cpp   # duckdb: BaseFunctionSetRegistrar for this query's table function
      parallel/              # optional parallel kernel.cpp, package.cpp, register.cpp
    aux/
      module.hpp, module.cpp
      finalize.hpp, finalize.cpp     # optional payload helpers
      register.cpp           # duckdb: catalog entry + table function + binding (registrars)
  tests/                     # test_{bench}_q{N}.cpp, test_{bench}_q{N}_aux.cpp, q{N}_manifest.hpp, ...
  bench/                     # benchmark_q{N}.cpp, benchmark_q{N}_aux.cpp, calibrate_q{N}_aux_cost_model.cpp
  config/                    # q{N}_aux_cost_model[_<engine>][_sf<N>].json (sec_edgar: sec_edgar_q{N}_aux_cost_model.json)
    verus/                   # the Verus accelerator's variants (ACCEL_AUX_COST_MODEL_DIR=verus selects them)
  verus_kit/<crate>/         # Verus accelerator: glue, C header, kernel crate
```

Benchmark-level helpers shared by a benchmark's queries live in `accelerators/{bench}/common/`
(e.g. `accelerators/sec_edgar/common/duckdb_probes.hpp`); cross-query evaluation
harnesses (differential drivers, cross-accel helpers, arg variants) live in `eval/`.

## Registration: the query registers itself (backends name no query)

Every engine backend exposes **registries**; a query plugs in with static registrars
from **its own directory**. Adding a query never edits a backend file, a catalog
array, a dispatch switch, or a function list; a tree with every
`accelerators/<bench>/q<N>/` removed still builds the backends.

| Engine | The query's registration files | Backend registry (generic) |
|---|---|---|
| duckdb | `q{N}/duckdb/aux/register.cpp`: catalog entry (`SharedQueryRegistrar`) + `AuxQueryRegistrar` (table function, argument types, `MakeBinding<Scan, Module>` with the query's InitialScan); `q{N}/duckdb/base/single-threaded/register.cpp`: `BaseFunctionSetRegistrar{&Create…FunctionSet}` (parallel variant in `base/parallel/register.cpp`); fused base scans via `QueryPackage::create_fused_kernel` / `fused_scan` in `package.cpp` | `backends/duckdb/api/catalog/query_catalog.hpp`, `backends/duckdb/api/aux_query_registry.hpp` (+ `scan::` helpers), `backends/duckdb/api/register.hpp` |
| postgres | `q{N}/postgres/aux/catalog.cpp` (`PostgresQueryCatalogRegistrar`); `q{N}/postgres/extension/scan.cpp` (SPI scans, scan class, runtime, argument decoding; `PostgresQueryExecutorRegistrar`, variant `aux`); `q{N}/postgres/extension/functions.cpp` (`PG_FUNCTION_INFO_V1` shims calling `ExecutePostgresQuery(bench, N, variant, fcinfo)` + the query's `CREATE FUNCTION` DDL via `PostgresQuerySqlRegistrar`); `q{N}/postgres/base/base.cpp` (variant `base`) | `backends/postgres/api/catalog/query_catalog.hpp`, `backends/postgres/api/runtime.hpp` (+ `engine_helpers.hpp`, `engine_runtime.hpp`); `SELECT accel_postgres_register_queries();` creates every registered query's SQL functions |
| mysql | `q{N}/mysql/aux/catalog.cpp` (`MysqlQueryCatalogRegistrar`); `q{N}/mysql/plugin/runtime.cpp` (scans + `MysqlUdfRegistrar{MysqlUdfEntry{name, arg_types, execute}}`); `q{N}/mysql/plugin/udf.cpp` (`ACCEL_MYSQL_QUERY_UDF(<name>)`: the UDF symbols); `q{N}/mysql/plugin/functions.sql` (its `CREATE FUNCTION`) | `backends/mysql/api/catalog/query_catalog.hpp`, `backends/mysql/api/runtime.hpp`, `backends/mysql/api/udf_shim.hpp`; `scripts/mysql_register_functions.sh` assembles the SQL |
| Verus (any engine the kit supports) | `q{N}/verus_kit/<crate>/`: glue (`kit::Registrar`), C header, kernel crate in `crate/` | `substrate/verus/kit/verus_kit.hpp` |

Every query ships `accelerators/{bench}/q{N}/cost_model.hpp` (scaffold generates it):
one `AuxCostModelLocation` naming the query's `config/` directory and file stem.
Each engine's registration passes it on: DuckDB as the catalog entry's
`SharedQueryCatalogEntry::cost_model` (last field, `duckdb/aux/register.cpp`),
Postgres and MySQL as the `Runtime` constructor's location argument
(`postgres/extension/scan.cpp`, `mysql/plugin/runtime.cpp`). The substrate picks
the engine and scale-factor variant from it; never hard-code a JSON path. Peer:
`accelerators/tpch/q6/`.

Backend files (`backends/{engine}/**`) belong to that engine's `engine_backend` run.
A query run never writes them (the write is denied): when a query needs an
**engine-level** helper the backend lacks, log a request for the engine_backend
run (Verus pipeline: the `backend_addition_record` tool; C++ pipeline: one JSON line
`{"benchmark", "query", "helper", "reason"}` appended to
`pipeline/artifacts/backend_additions/{engine}.jsonl`). The helper must be generic
(no query names); never put per-query code in the backend, and do not rely on the
helper in the current run.

Multiple engines may coexist under the same `q{N}/` directory (e.g. `q3/duckdb/` and `q3/postgres/`).

**Hard rule for `engine_name: postgres`:** write **only** under
`accelerators/{bench}/q{N}/postgres/{base,aux,extension}/` (plus the query's own
`tests/`, `bench/`, `config/`). Do **not** create or modify flat `q{N}/base/` or
`q{N}/aux/` trees. Base TPC-H tables remain native Postgres storage; aux may use
custom per-accelerator storage.

## Base kernel files (agents generate)

| Role | Path |
|------|------|
| Public API | `accelerators/{benchmark}/q{N}/{engine}/base/query.hpp` |
| Kernel | `accelerators/{benchmark}/q{N}/{engine}/base/single-threaded/kernel.cpp` |
| Host package | `accelerators/{benchmark}/q{N}/{engine}/base/single-threaded/package.cpp` |
| Fused scan (optional) | `.../fused_scan.cpp` + `base/fused.hpp`; the package sets `create_fused_kernel` / `fused_scan` (duckdb) |
| Registration | the query's registrar file(s), table above |
| Tests | `accelerators/{benchmark}/q{N}/tests/test_{benchmark}_q{N}.cpp` (or engine-qualified peers) |
| Benchmark | `accelerators/{benchmark}/q{N}/bench/benchmark_q{N}.cpp` |

**`engine_name` is required** — never assume `duckdb` or invent a flat `q{N}/base/` path without the engine segment.

**Do not edit** a backend's registration code (duckdb: `backends/duckdb/api/register.cpp`, the catalog, the table-function file): scaffold writes the query's own `register.cpp`. For other `engine_name` values, register through that backend's registries from the query's directory — do not invent a parallel adapter tree.

`CMakeLists.txt` is editable for CMake target registration when auto-discovery is insufficient; follow `cmake/AuxState.cmake` / `GeneratedSources.cmake` globs under `{engine}` and existing query blocks.

## Aux-state files (agents generate)

| Role | Path |
|------|------|
| Module API | `accelerators/{benchmark}/q{N}/{engine}/aux/module.hpp` |
| Module impl | `accelerators/{benchmark}/q{N}/{engine}/aux/module.cpp` |
| Finalize helpers | `accelerators/{benchmark}/q{N}/{engine}/aux/finalize.{hpp,cpp}` |
| Cost model | `accelerators/{benchmark}/q{N}/config/q{N}_aux_cost_model.json` (TPC-H) or `.../config/sec_edgar_q{N}_aux_cost_model.json` |
| Cost-model location | `accelerators/{benchmark}/q{N}/cost_model.hpp` (passed to every engine's registration; see above) |
| Tests / bench | `accelerators/{benchmark}/q{N}/tests/test_{benchmark}_q{N}_aux.cpp`, `accelerators/{benchmark}/q{N}/bench/benchmark_q{N}_aux.cpp` |

CMake: `accel_add_aux_state_query(BENCHMARK {benchmark} QUERY_NUM N ...)` in `cmake/AuxState.cmake` (engine-aware discovery).

## Naming vs paths

Workload YAML still uses symbol prefixes like `tpch_q14`, `accel_tpch_q14`, `CreateTpchQ14FunctionSet`. Those are **API names**, not header filenames. The header file is always `base/query.hpp` under the `{engine}/` segment.

## Reference implementations

Prefer peers under the **same** `{engine}/` tree when present (read-only: other queries' directories are never writable). Flat `q{N}/base/` paths are legacy and must not be reintroduced.

| Pattern | Read first |
|---------|------------|
| Base kernel | `accelerators/tpch/q*/{engine}/base/single-threaded/kernel.cpp` |
| Fused scan (duckdb) | `.../fused_scan.cpp`, `base/fused.hpp` when present for that host |
| Fused scan hook (duckdb) | a peer `package.cpp` setting `create_fused_kernel` / `fused_scan` |
| Registration | a peer's registrar file(s) (table above) |
| Aux module | `accelerators/tpch/q*/{engine}/aux/module.cpp` |
| Serialized boundary helpers | `substrate/aux/core/byte_stream.hpp` (only when store mode needs it) |

See also `pipeline/prompts/shared/engine_interfaces.md`.
