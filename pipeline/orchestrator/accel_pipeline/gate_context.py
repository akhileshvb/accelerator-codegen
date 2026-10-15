from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from accel_pipeline.config import (
    PipelineConfig,
    engine_host_conventions,
    resolve_workload_paths,
    sanitize_engine_name,
)
from accel_pipeline.paths import gates_dir_for_query, query_artifacts_dir


def build_gate_context(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
) -> dict[str, Any]:
    paths = resolve_workload_paths(pipeline_config, workload, query)
    benchmark = str(workload.get("benchmark_name", "tpch"))
    kind = str(workload.get("workload_kind", "base"))
    # Accelerators default to duckdb; engine_backend always sets engine_name.
    raw_engine = workload.get("engine_name")
    if kind == "engine_backend":
        engine = sanitize_engine_name(str(raw_engine or ""))
    else:
        engine = sanitize_engine_name(str(raw_engine) if raw_engine else "")
    host = dict(workload.get("engine_host") or engine_host_conventions(engine))
    ctx: dict[str, Any] = {
        "query_number": query,
        "benchmark_name": benchmark,
        "artifact_namespace": str(workload.get("artifact_namespace", benchmark)),
        "display_name": str(workload.get("display_name", benchmark)),
        "workload_kind": kind,
        "engine_name": engine,
        "engine_host": host,
        "source_subdir": paths["source_subdir"],
        "function_name": paths["function_name"],
        "create_fn": paths["create_fn"],
        "test_target": paths["test_target"],
        "bench_target": paths["bench_target"],
        "ctest_smoke_name": paths["ctest_smoke_name"],
        "ctest_smoke_regex": paths["ctest_smoke_regex"],
        "dataset_db": str(paths["dataset_db"]),
        "scale_db": str(paths.get("scale_db", "")),
        "db_env_var": paths["db_env_var"],
        "ctest_resource_lock": str(workload.get("ctest_resource_lock", f"{benchmark}_db")),
        "build_dir": str(pipeline_config.build_dir),
        "fairness": dict(workload.get("fairness", {})),
        "header_path": paths["header_path"],
        "kernel_path": paths["kernel_path"],
        "package_path": paths["package_path"],
    }
    if workload.get("workload_kind") == "aux_state":
        ctx.update(
            {
                "aux_function_name": paths["aux_function_name"],
                "aux_test_target": paths["aux_test_target"],
                "aux_bench_target": paths["aux_bench_target"],
                "aux_hybrid_bench_target": paths["aux_hybrid_bench_target"],
                "aux_calibrate_target": paths["aux_calibrate_target"],
                "aux_ctest_smoke_regex": paths["aux_ctest_smoke_regex"],
                "aux_state_subdir": paths["aux_state_subdir"],
                "cost_model_config": paths["cost_model_config"],
                "aux_bench_work_dir_suffix": paths["aux_bench_work_dir_suffix"],
                "aux_bench_suites": paths["aux_bench_suites"],
            }
        )
        bench_sweep = workload.get("aux_bench_sweep", {})
        if isinstance(bench_sweep, dict):
            if bench_sweep:
                ctx["aux_bench_sweep"] = bench_sweep
            freqs = bench_sweep.get("write_frequencies")
            if freqs:
                ctx["aux_write_frequencies"] = ",".join(str(f) for f in freqs)
        opt_sweep = workload.get("aux_bench_sweep_optimization", {})
        if isinstance(opt_sweep, dict) and opt_sweep:
            ctx["aux_bench_sweep_optimization"] = opt_sweep
    if kind == "engine_backend":
        ctx.update(
            {
                "target_paths": dict(workload.get("target_paths") or {}),
                "cmake": dict(workload.get("cmake") or {}),
                "placement_candidates": list(workload.get("placement_candidates") or []),
                "requires_in_process_host": bool(workload.get("requires_in_process_host", False)),
                "implementation_layers": list(workload.get("implementation_layers") or []),
                "host_runtime": dict(workload.get("host_runtime") or {}),
            }
        )
        host_rt = dict(workload.get("host_runtime") or {})
        if host_rt:
            ctx["host_sdk"] = {
                "engine": engine,
                "mode": str(host_rt.get("kind") or "host_cmake"),
                "found": True,
                "summary": str(host_rt.get("kind") or "host_cmake"),
            }
    return ctx


def write_gate_context(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
) -> Path:
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    gates = gates_dir_for_query(art)
    gates.mkdir(parents=True, exist_ok=True)
    path = gates / "_gate_context.json"
    path.write_text(json.dumps(build_gate_context(pipeline_config, workload, query), indent=2), encoding="utf-8")
    return path
