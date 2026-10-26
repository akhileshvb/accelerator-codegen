"""Fast correctness probe after smoke, before benchmark."""

from __future__ import annotations

import os
import subprocess
from dataclasses import dataclass

from accel_pipeline.config import PipelineConfig, resolve_workload_paths
from accel_pipeline.optimization_smoke import _smoke_regex


@dataclass
class ProbeResult:
    passed: bool
    output: str = ""
    regex: str = ""
    diff_summary: str = ""


def run_optimization_probe(
    pipeline_config: PipelineConfig,
    workload: dict,
    query: int,
    *,
    timeout_sec: int = 120,
) -> ProbeResult:
    """Run a fast correctness probe (ctest smoke regex) before quick bench."""
    paths = resolve_workload_paths(pipeline_config, workload, query)
    regex = _smoke_regex(workload, paths)
    if not regex:
        return ProbeResult(passed=False, output="no probe regex configured", regex="")

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
    diff_summary = _summarize_probe_failure(output)
    return ProbeResult(
        passed=proc.returncode == 0,
        output=output[-6000:],
        regex=regex,
        diff_summary=diff_summary,
    )


def _summarize_probe_failure(output: str) -> str:
    if not output:
        return ""
    markers = ("FAILED", "error:", "Assertion", "NULL", "mismatch", "expected")
    lines = [line.strip() for line in output.splitlines() if any(m in line for m in markers)]
    return "\n".join(lines[-12:])
