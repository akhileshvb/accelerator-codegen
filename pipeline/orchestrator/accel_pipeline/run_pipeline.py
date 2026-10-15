from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

from accel_pipeline.config import apply_workload_overrides, load_model_config, load_pipeline_config, load_workload
from accel_pipeline.env_loader import load_repo_dotenv
from accel_pipeline.paths import find_repo_root
from accel_pipeline.prerequisites import ensure_base_accelerator_prerequisite
from accel_pipeline.state import STAGES, advance_stage, load_or_create, state_path
from accel_pipeline.verus.stages import STAGES as VERUS_STAGES
from accel_pipeline.stages import analysis, adversarial_tests, audit, implementation, optimization
from accel_pipeline.structural_reanalysis import process_after_stage
from accel_pipeline.pipeline_log import log_event, log_pipeline_summary


def _parse_model_overrides(values: list[str] | None) -> dict[str, str]:
    overrides: dict[str, str] = {}
    for item in values or []:
        if "=" not in item:
            raise ValueError(f"expected stage=preset, got {item!r}")
        stage, preset = item.split("=", 1)
        overrides[stage.strip()] = preset.strip()
    return overrides


def _maybe_process_structural_reanalysis(
    *,
    triggered_after: str,
    pipeline_config,
    model_config,
    workload_name: str,
    workload: dict,
    query: int,
    model_overrides: dict[str, str],
    dry_run: bool,
    state,
) -> int | None:
    """Run bounded re-analysis if requested. Returns next STAGES index, or None."""
    if workload.get("workload_kind") != "aux_state":
        return None

    def run_analysis(**kwargs):
        analysis.run(
            pipeline_config,
            model_config,
            workload_name,
            query,
            model_overrides,
            dry_run,
            user_message=kwargs.get("user_message"),
            analysis_revision_request=kwargs.get("analysis_revision_request", ""),
        )

    def run_adv():
        adversarial_tests.run(
            pipeline_config, model_config, workload_name, query, model_overrides, dry_run
        )

    def run_impl():
        implementation.run(
            pipeline_config, model_config, workload_name, query, model_overrides, dry_run
        )

    def run_opt():
        optimization.run(
            pipeline_config,
            model_config,
            workload_name,
            query,
            pipeline_config.optimization_max_iterations,
            model_overrides,
            dry_run,
        )

    result = process_after_stage(
        repo_root=pipeline_config.repo_root,
        query=query,
        workload=workload,
        triggered_after=triggered_after,
        structural_reanalysis_count=state.structural_reanalysis_count,
        run_analysis=run_analysis,
        run_adversarial_tests=run_adv,
        run_implementation=run_impl,
        run_optimization=run_opt,
        dry_run=dry_run,
    )
    if not result:
        return None
    new_count, next_idx = result
    state.structural_reanalysis_count = new_count
    state.stage = STAGES[next_idx]
    state.save(state_path(pipeline_config.repo_root, query, workload_name))
    log_event(
        pipeline_config.repo_root,
        query,
        "structural_reanalysis_resume",
        rerun_number=new_count,
        resume_stage=STAGES[next_idx],
        triggered_after=triggered_after,
    )
    return next_idx


