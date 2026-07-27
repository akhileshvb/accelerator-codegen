from __future__ import annotations

import json

from accel_pipeline.aux_bench import initialize_optimization_state, process_aux_bench_after_gates
from accel_pipeline.config import ModelConfig, PipelineConfig, load_workload
from accel_pipeline.gates import run_gate
from accel_pipeline.paths import query_artifacts_dir
from accel_pipeline.stages.gate_plan import run_post_stage_gates, workload_post_implementation_gates
from accel_pipeline.stages.base import run_agent_stage
from accel_pipeline.validation import AuxAnalysisValidationError, validate_aux_analysis_file


def run(
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    model_overrides: dict[str, str] | None = None,
    dry_run: bool = False,
):
    workload = load_workload(workload_name, pipeline_config.repo_root)
    if workload.get("workload_kind") != "aux_state":
        run_gate(pipeline_config, "scaffold", query)
    else:
        analysis_path = query_artifacts_dir(pipeline_config.repo_root, query) / "analysis.json"
        if analysis_path.is_file():
            try:
                validate_aux_analysis_file(
                    analysis_path,
                    allow_poor_fitness=bool(workload.get("allow_poor_fitness", False)),
                )
            except AuxAnalysisValidationError as exc:
                raise RuntimeError(f"aux analysis validation failed before implementation: {exc}") from exc

    result = run_agent_stage(
        stage="implementation",
        prompt_file="02_implementation.md",
        pipeline_config=pipeline_config,
        model_config=model_config,
        workload_name=workload_name,
        query=query,
        user_message="Implement the accelerator, tests, and benchmark. Build and run correctness tests.",
        model_overrides=model_overrides,
        dry_run=dry_run,
    )
    if not dry_run:
        workload = load_workload(workload_name, pipeline_config.repo_root)
        analysis_path = query_artifacts_dir(pipeline_config.repo_root, query) / "analysis.json"
        if analysis_path.is_file():
            analysis = json.loads(analysis_path.read_text(encoding="utf-8"))
            initialize_optimization_state(pipeline_config.repo_root, query, analysis)
        run_post_stage_gates(
            pipeline_config,
            workload_name,
            query,
            workload_post_implementation_gates(workload),
        )
        if "aux_bench_sweep" in workload_post_implementation_gates(workload):
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
            )
    return result
