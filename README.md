# accelerator-codegen

Generates query accelerators for SQL engines. For each query of a workload
(TPC-H, SEC-EDGAR), an LLM agent pipeline (`pipeline/`) writes an accelerator
that keeps versioned auxiliary state next to the engine's tables, maintains it
incrementally under writes, and answers the query from it, checked against the
engine's own SQL. The pipeline writes only the per-query layer; the substrate,
engine backends and shared primitives it builds on are fixed or grow through
gated tools. Verus accelerators (`--mode verus`) are the same idea with a
formally verified kernel against a spec generated from the SQL.

## Layers

`substrate/` includes nothing from `backends/` or `accelerators/`, and `backends/` includes nothing from `accelerators/`
(`scripts/check_layer_includes.py`).

| Layer | Directory | Who writes it |
|---|---|---|
| Substrate: engine-agnostic aux-state core, state store, maintenance policy | `substrate/aux/{core,state,maintenance}` | people; read-only to the pipeline |
| Substrate: Verus libraries, host kit, SQL-to-Verus-spec generator | `substrate/verus/{common,store,kit,sql2verus}` | people; read-only to the pipeline |
| Engine backends (DuckDB, PostgreSQL, MySQL): generic registry and dispatch, no query named | `backends/<engine>/{state,engine,api,tests}` | the `engine_backend` pipeline run (or people) |
| Shared primitives (copy-on-write containers, SQL value semantics) and the Verus proof library | `accelerators/prims`, `accelerators/prooflib` | pipeline tools `primitive_publish`, `prooflib_publish` (additions only) |
| Per-query accelerators | `accelerators/<bench>/q<N>/` | the query pipeline runs |
| SQL workloads | `workloads/<bench>/{schema.sql,q<N>.sql}` | people |
| Evaluation: differential harness, tests, comparison and benchmark drivers | `eval/{harness,tests,compare,bench}` | people |
| Pipeline: orchestrator, prompts, gates, config, artifacts | `pipeline/` | people |

Query runs never write `backends/`; when a query needs something from a backend
they record a request (`backend_addition_record`) for an `engine_backend` run.

## Directory map

```
substrate/aux/        core/ (cost model, calibration, registry), state/ (versioned store, footprint),
                      maintenance/ (action selector), tests/
substrate/verus/      common/, store/ (crate verus_substrate), kit/ (host side), sql2verus/
backends/<engine>/    state/ engine/ api/ tests/ bench/   (api/catalog = the query registry)
accelerators/prims/   generic data-structure primitives (README.md catalogue)
accelerators/prooflib/ shared Verus proof library
accelerators/<bench>/ common/, verus_world/, q<N>/
  q<N>/               cost_model.hpp  layout.hpp  <engine>/{base,aux}/  verus_kit/<crate>/
                      config/[verus/]  tests/  bench/
workloads/<bench>/    schema.sql, q<N>.sql   (tpch, sec_edgar)
eval/                 harness/ tests/ compare/ bench/
pipeline/             see pipeline/README.md
scripts/              build, calibration, data-loading and layout-check scripts
cmake/                CMake modules
```

Headers sit next to their sources; there is no `include/` tree. Include roots
are the repository root and `accelerators/` (for example
`#include "substrate/aux/core/aux_cost_model_path.hpp"`).

## Build

DuckDB (the fork is found as a sibling checkout, `../accelerator-codegen-duckdb`):

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DACCEL_CODEGEN_ENGINE=duckdb -DACCEL_ENABLE_PARALLEL_KERNELS=ON -DBUILD_TESTING=ON
cmake --build build
```

`ACCEL_ENABLE_PARALLEL_KERNELS` builds the multithreaded TPC-H variants.
`ACCEL_AUX_COST_MODEL_DIR` (runtime env) names a subdirectory of each query's
`config/` to prefer when looking up cost models.

PostgreSQL and MySQL are built by scripts that configure the matching CMake targets:

- `scripts/build_accel_postgres_extension.sh` (module `accelerator_codegen_pg`; the
  test-only module `accelerator_codegen_pg_test` is built when a requested target needs it)
- `scripts/build_accel_mysql_plugin.sh` (production plugin; `--test` builds the test variant
  with the gate-test UDFs, `-DACCEL_MYSQL_TEST_UDFS=ON`)

Verus kits (needs the Verus toolchain under `tools/verus`, or `VERUS_BIN`):

```bash
scripts/build_verus_crate.sh <crate_dir> <name> [--verify-only | --rlib | ...]   # one kit
scripts/build_verus_world.sh <bench>                                              # all kits of a bench plus the one global store
```

Output goes to `build-verus/` (`ACCEL_VERUS_LIB_DIR`); CMake links each kit
whose library exists.

## Layout checks

```bash
python3 scripts/check_backend_query_free.py     # backends name no query
python3 scripts/check_layer_includes.py         # include direction between layers
cd pipeline/orchestrator && pytest tests/test_substrate_never_writable.py
```

## Running tests and binaries

Run test and benchmark binaries from the repository root: cost-model paths
(`accelerators/<bench>/q<N>/config/...`) are relative to the working directory.

Verus kit correctness (differential + concurrent snapshots, per kit, from the
repo's own inputs: `accelerators/tpch/q<N>/tests/verus_workload/` and
`workloads/tpch/schema_constraints.sql`):

```bash
TEST_DB=<tpch_test.duckdb> scripts/run_verus_kit_tests.sh <build_dir> [queries]
python3 scripts/gen_verus_kit_params.py [--check]   # params.sql of every kit + the schema constraints
```

See `pipeline/README.md` for the agent pipeline, `accelerators/prims/README.md` for
the primitives catalogue, and `substrate/aux/core/README.md` for the maintenance
and concurrency design.
