from __future__ import annotations

import json

from accel_pipeline.aux_bench import initialize_optimization_state, process_aux_bench_after_gates
from accel_pipeline.base_bench import process_base_bench_after_gates
from accel_pipeline.config import ModelConfig, PipelineConfig, load_run_workload, resolve_workload_paths
from accel_pipeline.crash_debug import run_post_stage_gates_with_crash_debug
from accel_pipeline.gates import run_gate
from accel_pipeline.paths import analysis_artifact_path, query_artifacts_dir
from accel_pipeline.stages.gate_plan import workload_post_implementation_gates
from accel_pipeline.stages.base import run_agent_stage
from accel_pipeline.validation import AuxAnalysisValidationError, validate_aux_analysis_file


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
    host = dict(workload.get("engine_host") or engine_host_conventions(engine))
    diff_bin = str(host.get("differential_binary") or "test_tpch_shared_differential")
    diff_cmd = str(
        host.get("differential_command")
        or f'./{build}/{diff_bin} "$ACCEL_TPCH_DB" {q}'
    )
    if engine == "duckdb":
        diff_step = f'3. ./{build}/{diff_bin} "$ACCEL_TPCH_DB" {q}\n'
        trailing = (
            "4. MCP run_multi_connection (or request_gate aux_multi_connection) until status=pass\n"
            "Keep fixing and re-running steps 3–4 until both pass. "
        )
    else:
        diff_src = str(host.get("differential_source") or f"eval/tests/{diff_bin}.cpp")
        multi = str(host.get("multi_conn_ctest_regex") or f"test_{engine}_aux_multi_connection_stress")
        diff_step = (
            f"3. Bootstrap/extend {engine} query host if needed "
            f"(see query_host_bootstrap.md), then ./{build}/{diff_bin} {q}\n"
            f"   (create {diff_src} + CMake target if missing; {diff_cmd})\n"
        )
        trailing = (
            f"4. MCP run_multi_connection until pass (ctest {multi}; "
            f"replicate duckdb multi-conn/cross-accel/bench harnesses for {engine})\n"
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
        user_message=_implementation_user_message(pipeline_config, workload, query),
        model_overrides=model_overrides,
        dry_run=dry_run,
    )
    if not dry_run and not skip_gates:
        workload = load_run_workload(pipeline_config, workload_name)
        analysis_path = analysis_artifact_path(pipeline_config.repo_root, query, workload=workload)
        if analysis_path.is_file() and workload.get("workload_kind") == "aux_state":
            analysis = json.loads(analysis_path.read_text(encoding="utf-8"))
            initialize_optimization_state(pipeline_config.repo_root, query, analysis, workload)
        impl_gates = workload_post_implementation_gates(workload)
        if workload.get("workload_kind") == "aux_state":
            from accel_pipeline.stages.gate_plan import run_post_stage_gates

            run_post_stage_gates(
                pipeline_config,
                workload_name,
                query,
                impl_gates,
                change_kind="implementation",
                iteration=0,
            )
        else:
            run_post_stage_gates_with_crash_debug(
                pipeline_config,
                model_config,
                workload_name,
                query,
                impl_gates,
                change_kind="implementation",
                iteration=0,
                model_overrides=model_overrides,
            )
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
    return result
