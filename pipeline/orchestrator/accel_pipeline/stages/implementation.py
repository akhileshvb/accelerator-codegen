from __future__ import annotations

import json

from accel_pipeline.audit_digest import record_implementation_base
from accel_pipeline.aux_bench import initialize_optimization_state, process_aux_bench_after_gates
from accel_pipeline.base_bench import process_base_bench_after_gates
from accel_pipeline.config import ModelConfig, PipelineConfig, load_run_workload, resolve_workload_paths
from accel_pipeline.gates import run_gate
from accel_pipeline.layout import query_dir
from accel_pipeline.paths import analysis_artifact_path, query_artifacts_dir
from accel_pipeline.repair import RepairCtx, run_with_repair
from accel_pipeline.session import MAIN
from accel_pipeline.stages.gate_plan import workload_post_implementation_gates
from accel_pipeline.stages.base import run_agent_stage
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

    from accel_pipeline.config import engine_host_conventions, sanitize_engine_name

    test_target = paths["aux_test_target"]
    bench_target = paths["aux_bench_target"]
    calibrate_target = paths["aux_calibrate_target"]
    q = query
    build = pipeline_config.build_dir.name
    engine = sanitize_engine_name(str(workload.get("engine_name") or ""))
    host = dict(workload.get("engine_host") or engine_host_conventions(engine, repo_root=pipeline_config.repo_root))
    diff_bin = str(host["differential_binary"])
    diff_cmd = str(host["differential_command"])
    diff_src = str(host["differential_source"])
    multi = str(host["multi_conn_target"])
    diff_step = (
        f"3. ./{build}/{diff_bin} {q}\n"
        f"   (the harness {diff_src} is made by host_bootstrap; {diff_cmd})\n"
    )
    trailing = (
        f"4. MCP run_multi_connection until pass ({multi})\n"
        "Keep fixing and re-running steps 3–4 until both pass. "
    )
    return (
        "RESUME partial aux implementation: sources already exist on disk from a prior run. "
        "Do NOT rewrite module.hpp/module.cpp from scratch or explore cmake/ (never edited).\n"
        f"Host engine: {engine}. Registered CMake targets: {test_target}, {bench_target}, "
        f"{calibrate_target}, {diff_bin}.\n"
        "Correctness requires SF1 SQL differential on the engine-selected host "
        "(see unit_test_contract.md / query_host_bootstrap.md / engine_host in gate context).\n"
        f"Register this query on every shared surface peer queries use (catalog / allowlists / "
        f"{engine} SQL entrypoint / helpers) — search a neighbor query to find sites.\n"
        "Build and test with these commands (this worker's build dir):\n"
        f"1. cmake --build {build} --target {test_target} {bench_target} {diff_bin} -j$(nproc)\n"
        f"2. ctest --test-dir {build} -R tpch_q{q}_aux --output-on-failure\n"
        f"{diff_step}"
        "   (or MCP run_correctness / request_gate aux_correctness — preferred)\n"
        f"{trailing}"
        "Fix compile/test failures with minimal edits to the generated aux + shared registration appends. "
        "Do not weaken unit fixtures to hide kernel bugs."
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
            analysis = json.loads(analysis_path.read_text(encoding="utf-8"))
            initialize_optimization_state(pipeline_config.repo_root, query, analysis, workload)
        return result

    result = run_with_repair(repair_ctx, implement_and_seed, gates=impl_gates)
    run_with_repair(repair_ctx, lambda: _record_baseline(pipeline_config, workload, query, analysis_path, impl_gates))
    return result


def _record_baseline(pipeline_config, workload: dict, query: int, analysis_path, impl_gates: list[str]) -> None:
    if workload.get("workload_kind") != "aux_state":
        process_base_bench_after_gates(
            pipeline_config.repo_root,
            query,
            workload,
            iteration=0,
            change_kind="implementation",
        )
    elif "aux_bench_sweep" in impl_gates:
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
