# Accelerator codegen agent pipeline

Four-stage pipeline for generating TPC-H query accelerators:

**Analysis → Implementation → Optimization (loop) → Audit**

## Prerequisites

- Python 3.11+
- `CURSOR_API_KEY` (default backend) or `OPENAI_API_KEY` (set `provider: openai` in `config/models.yaml`)
- Release build at `build-release/` (for gate scripts)
- DuckDB databases in `accelerator-codegen-duckdb`

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
| `openrouter` | `OPENROUTER_API_KEY` | not implemented yet |

Both Cursor and OpenAI paths use the same custom pipeline tools (`read_file`, `apply_patch`, `request_gate`, etc.) for parity.

## Aux-state workloads

Use workloads with `workload_kind: aux_state` (e.g. `tpch_q14_aux`). Copy from `config/workloads/tpch_q_aux.template.yaml`.

```bash
python -m accel_pipeline.run_pipeline --query 14 --workload tpch_q14_aux --dry-run
```

Aux workloads:

- Prompts: `pipeline/prompts/aux_state/` (pattern library under `shared/`)
- Config: `pipeline/config/aux_state.yaml` (write paths, gates, speedup thresholds)
- Analysis schema: `pipeline/schemas/aux_state_analysis.schema.json`
- Audit measurements: `pipeline/artifacts/q{N}/audit_result.json` (separate from `analysis.json`)
- Gates: `aux_correctness`, `aux_unit`, `aux_mutation_scenarios`, `aux_speedup_verification`, `fairness`
- Base accelerator prerequisite: functional `ctest -R tpch_q{N}` before aux stages (auto-runs `prerequisite_workload` if missing)

Tests: `cd pipeline/orchestrator && pip install -e ".[dev]" && pytest tests/test_aux_pipeline.py`

## TODO

See [orchestrator/accel_pipeline/TODO.md](orchestrator/accel_pipeline/TODO.md) and inline `# TODO:` in source.

- OpenRouter provider via LiteLLM (`models.yaml` `provider: openrouter`, `pip install -e ".[openrouter]"`)
