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
| Substrate: engine-agnostic aux-state core, state store, maintenance policy, runtime | `substrate/aux/{core,state,maintenance,runtime}` | people; read-only to the pipeline |
| Substrate: Verus libraries, host kit, SQL-to-Verus-spec generator | `substrate/verus/{common,store,kit,sql2verus}` | people; read-only to the pipeline |
| Engine backends (DuckDB, PostgreSQL, MySQL): generic registry and dispatch, no query named | `backends/<engine>/{state,engine,api,tests}`; engine-specific eval drivers in `tests/harness`, `bench` | the `engine_backend` pipeline run (or people) |
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
                      maintenance/ (action selector), runtime/ (engine slots, runtime host), tests/
substrate/verus/      common/, store/ (crate verus_substrate), kit/ (host side), sql2verus/
backends/<engine>/    state/ engine/ api/ tests/ bench/   (api/catalog = the query registry)
accelerators/prims/   generic data-structure primitives (README.md catalogue)
accelerators/prooflib/ shared Verus proof library
accelerators/<bench>/ common/, verus_world/, q<N>/
  q<N>/               cost_model.hpp  layout.hpp  <engine>/{base,aux}/  verus_kit/<crate>/
                      config/[verus/]  tests/  bench/
workloads/<bench>/    schema.sql, q<N>.sql   (tpch, sec_edgar)
eval/                 harness/ tests/ compare/ bench/
pipeline/             orchestrator/ config/ prompts/ gates/ templates/ verus/ clean_tree/
scripts/              build, calibration, data-loading and layout-check scripts
cmake/                CMake modules
```

Headers sit next to their sources; there is no `include/` tree. Include roots
are the repository root and `accelerators/` (for example
`#include "substrate/aux/core/aux_cost_model_path.hpp"`).

## Build

DuckDB (needs the capture fork, branch `skeleton/db-capture` with `DatabaseCaptureRegistry`, `OnCommitWithContext` and `ActiveWriterStarts`; found as the sibling checkout `../duckdb-dbcapture`, or set `ACCEL_DUCKDB_ROOT`):

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DACCEL_CODEGEN_ENGINE=duckdb -DACCEL_ENABLE_PARALLEL_KERNELS=ON -DBUILD_TESTING=ON
cmake --build build
```

`ACCEL_ENABLE_PARALLEL_KERNELS` builds the multithreaded TPC-H variants.
`ACCEL_AUX_COST_MODEL_DIR` (runtime env) names a subdirectory of each query's
`config/` to prefer when looking up cost models.

Every backend, whatever the engine, is built, started and loaded through its own
`backends/<engine>/host.sh` (`build`, `up`, `load`, ...; see
`pipeline/prompts/engine_backend/host_contract.md`), which the engine_backend pipeline generates.

Verus kits (needs the Verus toolchain under `tools/verus`, or `VERUS_BIN`):

```bash
scripts/build_verus_crate.sh <crate_dir> <name> [--verify-only | --rlib | ...]   # one kit
scripts/build_verus_world.sh <bench>                                              # all kits of a bench plus the one global store
```

Output goes to `build-verus/` (`ACCEL_VERUS_LIB_DIR`); CMake links each kit
whose library exists.

## Concurrent pipelines

Pipelines that run at once on one host (a pack, or separate trees) share it through `ConcurrencyConfig` (the `concurrency:` block of
`pipeline/config/default.yaml` overrides its defaults). Each run claims one of `workers` disjoint CPU sets (a host-wide flock slot) and
pins itself to it, so its agent, builds (`ACCEL_BUILD_JOBS`, `ACCEL_CTEST_JOBS`, `CMAKE_BUILD_PARALLEL_LEVEL`),
gates and engine instances stay inside it. Benchmarks (the bench, baseline, profile, sf10 and calibration gates,
`run_quick_benchmark` at the acceptance tier) queue first come first served for the host-wide measurement lease
and run on the reserved measurement CPUs, which no worker uses; the quick tier runs without the lease and says
when the host is loaded. Locks live in `/dev/shm/accel-pipeline-<uid>` (`ACCEL_HOST_LOCK_DIR`), so they span every
tree on the host, and they are flocks: a crashed holder releases them. `ACCEL_CONCURRENT_WORKERS` and
`ACCEL_MEASURE_CORES` override the config; `ACCEL_MEASURE_LEASE=0` turns the lease off.

## Layout checks

```bash
python3 scripts/check_backend_query_free.py     # backends name no query
python3 scripts/check_layer_includes.py         # include direction between layers
cd pipeline/orchestrator && pytest tests/test_substrate_never_writable.py
```

The write policy (`write_ownership.py`) is enforced on agent steps: codex runs under a bubblewrap
sandbox (`sandbox:`, default `bwrap`) that mounts `substrate/` and `workloads/`
read-only, and after every stage the paths it changed are checked against each layer's owner
(`write_policy: enforce | warn | off`).

## Running tests and binaries

Run test and benchmark binaries from the repository root: cost-model paths
(`accelerators/<bench>/q<N>/config/...`) are relative to the working directory.

The per-query test suite (`accelerators/<bench>/q<N>/tests/workload/`: fixed tuples, a parameter
domain, write transactions, a write mix; written by the pipeline's `test_suite` stage and frozen after it)
is one format for the C++ and the Verus pipelines (`pipeline/prompts/shared/test_suite.md`). The C++ gates
(`aux_correctness`, `quick_check`) run it with a fresh seed each time, printed and kept in the gate result;
`ACCEL_SUITE_SEED=<seed>` repeats a run.

Verus kit correctness (differential + concurrent snapshots, per kit, from the
suite and `workloads/tpch/schema_constraints.sql`):

```bash
TEST_DB=<tpch_test.duckdb> scripts/run_verus_kit_tests.sh <build_dir> [queries]
python3 scripts/gen_verus_kit_params.py [--check]   # params.sql of every kit + the schema constraints
```

## Running the pipeline

Needs Python 3.11+, the [Codex CLI](https://github.com/openai/codex) on `PATH`, and `OPENROUTER_API_KEY` (in the
environment or in `.env`, copied from `.env.example`). Stages, models and presets are in `pipeline/config/models.yaml`.

```bash
cd pipeline/orchestrator && pip install -e ".[dev]"      # installs `accel` (also: python -m accel_pipeline)

