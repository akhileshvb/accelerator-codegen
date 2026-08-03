from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

from accel_pipeline.config import PipelineConfig, load_workload, resolve_workload_paths


def base_smoke_test_ctest_regex(workload: dict, query: int) -> str:
    benchmark = str(workload.get("benchmark_name", "tpch"))
    smoke_suffix = str(workload.get("ctest_smoke_suffix", "_smoke_test"))
    explicit = workload.get("ctest_smoke_regex")
    if explicit:
        return str(explicit)
    return f"^{benchmark}_q{query}{smoke_suffix}$"


def base_accelerator_header_path(repo_root: Path, workload: dict, query: int) -> Path:
    header = str(workload.get("header_basename", f"{workload.get('header_prefix', 'tpch_q')}{query}"))
    return repo_root / "include" / "accelerator_codegen" / f"{header}.hpp"


def base_kernel_source_path(repo_root: Path, workload: dict, query: int) -> Path:
    source_subdir = str(workload.get("source_subdir", f"{workload.get('benchmark_name', 'tpch')}/single-threaded"))
    return repo_root / "src" / "generated" / source_subdir / f"q{query}_kernel.cpp"


def base_accelerator_exists(repo_root: Path, workload: dict, query: int) -> bool:
    header = base_accelerator_header_path(repo_root, workload, query)
    kernel = base_kernel_source_path(repo_root, workload, query)
    return header.is_file() or kernel.is_file()


def base_smoke_test_binary_path(build_dir: Path, workload: dict, query: int) -> Path:
    test_target = str(workload.get("test_target", f"test_{workload.get('benchmark_name', 'tpch')}_q{query}"))
    return build_dir / test_target


def ensure_smoke_test_binary(build_dir: Path, workload: dict, query: int) -> Path | None:
    binary = base_smoke_test_binary_path(build_dir, workload, query)
    if binary.is_file():
        return binary
    target = str(workload.get("test_target", f"test_{workload.get('benchmark_name', 'tpch')}_q{query}"))
    subprocess.run(
        ["cmake", "--build", str(build_dir), "--target", target],
        cwd=build_dir,
        capture_output=True,
        text=True,
        check=False,
    )
    return binary if binary.is_file() else None


def base_correctness_passes(
    pipeline_config: PipelineConfig,
    workload: dict,
    query: int,
) -> tuple[bool, str]:
    build_dir = pipeline_config.build_dir
    if not build_dir.is_dir():
        return False, f"build dir missing: {build_dir}"
    proc = subprocess.run(
        ["ctest", "-R", base_smoke_test_ctest_regex(workload, query), "--output-on-failure"],
        cwd=build_dir,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode == 0:
        return True, ""

    binary = ensure_smoke_test_binary(build_dir, workload, query)
    if binary is not None:
        paths = resolve_workload_paths(pipeline_config, workload, query)
        env = {paths["db_env_var"]: paths["dataset_db"]}
        direct = subprocess.run(
            [str(binary)],
            cwd=build_dir,
            capture_output=True,
            text=True,
            check=False,
            env={**os.environ, **env},
        )
        if direct.returncode == 0:
            return True, ""
        tail = (direct.stdout or direct.stderr or "").strip().splitlines()
        reason = tail[-1] if tail else f"smoke binary exit {direct.returncode}"
        return False, reason

    tail = (proc.stdout or proc.stderr or "").strip().splitlines()
    reason = tail[-1] if tail else f"ctest exit {proc.returncode}"
    return False, reason


def base_accelerator_ready(pipeline_config: PipelineConfig, workload: dict, query: int) -> bool:
    repo = pipeline_config.repo_root
    if not base_accelerator_exists(repo, workload, query):
        return False
    ok, _ = base_correctness_passes(pipeline_config, workload, query)
    return ok


def base_accelerator_not_ready_reason(
    pipeline_config: PipelineConfig,
    workload: dict,
    query: int,
) -> str:
    repo = pipeline_config.repo_root
    if not base_accelerator_exists(repo, workload, query):
        return "base accelerator source not found"
    ok, reason = base_correctness_passes(pipeline_config, workload, query)
    if ok:
        return ""
    return f"smoke test failed: {reason}"


def ensure_base_accelerator_prerequisite(
    pipeline_config: PipelineConfig,
    workload_name: str,
    query: int,
    *,
    dry_run: bool = False,
) -> bool:
    """
    If aux workload requires a base accelerator, ensure it exists and passes correctness.
    Returns True if prerequisite was run, False if already satisfied or not required.
    """
    workload = load_workload(workload_name, pipeline_config.repo_root)
    if workload.get("workload_kind") != "aux_state":
        return False
    if not workload.get("requires_base_accelerator", False):
        return False

    benchmark = str(workload.get("benchmark_name", "tpch"))
    prereq_name = workload.get("prerequisite_workload") or f"{benchmark}_q{query}"
    prereq_workload = load_workload(str(prereq_name), pipeline_config.repo_root)

    if base_accelerator_ready(pipeline_config, prereq_workload, query):
        return False

    min_stage = workload.get("prerequisite_min_stage", "implementation")
    not_ready = base_accelerator_not_ready_reason(pipeline_config, prereq_workload, query)
    print(
        f"Base accelerator for Q{query} not ready ({not_ready}); "
        f"running prerequisite workload {prereq_name!r} through {min_stage!r}",
        file=sys.stderr,
    )

    if dry_run:
        return True

    from accel_pipeline.config import load_model_config
    from accel_pipeline.stages import analysis, audit, implementation, optimization

    model_config = load_model_config(repo_root=pipeline_config.repo_root)
    stages = {
        "analysis": analysis.run,
        "implementation": implementation.run,
        "optimization": lambda pc, mc, wn, q, mo, dr: optimization.run(
            pc, mc, wn, q, pipeline_config.optimization_max_iterations, mo, dr
        ),
        "audit": audit.run,
    }
    stage_order = ["analysis", "implementation", "optimization", "audit"]
    if min_stage not in stage_order:
        raise ValueError(f"invalid prerequisite_min_stage: {min_stage}")
    end_idx = stage_order.index(min_stage) + 1
    for stage_name in stage_order[:end_idx]:
        stages[stage_name](
            pipeline_config,
            model_config,
            str(prereq_name),
            query,
            None,
            dry_run=False,
        )

    if not base_accelerator_ready(pipeline_config, prereq_workload, query):
        raise RuntimeError(
            f"prerequisite workload {prereq_name!r} did not produce a passing base accelerator for Q{query}"
        )
    return True
