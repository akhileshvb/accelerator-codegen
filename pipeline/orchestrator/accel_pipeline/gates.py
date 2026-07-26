from __future__ import annotations

import json
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from accel_pipeline.config import PipelineConfig


GATE_SCRIPTS = {
    "scaffold": "scaffold_query.sh",
    "correctness": "run_correctness.sh",
    "baseline": "run_baseline.sh",
    "sf10": "run_sf10.sh",
    "profile": "run_profile.sh",
    "fairness": "check_fairness.sh",
}


@dataclass
class GateResult:
    name: str
    status: str
    data: dict[str, Any]
    path: Path | None = None


def run_gate(
    pipeline_config: PipelineConfig,
    gate_name: str,
    query: int,
) -> GateResult:
    if gate_name not in GATE_SCRIPTS:
        raise ValueError(f"unknown gate: {gate_name}")

    script = pipeline_config.gates_dir / GATE_SCRIPTS[gate_name]
    if not script.is_file():
        raise FileNotFoundError(script)

    proc = subprocess.run(
        ["bash", str(script), str(query)],
        cwd=pipeline_config.repo_root,
        capture_output=True,
        text=True,
        check=False,
    )

    out_json = pipeline_config.repo_root / "pipeline" / "artifacts" / f"q{query}" / "gates" / f"{gate_name}.json"
    data: dict[str, Any] = {}
    if out_json.is_file():
        data = json.loads(out_json.read_text(encoding="utf-8"))
    elif proc.stdout.strip():
        try:
            data = json.loads(proc.stdout.strip().splitlines()[-1])
        except json.JSONDecodeError:
            data = {"stdout": proc.stdout[-2000:], "stderr": proc.stderr[-2000:]}

    status = data.get("status", "pass" if proc.returncode == 0 else "fail")
    # TODO: Parse benchmark stdout/CSV into baseline_summary.json and profile_summary.json.
    return GateResult(name=gate_name, status=status, data=data, path=out_json if out_json.is_file() else None)
