from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from accel_pipeline.config import PipelineConfig, resolve_workload_paths
from accel_pipeline.paths import gates_dir_for_query, query_artifacts_dir


def build_gate_context(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
) -> dict[str, Any]:
    paths = resolve_workload_paths(pipeline_config, workload, query)
    benchmark = str(workload.get("benchmark_name", "tpch"))
    return {
        "query_number": query,
        "benchmark_name": benchmark,
        "artifact_namespace": str(workload.get("artifact_namespace", benchmark)),
        "display_name": str(workload.get("display_name", benchmark)),
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
        "fairness": dict(workload.get("fairness", {})),
        "header_path": paths["header_path"],
        "kernel_path": paths["kernel_path"],
        "package_path": paths["package_path"],
    }


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
