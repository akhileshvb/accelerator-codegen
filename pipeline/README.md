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

- `config/` — orchestrator and model settings, workload YAML
- `prompts/` — stage prompts and shared constraints
- `gates/` — deterministic gate scripts (ctest, benchmarks, fairness)
- `templates/scaffold/` — register/CMake snippet templates
- `artifacts/` — per-query pipeline state and reports
- `orchestrator/` — Python package (`accel_pipeline`)

## Agent backends

Set `provider` in `config/models.yaml`:

| Provider | Key | Install |
|----------|-----|---------|
| `cursor` (default) | `CURSOR_API_KEY` | `pip install -e ".[cursor]"` |
| `openai` | `OPENAI_API_KEY` | `pip install -e .` |
| `openrouter` | `OPENROUTER_API_KEY` | `pip install -e ".[openrouter]"` |

Set `provider: openrouter` in `config/models.yaml` (see `config/models.openrouter.yaml` for a starter config). OpenRouter uses the same custom pipeline tools as Cursor (`read_file`, `apply_patch`, `run_command`, A/B benchmark tools, etc.) via the OpenAI Agents SDK + LiteLLM.

Both Cursor and OpenAI/OpenRouter paths use the same custom pipeline tools (`read_file`, `apply_patch`, `request_gate`, etc.) for parity.

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
- Query SQL: `benchmarks/sec_edgar/queries/q{N}.sql`

Tests: `pytest tests/test_benchmark_pack.py`

## TODO

See [orchestrator/accel_pipeline/TODO.md](orchestrator/accel_pipeline/TODO.md) and inline `# TODO:` in source.

- OpenRouter provider via LiteLLM (`models.yaml` `provider: openrouter`, `pip install -e ".[openrouter]"`)
