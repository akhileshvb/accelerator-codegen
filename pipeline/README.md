# Accelerator codegen agent pipeline

Four-stage pipeline for generating TPC-H query accelerators:

**Analysis → Implementation → Optimization (loop) → Audit**

## Prerequisites

- Python 3.11+
- API keys in environment or repo-root `.env` (copy from `.env.example`)
- `OPENROUTER_API_KEY` and the [Codex CLI](https://github.com/openai/codex) on `PATH` (the only agent harness)
- Release build at `build-release/` (for gate scripts)
- DuckDB databases in `accelerator-codegen-duckdb`

## Environment file

```bash
cp .env.example .env
# edit .env — keys are loaded automatically when you run the pipeline
```

`.env` is gitignored; `.env.example` is the committed template.

## Install orchestrator

```bash
cd pipeline/orchestrator
pip install -e ".[dev]"
```

## Run (full pipeline)

```bash
export OPENROUTER_API_KEY=sk-or-...
python -m accel_pipeline.run_pipeline --query 3 --workload tpch_q3
```

## Run a single stage

```bash
python -m accel_pipeline.run_pipeline --query 3 --workload tpch_q3 --stage implementation
```

## Resume

```bash
python -m accel_pipeline.run_pipeline --query 3 --workload tpch_q3 --resume
```

## Dry run (orchestration only, no LLM calls)

```bash
python -m accel_pipeline.run_pipeline --query 3 --workload tpch_q3 --dry-run
```

## Layout

This directory (`pipeline/`) is the agent layer. It writes into the rest of the
repository (see the layer table in the top-level `README.md`), and the
substrate is read-only to it.

- `config/` — orchestrator and model settings, workload YAML
- `prompts/` — stage prompts and shared constraints
- `gates/` — deterministic gate scripts (ctest, benchmarks, fairness); the backend slot gates (`run_slot_gates.sh`, per-engine adapters) in `gates/README_runtime_gates.md`
- `templates/scaffold/` — per-query stubs (kernel, package, the query's `register.cpp`) and CMake snippet
- `artifacts/` — per-query pipeline state and reports
- `orchestrator/` — Python package (`accel_pipeline`)
- `verus/` — shared Verus state: the playbook (`playbook/`) and provenance logs
- `codex/` — committed Codex config template and MCP launcher (see Codex exec)
- `catalog/`, `schemas/` — catalog data, JSON schemas

### Layers and who may write them

| Layer | Directory | Writer |
|-------|-----------|--------|
| Substrate | `substrate/aux/{core,state,maintenance,runtime}`, `substrate/verus/{common,store,kit,sql2verus}` | nobody in a run (write-denied, `test_substrate_never_writable.py`) |
| Engine backends (slot implementations) | `backends/<engine>/{state,engine,api,tests}` | the `engine_backend` run only |
| Shared primitives / proof library | `accelerators/prims`, `accelerators/prooflib` | `primitive_publish` / `prooflib_publish`, additions only |
| Per-query accelerators | `accelerators/<bench>/q<N>/` | the query's run |
| Workload SQL | `workloads/<bench>/` | people |

### Where a query lives (engine backends name no query)

Everything query-specific is under `accelerators/<bench>/q<N>/`: `cost_model.hpp`,
`layout.hpp`, `config/` (cost-model JSON; `config/verus/` for Verus kits),
`tests/`, `bench/`, and per engine `<engine>/{base,aux}/…` including the query's
**registrars** (catalog entry, SQL entry points, runtime binding), plus
`verus_kit/<crate>/` for Verus accelerators. The engine backends
(`backends/<engine>/{state,engine,api,tests}`; catalogs under
`backends/<engine>/api/catalog`) hold registries and generic dispatch only, so
adding a query edits no shared file and the backends build with every `q<N>/`
removed. Query runs never write `backends/`: when a query needs an engine-level
helper, the run records a request with the `backend_addition_record` tool and an
`engine_backend` run implements it.
`scripts/check_backend_query_free.py` enforces the first property (run by the
`aux_correctness` and `engine_build` gates); `scripts/check_layer_includes.py`
checks include direction between layers; `scripts/run_duckdb_registry_checks.sh`
runs the DuckDB accelerators end to end. Include roots are the repository root
and `accelerators/`; headers sit next to their sources.

## Shared primitives library (`accelerators/prims/`)

Accelerator state is versioned copy-on-write and built from the generic, workload-agnostic
primitives in `accelerators/prims` (catalogue: its `README.md`).
The library grows through the `primitive_publish` tool (aux_state agents), the C++ analogue of
the Verus `prooflib_publish`: the agent stages the new header, the updated README and
`accelerators/prims/tests/test_prims.cpp` under `pipeline/prims_staging/<repo path>`; under an exclusive
lock (`pipeline/.locks/prims.lock`) the tool checks additions-only, header comment / README row /
tests, and the workload-vocabulary check (`scripts/check_backend_query_free.py`), writes into the
primary tree with a backup, builds and runs `test_prims` (and every target that includes a
changed existing header), restores on failure, and appends `pipeline/prims_provenance.jsonl`.
Direct writes to those paths are denied (`write_ownership.py`); `refresh_prims` mirrors the
primary's copy into each worker worktree at stage start and after a publish; worktree merge-back
never copies `prims/`. Gate `prims_tests` (`pipeline/gates/run_prims_tests.sh`) builds and runs
`test_prims`, checks the vocabulary and that the library only grew relative to HEAD.

## Codex exec (the agent harness)

Every agent step runs `codex exec` against OpenRouter, with Codex's native
edit/search/shell plus an MCP server (`accel_pipeline`, `orchestrator/accel_pipeline/mcp_server.py`)
for gates, profile digests, A/B trials and the other pipeline tools.

- Stages, models and presets: [`config/models.yaml`](config/models.yaml) (`stage_providers` must name
  every stage; an unlisted stage is an error). `--models-config` selects another file.
- `CODEX_HOME` (`pipeline/.codex/`, gitignored; per-worker homes under `pipeline/.codex/workers/`)
  holds the OpenRouter model provider and the MCP server definition. Missing, it is created from
  [`codex/config.template.toml`](codex/config.template.toml); the MCP launcher is
  [`codex/run_mcp_server.sh`](codex/run_mcp_server.sh).
- Hard guidelines for agents: `prompts/shared/hard_guidelines.md`; Codex tooling guide:
  `prompts/shared/codex_tooling.md`.

### Concurrent pack (shared tree, no stage barriers)

Run several query pipelines in parallel on one checkout. Isolation is **separate build dirs** + **per-worker `CODEX_HOME`** + a **shared-file lease** for CMake and engine-backend edits (queries register from their own `accelerators/<bench>/qN/**`, which needs no lease). Workers do not wait on each other's stages.

```bash
cd pipeline/orchestrator && source .venv/bin/activate
accel-pipeline-pack --pack tpch_aux_parallel
# or: python -m accel_pipeline.run_pack --pack tpch_aux_parallel ...
```

Pack YAML examples:
- [`config/packs/tpch_aux_parallel.yaml`](config/packs/tpch_aux_parallel.yaml) — Q2/Q5/Q16
- [`config/packs/tpch_aux_remaining.yaml`](config/packs/tpch_aux_remaining.yaml) — Q4/Q7/Q8/Q13/Q15/Q19/Q20/Q22

Shared path globs: `shared_paths` in [`config/default.yaml`](config/default.yaml). Logs/summary under `pipeline/artifacts/packs/`.

## TPC-H scale factors

| Scale | Role |
|-------|------|
| **SF1** | Pipeline iteration — correctness, baseline, profile, optimization A/B |
| **SF10** | Optional `sf10` gate — coarse check that the kernel still runs at ~10× rows |
| **SF20+** | Production benchmarking target — agents must design for this even though the pipeline does not gate on it by default |

See `pipeline/prompts/shared/scale_factor_contract.md`. Optional manual check:

```bash
ACCEL_TPCH_DB=/path/to/tpch_sf20.duckdb ./build-release/benchmark_tpch_q3
```

## Verus accelerators (`--mode verus`)

Same front end and workload YAML, a different stage pipeline: the accelerator
is a Verus-verified kernel against a spec generated from the SQL (sql2verus),
built from the query alone (no C++ accelerator or other Verus kernel is
visible to the agents). Every agent step runs on codex exec.

```bash
python -m accel_pipeline.run_pipeline --query 21 --workload tpch_q21_aux --mode verus
accel-pipeline-pack --pack tpch_aux_parallel --mode verus
```

Stages (`accel_pipeline/verus/stages.py`): preflight → spec (frozen from the
SQL; faithfulness audit) → analysis (aux state + declared kernel inputs; critic)
→ freeze (obligations, store adapter, C ABI: generated; obligations faithfulness
audit) → tests (adversarial, frozen) → glue (written by an agent; trusted: build,
record-mode contract check against the declared inputs, glue audit; frozen) →
kernel (+ proofs) → integration (frozen tests on
the Verus store; fault self-check of the tests) → optimization (A/B on the Verus
store; stopping in `verus/stopping.py`) → audit (trusted base, leak tripwire,
C++-store compatibility). Settings: `config/verus.yaml`, overridden by a
workload's `verus:` section. Agents work in `pipeline/verus_runs/<workload>/workspace`
(an allowlisted clean room) and build/test only through the `verus_*` MCP tools.
Prompts and contracts: `prompts/verus/`. Shared, growing state: the proof
library (`accelerators/prooflib`) and the playbook (`pipeline/verus/playbook`).
Host side: the generic kit (`substrate/verus/kit/verus_kit.hpp`)
and runner (`eval/tests/verus_kit/verus_kit_runner.cpp`); DuckDB only for now.
A run's output lives in the query's directory: `accelerators/<bench>/q<N>/verus_kit/<crate>/`
(glue + C header; the kernel crate in `crate/`), registered through the kit's registry.

Parameters: every TPC-H substitution parameter (spec 2.4: LIKE patterns and
Q11's fraction included) is a runtime `$n` parameter of the kit (preflight
rejects a query SQL with a literal LIKE pattern or fewer `$n` than the domain
has). Their domains are `eval/compare/gen_tpch_params.py` (its `verus`
argument lists), called by `verus/params.py`. Every benchmark, profile and
gate run draws fresh random tuples there with a new `os.urandom` seed
(`verus_kit_runner` reads them from `VERUS_KIT_PARAMS_FILE` for bench/profile,
`VERUS_KIT_EXTRA_PARAMS_FILE` for differential/snapshots); the seed and tuples
are written to `<artifacts>/param_draws/` and into the run's result JSON, and
are not shown to the agent beforehand. A paired decision (best vs candidate)
shares one draw across both sides and all rounds; each decision, baseline and
audit takes a new one. The frozen `tests/params.sql` also gets random domain
tuples when it is frozen.

Smoke test without an LLM: `ACCEL_VERUS_REPLAY=<dir>` replays agent steps from
files (`verus/agent.py`); `config/workloads/tpch_q21_verus_smoke.yaml` is a short run.

## Aux-state workloads

Use workloads with `workload_kind: aux_state` (e.g. `tpch_q14_aux`). Copy from `config/workloads/tpch_q_aux.template.yaml`.

```bash
python -m accel_pipeline.run_pipeline --query 14 --workload tpch_q14_aux --dry-run
```

Aux workloads:

- Prompts: `pipeline/prompts/aux_state/` (pattern library under `shared/`)
- Config: `pipeline/config/aux_state.yaml` (write paths, gates, speedup thresholds)
- Analysis schema: `pipeline/schemas/aux_state_analysis.schema.json`
- Audit measurements: `pipeline/artifacts/{tpch|sec_edgar}/q{N}/audit_result.json` (separate from `analysis.json`)
- Gates: `aux_correctness`, `aux_unit`, `aux_mutation_scenarios`, `aux_speedup_verification`, `fairness`
- Base accelerator prerequisite: functional `ctest -R tpch_q{N}` before aux stages (auto-runs `prerequisite_workload` if missing)

Tests: `cd pipeline/orchestrator && pip install -e ".[dev]" && pytest tests/test_aux_pipeline.py`

## SEC-EDGAR base workloads

Setup data and DuckDB (downloads ~5GB SEC quarterly filings):

```bash
export SEC_EDGAR_USER_AGENT="accelerator-codegen your-name@example.com"
bash scripts/setup_sec_edgar.sh
export ACCEL_SEC_EDGAR_DB=/home/akhilvb/accelerator-codegen-duckdb/sec_edgar_sf3.duckdb
```

SEC.gov requires `SEC_EDGAR_USER_AGENT` with your contact email or downloads return HTTP 403.
Setup also needs Python `duckdb` (`pip install duckdb` or `pip install -e '.[dev]'` in `pipeline/orchestrator`).

Run the base pipeline for a SEC-EDGAR query (GenDB query IDs: 1, 2, 3, 4, 6, 24):

```bash
python -m accel_pipeline.run_pipeline --query 1 --workload sec_edgar_q1
python -m accel_pipeline.run_pipeline --query 24 --workload sec_edgar_q24 --dry-run
```

- Benchmark pack: `pipeline/config/benchmark_packs/sec_edgar.yaml`
- Workloads: `pipeline/config/workloads/sec_edgar_q*.yaml`
- Artifacts: `pipeline/artifacts/sec_edgar/q{N}/`
- Gates: `correctness`, `baseline`, `profile`, `fairness` (no `sf10` — single scale factor)
- Gates run automatically after each stage. **Implementation** and **each optimization iteration** require passing: `correctness` → `baseline` → `profile`. Failures abort the pipeline.
- After gates, `optimization_feedback.md` is written for the next optimization iteration (dominant phase, gaps vs DuckDB, suggested fixes).
- Query SQL: `workloads/sec_edgar/q{N}.sql`

Tests: `pytest tests/test_benchmark_pack.py`

## Engine backend onboarding

Separate pipeline (`workload_kind: engine_backend`) generates an engine's **slot
implementations** (S1-S8, `substrate/aux/runtime/engine_slots.hpp`); the runtime
flow (commit order, versioned store, background builds with catch-up, reads at
the reader's epoch, thread budget) is the **fixed substrate**
(`substrate/aux/runtime/`) and never generated. It is **engine-agnostic and
from-scratch**: conventional paths come from `config/engine_backend.yaml`.
Agents must **not** read or copy other engine trees.

```bash
python -m accel_pipeline.run_pipeline --query 0 --workload engine_postgres

python -m accel_pipeline.run_pipeline --query 0 --workload engine_mysql

# Single layer (dry-run)
python -m accel_pipeline.run_pipeline --query 0 --workload engine_mysql \
  --stage impl_capture --dry-run
```

- Stages: analysis (sketches every layer) → `impl_capture` (S1) → `impl_epochs`
  (S2-S4) → `impl_scan` (S5, S8) → `impl_runtime` (S6) → `impl_host` (S7; skipped
  unless `requires_in_process_host`) → architecture_audit → validation
  (adversarial tests, then the slot gates)
- Layer spec: `prompts/engine_backend/layers.md`
- Each impl layer loops `engine_build` + that layer’s gate until pass
- Architecture audit is read-only; on fail it re-runs **only the named failing
  layers** (small impl fix-up) then re-audits (up to `architecture_audit_max_rounds`)
- Gates: `engine_capture`, `engine_epochs`, `engine_scan`, `engine_runtime`,
  `engine_host`, `engine_adversarial`, then the backend-generic `slot_*` gates
  (`slot_epoch_stress`, `slot_hits_under_writes`, `slot_read_after_commit`,
  `slot_txn_edges`, `slot_restart`, `slot_differential`;
  `gates/README_runtime_gates.md`)
- Host connection is workload `host_runtime` (not hardcoded in shared gates)
- Workloads: `config/workloads/engine_*.yaml`