accel check tpch_q14_aux                                 # preflight, ~30 s, no model call; --models also probes each model
accel smoke                                              # happy path with a scripted agent, minutes, no model call
accel smoke --adversarial                                # every guardrail case (--case NAME for one), ~10 min
accel canary tpch_q14_aux --models models.yaml           # real models, fresh sandboxed clean tree, ~20-30 min, ~$1
accel run tpch_q3_aux --engine <engine>                    # one query end to end in this tree
accel status <run dir>                                   # stage, last events, cost so far
accel timeline <run dir>                                 # where the time and the money went
accel run tpch_q21_aux --mode verus                      # Verus pipeline for one query
accel run engine_<engine>                                # engine backend onboarding
python -m accel_pipeline.pack --pack tpch_aux_parallel  # many runs, one worker each
```

- `accel run WORKLOAD` runs analysis, test_suite, implementation, optimization and audit. `--stage S` runs one
  stage, `--until S` pauses after stage S (`--resume` continues with the next), `--resume` continues a stopped or
  paused run, `--dry-run` renders prompts without a model, `--models FILE` and `--model STAGE=PRESET` choose
  models, `--mode verus` runs the Verus pipeline. `--query` defaults to the workload's query; an engine backend is
  onboarded with `accel run engine_<engine> --query 0`. Run state, events and results go to
  `pipeline/artifacts/<bench>/q<N>/<engine>/`; `accel_pipeline.run_pack` runs many queries, one worker each.
- `accel canary` builds the tree under `$ACCEL_CANARY_HOME/runs/<time>` (or `--work DIR`), runs the pipeline in the
  real sandbox with the models file and writes `TIMELINE.md` in that directory: read it before a big run. `--until S`
  stops after a stage, `--resume --work DIR` continues that run with the pipeline of `--rev`. `accel status` and
  `accel timeline` take that directory (or any tree).
- Before a real run, `accel check`, `accel smoke --adversarial` and a canary must be green on the pipeline commit
  the run records. The checks, in the sandboxes the run uses: `perf stat` returns counters; the codex command line
  carries the configured window and compaction settings; the MCP server serves every stage's tools and answers
  parallel data-tool calls; gate scripts do not paste log text into Python source; the test database exists or can
  be built; the thread-sanitizer build dir is writable; every stage prompt renders with only existing paths.
  `accel run` repeats the preflight before any model call. Each adversarial case has a scripted agent misbehave in
  one way (rebuild-on-write, a serial build, a data race, no quick_check, a dismissed check, a false blocker, no
  edits, ...) and must be stopped by its guardrail; `xfail` marks in `pipeline/orchestrator/tests/smoke/fixtures/cases.json`
  are guardrails not built yet. The happy path fails on any stage, gate or repair round that did not go clean.
- `accel smoke` and `accel canary` run from a source checkout (their fixtures are not in clean trees). Add a
  workload or engine as fixture data next to them; the known-good accelerator is taken from the revision under test.
  `ACCEL_HOST_LOCK_DIR` should name the lock directory of the real runs
  so benchmarks queue behind theirs.

The primitives catalogue is `accelerators/prims/README.md`; the runtime contracts backends implement are in
`substrate/aux/runtime/README.md`.
