# Accelerator codegen agent pipeline

Four-stage pipeline for generating TPC-H query accelerators:

**Analysis → Implementation → Optimization (loop) → Audit**

## Prerequisites

- Python 3.11+
- API keys in environment or repo-root `.env` (copy from `.env.example`)
- `CURSOR_API_KEY` (default backend) or `OPENROUTER_API_KEY` / `OPENAI_API_KEY` per `config/models.yaml`
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
pip install -e ".[cursor]"
```

For OpenAI-only (no Cursor SDK):

```bash
pip install -e .
```

## Run (full pipeline)

```bash
export CURSOR_API_KEY=cursor_...
python -m accel_pipeline.run_pipeline --query 3 --workload tpch_q3
```

### OpenRouter

```bash
pip install -e ".[openrouter]"
export OPENROUTER_API_KEY=sk-or-...
python -m accel_pipeline.run_pipeline --query 3 --workload tpch_q3 \
  --models-config pipeline/config/models.openrouter.yaml
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
- `gates/` — deterministic gate scripts (ctest, benchmarks, fairness); runtime-skeleton gate specs in `gates/README_runtime_gates.md`
- `templates/scaffold/` — per-query stubs (kernel, package, the query's `register.cpp`) and CMake snippet
- `artifacts/` — per-query pipeline state and reports
- `orchestrator/` — Python package (`accel_pipeline`)
- `verus/` — shared Verus state: the playbook (`playbook/`) and provenance logs
- `catalog/`, `schemas/`, `fixtures/` — catalog data, JSON schemas, test fixtures

### Layers and who may write them

| Layer | Directory | Writer |
|-------|-----------|--------|
| Substrate | `substrate/aux/{core,state,maintenance}`, `substrate/verus/{common,store,kit,sql2verus}` | nobody in a run (write-denied, `test_substrate_never_writable.py`) |
| Engine backends | `backends/<engine>/{state,engine,api,tests}` | the `engine_backend` run only |
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

## Agent backends

Set `provider` in `config/models.yaml` (and optional per-stage overrides via `stage_providers`):

| Provider | Key | Install |
|----------|-----|---------|
| `cursor` (default) | `CURSOR_API_KEY` | `pip install -e ".[cursor]"` |
| `openai` | `OPENAI_API_KEY` | `pip install -e .` |
| `openrouter` | `OPENROUTER_API_KEY` | `pip install -e ".[openrouter]"` |
| `codex_exec` (via `stage_providers`) | `OPENROUTER_API_KEY` + Codex CLI | `pip install -e ".[codex]"` |

### Codex exec (analysis + implementation + optimization)

Uses Codex CLI native edit/search/shell plus an MCP server for gates/profile digests/A/B:

```bash
pip install -e ".[codex]"
# install Codex CLI separately; ensure `codex` is on PATH
export OPENROUTER_API_KEY=sk-or-...
python -m accel_pipeline.run_pipeline --query 3 --workload tpch_q3 \
  --models-config pipeline/config/models.codex.yaml
```

Config: [`config/models.codex.yaml`](config/models.codex.yaml) + [`pipeline/.codex/config.toml`](pipeline/.codex/config.toml) (OpenRouter model provider + `accel_pipeline` MCP). Hard guidelines live in `prompts/shared/hard_guidelines.md`.

Analysis and audit stay on OpenRouter Agents when using `models.codex.yaml`.

### Concurrent pack (shared tree, no stage barriers)

Run several query pipelines in parallel on one checkout. Isolation is **separate build dirs** + **per-worker `CODEX_HOME`** + a **shared-file lease** for CMake and engine-backend edits (queries register from their own `accelerators/<bench>/qN/**`, which needs no lease). Workers do not wait on each other's stages.

```bash
cd pipeline/orchestrator && source .venv/bin/activate
accel-pipeline-pack --pack tpch_aux_parallel \
  --models-config pipeline/config/models.codex.yaml
# or: python -m accel_pipeline.run_pack --pack tpch_aux_parallel ...
```

Pack YAML examples:
- [`config/packs/tpch_aux_parallel.yaml`](config/packs/tpch_aux_parallel.yaml) — Q2/Q5/Q16
- [`config/packs/tpch_aux_remaining.yaml`](config/packs/tpch_aux_remaining.yaml) — Q4/Q7/Q8/Q13/Q15/Q19/Q20/Q22

Shared path globs: `shared_paths` in [`config/default.yaml`](config/default.yaml). Logs/summary under `pipeline/artifacts/packs/`.

Set `provider: openrouter` in `config/models.yaml` (see `config/models.openrouter.yaml` for a starter config). OpenRouter Agents uses the same custom pipeline tools as Cursor (`read_file`, `apply_patch`, `run_command`, A/B benchmark tools, etc.) via the OpenAI Agents SDK + LiteLLM.

### Cursor SDK timeouts and bridge lifecycle

The Cursor SDK's default client launches a **global** bridge subprocess with `os.getcwd()` as workspace and a **60s** unary HTTP timeout. The pipeline instead:

- Launches a **dedicated owned bridge per stage** with workspace = repo root (`accelerator-codegen/`)
- Uses **300s unary / 7200s stream** timeouts by default
- Retries the stage on bridge failures (default 2 retries)

Override if needed:

```bash
export ACCEL_PIPELINE_CURSOR_UNARY_TIMEOUT=600
export ACCEL_PIPELINE_CURSOR_STREAM_TIMEOUT=14400
export ACCEL_PIPELINE_CURSOR_BRIDGE_LAUNCH_TIMEOUT=180
export ACCEL_PIPELINE_CURSOR_BRIDGE_RETRIES=4
```

For maximum stability, run a persistent external bridge (e.g. from Cursor CLI) and point the pipeline at it:

```bash
export CURSOR_SDK_BRIDGE_URL=http://127.0.0.1:PORT
export CURSOR_SDK_BRIDGE_TOKEN=...
```

If you see `NetworkError: Connection refused`, retry with `--resume`. With the dedicated-bridge fix, this should be rare; if it persists, use an external bridge.

If you see `Missing value for --tool-callback-auth-token`, that is an intermittent cursor-sdk bridge bug (~1/64 launches) when the generated callback token starts with `-`. The pipeline retries automatically; you can also `--resume` the failed stage or upgrade `cursor-sdk` (`pip install -U 'cursor-sdk>=1.0.26'` in `pipeline/orchestrator`).

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
python -m accel_pipeline.run_pipeline --query 21 --workload tpch_q21_aux --mode verus \
  --models-config pipeline/config/models.codex.yaml
accel-pipeline-pack --pack tpch_aux_parallel --mode verus --models-config pipeline/config/models.codex.yaml
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

Separate pipeline (`workload_kind: engine_backend`) generates engine adapters from the
**fixed substrate** (`substrate/aux/{core,state,maintenance}`). It is **engine-agnostic
and from-scratch**: conventional paths come from `config/engine_backend.yaml`.
Agents must **not** read or copy other engine trees.

```bash
python -m accel_pipeline.run_pipeline --query 0 --workload engine_postgres \
  --models-config pipeline/config/models.codex.yaml

python -m accel_pipeline.run_pipeline --query 0 --workload engine_mysql \
  --models-config pipeline/config/models.codex.yaml

# Single layer (dry-run)
python -m accel_pipeline.run_pipeline --query 0 --workload engine_mysql \
  --stage impl_store --dry-run
```

- Stages: analysis (sketches every layer) → `impl_store` → `impl_aux_backend` →
  `impl_txn` → `impl_mutations` → `impl_scan` → `impl_runtime` → `impl_visibility` →
  `impl_host` (skipped unless `requires_in_process_host`) → architecture_audit →
  validation (adversarial)
- Layer spec: `prompts/engine_backend/layers.md`
- Each impl layer loops `engine_build` + that layer’s gate until pass
- Architecture audit is read-only; on fail it re-runs **only the named failing
  layers** (small impl fix-up) then re-audits (up to `architecture_audit_max_rounds`)
- Gates: `engine_store`, `engine_aux_backend`, `engine_txn`, `engine_mutations`,
  `engine_scan`, `engine_runtime`, `engine_visibility`, `engine_host`,
  `engine_adversarial`
- Host connection is workload `host_runtime` (not hardcoded in shared gates)
- Workloads: `config/workloads/engine_*.yaml`

## TODO

See [orchestrator/accel_pipeline/TODO.md](orchestrator/accel_pipeline/TODO.md) and inline `# TODO:` in source.