def _run_stage(
    stage: str,
    pipeline_config,
    model_config,
    workload_name: str,
    query: int,
    model_overrides: dict[str, str],
    dry_run: bool,
    *,
    skip_gates: bool = False,
    resume: bool = False,
) -> None:
    if stage == "analysis":
        analysis.run(pipeline_config, model_config, workload_name, query, model_overrides, dry_run)
    elif stage == "adversarial_tests":
        adversarial_tests.run(
            pipeline_config, model_config, workload_name, query, model_overrides, dry_run
        )
    elif stage == "implementation":
        implementation.run(
            pipeline_config,
            model_config,
            workload_name,
            query,
            model_overrides,
            dry_run,
            skip_gates=skip_gates,
        )
    elif stage == "optimization":
        optimization.run(
            pipeline_config,
            model_config,
            workload_name,
            query,
            pipeline_config.optimization_max_iterations,
            model_overrides,
            dry_run,
            skip_gates=skip_gates,
            resume=resume,
        )
    elif stage == "audit":
        audit.run(pipeline_config, model_config, workload_name, query, model_overrides, dry_run)
    else:
        raise ValueError(f"unknown stage: {stage}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Accelerator codegen agent pipeline")
    parser.add_argument("--query", type=int, required=True)
    parser.add_argument("--workload", type=str, default="tpch_q3")
    parser.add_argument(
        "--stage",
        choices=[
            *STAGES,
            *VERUS_STAGES,
            "host_bootstrap",
            "validation",
            "architecture_audit",
            "impl_capture",
            "impl_epochs",
            "impl_scan",
            "impl_runtime",
            "impl_host",
            "all",
        ],
        default="all",
    )
    parser.add_argument(
        "--mode",
        choices=["cpp", "verus"],
        default=None,
        help="cpp: C++ accelerator pipeline (default); verus: the Verus accelerator pipeline "
        "(accel_pipeline/verus; same front end, its own stages). Default: the workload's `mode`.",
    )
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--dry-run", action="store_true", help="Render prompts; skip LLM calls")
    parser.add_argument(
        "--skip-gates",
        action="store_true",
        help="Skip post-stage gates (agent only; no calibrate/sweep/ctest after the stage).",
    )
    parser.add_argument("--config", type=Path, default=None)
    parser.add_argument(
        "--models-config",
        type=Path,
        default=None,
        help="Path to models.yaml (default: pipeline/config/models.yaml)",
    )
    parser.add_argument("--model", action="append", dest="models", default=[])
    args = parser.parse_args(argv)

    if args.dry_run:
        os.environ["ACCEL_PIPELINE_DRY_RUN"] = "1"

    # Pack worktree isolation: prefer explicit ACCEL_PIPELINE_REPO_ROOT.
    env_repo = os.environ.get("ACCEL_PIPELINE_REPO_ROOT")
    primary_env = os.environ.get("ACCEL_PIPELINE_PRIMARY_REPO")
    repo_root = Path(env_repo).resolve() if env_repo else find_repo_root()
    primary_root = Path(primary_env).resolve() if primary_env else find_repo_root()
    dotenv_path = load_repo_dotenv(primary_root)
    if dotenv_path is not None:
        try:
            print(f"loaded env file: {dotenv_path.relative_to(primary_root)}", file=sys.stderr)
        except ValueError:
            print(f"loaded env file: {dotenv_path}", file=sys.stderr)
    pipeline_config = load_pipeline_config(args.config, repo_root)
    workload = load_workload(args.workload, primary_root)
    pipeline_config = apply_workload_overrides(pipeline_config, workload)
    # Concurrent pack workers: separate build dir + CODEX_HOME (+ optional worktree).
    if os.environ.get("ACCEL_PIPELINE_WORKER_ID") or os.environ.get("ACCEL_PIPELINE_BUILD_DIR"):
        from accel_pipeline.worker_isolation import apply_worker_isolation, isolation_summary

        pipeline_config = apply_worker_isolation(
            pipeline_config,
            query=args.query,
            workload_name=args.workload,
            set_environ=True,
        )
        print(f"worker isolation: {isolation_summary(pipeline_config)}", file=sys.stderr)
    repo_root = Path(pipeline_config.repo_root)
    model_config = load_model_config(args.models_config, repo_root=primary_root)
    print(f"agent provider: {model_config.provider}", file=sys.stderr)
    query = args.query
    if workload.get("query_number") is not None and int(workload["query_number"]) != query:
        if workload.get("workload_kind") != "engine_backend":
            print("warning: --query does not match workload query_number", file=sys.stderr)

    model_overrides = _parse_model_overrides(args.models)

    if workload.get("workload_kind") == "engine_backend":
        from accel_pipeline.stages import engine_backend as engine_backend_stage

        print(f"=== engine_backend workload: {args.workload} ===", file=sys.stderr)
        engine_backend_stage.run_engine_pipeline(
            pipeline_config,
            model_config,
            args.workload,
            query,
            model_overrides,
            args.dry_run,
            stage=args.stage if args.stage != "all" else "all",
            skip_gates=args.skip_gates,
        )
        print("pipeline complete", file=sys.stderr)
        return 0

    # Verus mode: same front end, then the Verus pipeline's own stages.  It
    # runs on DuckDB (in-process; no host bootstrap) and skips the C++
    # base-accelerator prerequisite: it must not see C++ accelerators.
    mode = args.mode or str(workload.get("mode") or "cpp")
    if mode == "verus":
        from accel_pipeline.verus.run import run_verus_pipeline

        print(f"=== verus mode: {args.workload} ===", file=sys.stderr)
        return run_verus_pipeline(
            pipeline_config,
            model_config,
            workload,
            args.workload,
            query,
            stage=args.stage,
            resume=args.resume,
            dry_run=args.dry_run,
            model_overrides=model_overrides,
        )
    if args.stage in VERUS_STAGES and args.stage not in STAGES:
        print(f"stage {args.stage!r} belongs to --mode verus", file=sys.stderr)
        return 2

    # One-shot query-host bootstrap (non-duckdb) before per-query stages.
    if args.stage in {"all", "host_bootstrap"}:
        from accel_pipeline.host_bootstrap import ensure_host_bootstrap_for_workload

        hb = ensure_host_bootstrap_for_workload(
            pipeline_config,
            model_config,
            args.workload,
            model_overrides=model_overrides,
            dry_run=args.dry_run,
            skip_gates=args.skip_gates,
        )
        if hb is not None:
            print(f"=== host_bootstrap: {hb} ===", file=sys.stderr)
        if args.stage == "host_bootstrap":
            print("pipeline complete", file=sys.stderr)
            return 0

    state = load_or_create(repo_root, query, args.workload, resume=args.resume)

    if workload.get("workload_kind") == "aux_state":
        ensure_base_accelerator_prerequisite(
            pipeline_config,
            args.workload,
            query,
            model_config=model_config,
            model_overrides=model_overrides,
            dry_run=args.dry_run,
        )

    if args.stage != "all":
        print(f"=== stage: {args.stage} ===", file=sys.stderr)
        _run_stage(
            args.stage,
            pipeline_config,
            model_config,
            args.workload,
            query,
            model_overrides,
            args.dry_run,
            skip_gates=args.skip_gates,
            resume=args.resume,
        )
        state.stage = args.stage
        state.save(state_path(repo_root, query, args.workload))
        print("pipeline complete", file=sys.stderr)
        return 0

    start_idx = STAGES.index(state.stage) if args.resume else 0
    initial_stage = state.stage
    idx = start_idx
    while idx < len(STAGES):
        stage = STAGES[idx]
        print(f"=== stage: {stage} ===", file=sys.stderr)
        if not args.dry_run:
            state.stage = stage
            state.save(state_path(repo_root, query, args.workload))
        _run_stage(stage, pipeline_config, model_config, args.workload, query, model_overrides, args.dry_run, skip_gates=args.skip_gates, resume=args.resume)

        if stage == "implementation":
            next_idx = _maybe_process_structural_reanalysis(
                triggered_after="implementation",
                pipeline_config=pipeline_config,
                model_config=model_config,
                workload_name=args.workload,
                workload=workload,
                query=query,
                model_overrides=model_overrides,
                dry_run=args.dry_run,
                state=state,
            )
            if next_idx is not None:
                idx = next_idx
                continue

        if stage == "optimization":
            next_idx = _maybe_process_structural_reanalysis(
                triggered_after="optimization",
                pipeline_config=pipeline_config,
                model_config=model_config,
                workload_name=args.workload,
                workload=workload,
                query=query,
                model_overrides=model_overrides,
                dry_run=args.dry_run,
                state=state,
            )
            if next_idx is not None:
                idx = next_idx
                continue

        if stage != STAGES[-1] and not args.dry_run:
            advance_stage(state, repo_root)
        idx += 1

    if args.dry_run:
        # Dry-run must not poison --resume (e.g. leave stage=audit with no sources).
        state.stage = initial_stage
        state.save(state_path(repo_root, query, args.workload))
        print("pipeline dry-run complete (state unchanged)", file=sys.stderr)
        return 0

    state.stage = STAGES[-1]
    state.save(state_path(repo_root, query, args.workload))
    log_pipeline_summary(repo_root, query)
    print("pipeline complete", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
