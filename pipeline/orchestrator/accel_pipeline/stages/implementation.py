from __future__ import annotations

import json

from accel_pipeline.digests import record_implementation_base
from accel_pipeline.config import ModelConfig, PipelineConfig
from accel_pipeline.workload import (
    load_run_workload,
    resolve_workload_paths,
    analysis_artifact_path,
    query_artifacts_dir,
)
from accel_pipeline.gates.run import run_gate
from accel_pipeline.layout import query_dir
from accel_pipeline.repair import RepairCtx, run_with_repair
from accel_pipeline.session import MAIN
from accel_pipeline.stages.base import run_agent_stage, workload_post_implementation_gates
from accel_pipeline.stages.analysis import validate_with_repair


def _implementation_user_message(
    pipeline_config: PipelineConfig,
    workload: dict,
    query: int,
) -> str:
    if workload.get("workload_kind") != "aux_state":
        return "Implement the accelerator, tests, and benchmark. Build and run correctness tests."

    paths = resolve_workload_paths(pipeline_config, workload, query)
    aux_module = pipeline_config.repo_root / paths["aux_state_subdir"] / "module.cpp"
    if not aux_module.is_file():
        return "Implement the accelerator, tests, and benchmark. Build and run correctness tests."

    return (
        "RESUME partial aux implementation: the query's sources already exist on disk from a prior run. Continue "
        "from them; do not rewrite the module from scratch.\n"
        "Validate with `quick_check` after each edit, then the gates (`request_gate`, e.g. `aux_exit`). The workload "
        "harnesses (differential, multi-connection, cross-accelerator, bench, calibrate, module test) and their "
        "targets belong to host_bootstrap: build them through the gate tools, never define or redirect them. If one "
        "is missing or broken, call `report_blocker` with the evidence.\n"
        "Fix failures with minimal edits to the query's own files. Do not weaken tests or fixtures to hide bugs."
    )


def run(
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    model_overrides: dict[str, str] | None = None,
    dry_run: bool = False,
    *,
    skip_gates: bool = False,
):
    workload = load_run_workload(pipeline_config, workload_name)
    if workload.get("workload_kind") != "aux_state":
        run_gate(pipeline_config, "scaffold", query, workload=workload)
    else:
        analysis_path = analysis_artifact_path(pipeline_config.repo_root, query, workload=workload)
        if analysis_path.is_file() and not dry_run:
            # An analysis written under older rules goes back to its session rather than stopping the run.
            validate_with_repair(pipeline_config, model_config, workload_name, query, model_overrides,
                                 analysis_path, bool(workload.get("allow_poor_fitness", False)))

    def implement():
        return run_agent_stage(
            stage="implementation",
            prompt_file="02_implementation.md",
            pipeline_config=pipeline_config,
            model_config=model_config,
            workload_name=workload_name,
            query=query,
            user_message=_implementation_user_message(pipeline_config, workload, query),
            model_overrides=model_overrides,
            dry_run=dry_run,
            must_change=pipeline_config.repo_root / query_dir(str(workload.get("benchmark_name", "tpch")), query),
            session_role=MAIN,
        )

    if dry_run:
        return implement()
    record_implementation_base(
        pipeline_config.repo_root, query_artifacts_dir(pipeline_config.repo_root, query, workload=workload))
    repair_ctx = RepairCtx(pipeline_config, model_config, workload_name, query, stage="implementation",
                           change_kind="implementation", model_overrides=model_overrides,
                           quick_first=workload.get("workload_kind") == "aux_state")
    if skip_gates:
        return run_with_repair(repair_ctx, implement)
    workload = load_run_workload(pipeline_config, workload_name)
    analysis_path = analysis_artifact_path(pipeline_config.repo_root, query, workload=workload)
    impl_gates = workload_post_implementation_gates(workload)

    def implement_and_seed():
        result = implement()
        if analysis_path.is_file() and workload.get("workload_kind") == "aux_state":
            from accel_pipeline.optimization.aux_bench import initialize_optimization_state

            analysis = json.loads(analysis_path.read_text(encoding="utf-8"))
            initialize_optimization_state(pipeline_config.repo_root, query, analysis, workload)
        return result

    result = run_with_repair(repair_ctx, implement_and_seed, gates=impl_gates)
    run_with_repair(repair_ctx, lambda: _record_baseline(pipeline_config, workload, query, analysis_path, impl_gates))
    return result


def _record_baseline(pipeline_config, workload: dict, query: int, analysis_path, impl_gates: list[str]) -> None:
    """The implementation's bench result is the optimization stage's baseline."""
    if workload.get("workload_kind") != "aux_state":
        from accel_pipeline.optimization.base_bench import process_base_bench_after_gates

        process_base_bench_after_gates(
            pipeline_config.repo_root,
            query,
            workload,
            iteration=0,
            change_kind="implementation",
        )
    elif {"aux_bench_sweep", "aux_exit"} & set(impl_gates):
        from accel_pipeline.optimization.aux_bench import process_aux_bench_after_gates

        selected = ""
        if analysis_path.is_file():
            selected = (
                json.loads(analysis_path.read_text(encoding="utf-8"))
                .get("correctness_facts", {})
                .get("state_design", {})
                .get("selected_candidate_id", "")
            )
        process_aux_bench_after_gates(
            pipeline_config.repo_root,
            query,
            iteration=0,
            change_kind="baseline",
            active_candidate_id=selected or None,
            workload=workload,
        )
