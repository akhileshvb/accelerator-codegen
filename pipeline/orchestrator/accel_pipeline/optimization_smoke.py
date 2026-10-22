"""Cheap correctness smoke test before optimization benchmarks."""

from __future__ import annotations

import os
import subprocess
from dataclasses import dataclass

from accel_pipeline.config import PipelineConfig, resolve_workload_paths


@dataclass
class SmokeResult:
    passed: bool
    output: str = ""
    regex: str = ""


def _smoke_regex(workload: dict, paths: dict[str, str]) -> str:
    if workload.get("workload_kind") == "aux_state":
        explicit = paths.get("aux_ctest_smoke_regex") or workload.get("aux_ctest_smoke_regex")
        if explicit:
            return str(explicit)
        q = int(workload.get("query_number", 0) or 0)
        benchmark = str(workload.get("benchmark_name", "tpch"))
        return f"{benchmark}_q{q}_aux"
    return str(paths.get("ctest_smoke_regex") or paths.get("test_target", ""))


def run_smoke_correctness(
    pipeline_config: PipelineConfig,
    workload: dict,
    query: int,
    *,
    timeout_sec: int = 180,
) -> SmokeResult:
    """Quick correctness gate: ctest smoke for this workload (runs after every optimization patch)."""
    paths = resolve_workload_paths(pipeline_config, workload, query)
    regex = _smoke_regex(workload, paths)
    if not regex:
        return SmokeResult(passed=False, output="no smoke test regex configured", regex="")

    build_dir = pipeline_config.build_dir
    ctest_bin = build_dir / "ctest"
    cmd = [
        str(ctest_bin if ctest_bin.is_file() else "ctest"),
        "--test-dir",
        str(build_dir),
        "-R",
        regex,
        "--output-on-failure",
    ]
    env = dict(os.environ)
    db_var = paths.get("db_env_var")
    db_path = paths.get("dataset_db")
    if db_var and db_path:
        env[str(db_var)] = str(db_path)

    proc = subprocess.run(
        cmd,
        cwd=pipeline_config.repo_root,
        capture_output=True,
        text=True,
        check=False,
        timeout=timeout_sec,
        env=env,
    )
    output = (proc.stdout or "") + (proc.stderr or "")
    return SmokeResult(passed=proc.returncode == 0, output=output[-6000:], regex=regex)


def run_smoke_correctness_repeated(
    pipeline_config: PipelineConfig,
    workload: dict,
    query: int,
    *,
    runs: int = 2,
    timeout_sec: int = 180,
) -> SmokeResult:
    """Run smoke ctest multiple times; all runs must pass."""
    if runs <= 1:
        return run_smoke_correctness(
            pipeline_config, workload, query, timeout_sec=timeout_sec
        )
    last: SmokeResult | None = None
    combined_output: list[str] = []
    for run_idx in range(1, runs + 1):
        last = run_smoke_correctness(
            pipeline_config, workload, query, timeout_sec=timeout_sec
        )
        combined_output.append(f"--- smoke run {run_idx}/{runs} ---\n{last.output}")
        if not last.passed:
            return SmokeResult(
                passed=False,
                output="\n".join(combined_output)[-6000:],
                regex=last.regex,
            )
    assert last is not None
    return SmokeResult(
        passed=True,
        output="\n".join(combined_output)[-6000:],
        regex=last.regex,
    )


def run_build(
    pipeline_config: PipelineConfig,
    workload: dict,
    query: int,
) -> tuple[bool, str]:
    from accel_pipeline.worker_isolation import ensure_worker_build_dir

    # Pack workers use build-release-{worker_id}; dirs may be missing after disk cleanup.
    try:
        ensure_worker_build_dir(pipeline_config, configure=True)
    except Exception as exc:  # noqa: BLE001 — surface as preflight failure
        return False, str(exc)

    paths = resolve_workload_paths(pipeline_config, workload, query)
    if workload.get("workload_kind") == "aux_state":
        targets = [paths.get("aux_bench_target", ""), paths.get("aux_test_target", "")]
    else:
        targets = [paths["bench_target"], paths["test_target"]]
    targets = [t for t in targets if t]
    combined: list[str] = []
    for target in targets:
        proc = subprocess.run(
            ["cmake", "--build", str(pipeline_config.build_dir), "--target", target, "-j", "4"],
            cwd=pipeline_config.repo_root,
            capture_output=True,
            text=True,
            check=False,
        )
        out = (proc.stdout or "") + (proc.stderr or "")
        combined.append(out[-1500:])
        if proc.returncode != 0:
            return False, "\n".join(combined)
    return True, "\n".join(combined)
