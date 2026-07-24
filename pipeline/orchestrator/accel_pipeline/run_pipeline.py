from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

from accel_pipeline.config import load_model_config, load_pipeline_config, load_workload
from accel_pipeline.paths import find_repo_root
from accel_pipeline.state import STAGES, advance_stage, load_or_create, state_path
from accel_pipeline.stages import analysis, audit, implementation, optimization


def _parse_model_overrides(values: list[str] | None) -> dict[str, str]:
    overrides: dict[str, str] = {}
    for item in values or []:
        if "=" not in item:
            raise ValueError(f"expected stage=preset, got {item!r}")
        stage, preset = item.split("=", 1)
        overrides[stage.strip()] = preset.strip()
    return overrides


def _run_stage(
    stage: str,
    pipeline_config,
    model_config,
    workload_name: str,
    query: int,
    model_overrides: dict[str, str],
    dry_run: bool,
) -> None:
    if stage == "analysis":
        analysis.run(pipeline_config, model_config, workload_name, query, model_overrides, dry_run)
    elif stage == "implementation":
        implementation.run(pipeline_config, model_config, workload_name, query, model_overrides, dry_run)
    elif stage == "optimization":
        optimization.run(
            pipeline_config,
            model_config,
            workload_name,
            query,
            pipeline_config.optimization_max_iterations,
            model_overrides,
            dry_run,
        )
    elif stage == "audit":
        audit.run(pipeline_config, model_config, workload_name, query, model_overrides, dry_run)
    else:
        raise ValueError(f"unknown stage: {stage}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Accelerator codegen agent pipeline")
    parser.add_argument("--query", type=int, required=True)
    parser.add_argument("--workload", type=str, default="tpch_q3")
    parser.add_argument("--stage", choices=[*STAGES, "all"], default="all")
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--dry-run", action="store_true", help="Render prompts; skip LLM calls")
    parser.add_argument("--config", type=Path, default=None)
    parser.add_argument("--model", action="append", dest="models", default=[])
    # TODO: --queries 1,3,6,12 batch mode; --branch auto for isolated git branches per run.
    args = parser.parse_args(argv)

    if args.dry_run:
        os.environ["ACCEL_PIPELINE_DRY_RUN"] = "1"

    repo_root = find_repo_root()
    pipeline_config = load_pipeline_config(args.config, repo_root)
    model_config = load_model_config(repo_root=repo_root)
    workload = load_workload(args.workload, repo_root)
    query = args.query
    if workload.get("query_number") and int(workload["query_number"]) != query:
        print("warning: --query does not match workload query_number", file=sys.stderr)

    model_overrides = _parse_model_overrides(args.models)
    state = load_or_create(repo_root, query, args.workload, resume=args.resume)

    if args.stage != "all":
        print(f"=== stage: {args.stage} ===", file=sys.stderr)
        _run_stage(args.stage, pipeline_config, model_config, args.workload, query, model_overrides, args.dry_run)
        state.stage = args.stage
        state.save(state_path(repo_root, query))
        print("pipeline complete", file=sys.stderr)
        return 0

    start_idx = STAGES.index(state.stage) if args.resume else 0
    for stage in STAGES[start_idx:]:
        print(f"=== stage: {stage} ===", file=sys.stderr)
        state.stage = stage
        state.save(state_path(repo_root, query))
        _run_stage(stage, pipeline_config, model_config, args.workload, query, model_overrides, args.dry_run)
        if stage != STAGES[-1]:
            advance_stage(state, repo_root)

    state.stage = STAGES[-1]
    state.save(state_path(repo_root, query))
    print("pipeline complete", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
