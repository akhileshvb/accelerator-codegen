# Accelerator codegen agent pipeline

Four-stage pipeline for generating TPC-H query accelerators:

**Analysis → Implementation → Optimization (loop) → Audit**

## Prerequisites

- Python 3.11+
- `OPENAI_API_KEY` in the environment
- Release build at `build-release/` (for gate scripts)
- DuckDB databases in `accelerator-codegen-duckdb`

## Install orchestrator

```bash
cd pipeline/orchestrator
pip install -e .
```

## Run (full pipeline)

```bash
export OPENAI_API_KEY=sk-...
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

## TODO

See [orchestrator/accel_pipeline/TODO.md](orchestrator/accel_pipeline/TODO.md) and inline `# TODO:` in source.

- OpenRouter provider via LiteLLM (`models.yaml` `provider: openrouter`)
