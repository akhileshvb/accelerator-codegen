from __future__ import annotations

import json
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from accel_pipeline.config import PipelineConfig
from accel_pipeline.gate_context import write_gate_context
from accel_pipeline.paths import gates_dir_for_query, query_artifacts_dir


GATE_SCRIPTS = {
    "scaffold": "scaffold_query.sh",
    "correctness": "run_correctness.sh",
    "baseline": "run_baseline.sh",
    "sf10": "run_sf10.sh",
    "profile": "run_profile.sh",
    "fairness": "check_fairness.sh",
    "aux_correctness": "run_aux_correctness.sh",
    "aux_unit": "run_aux_unit_tests.sh",
    "aux_mutation_scenarios": "run_aux_mutation_scenarios.sh",
    "aux_speedup_verification": "run_aux_speedup_verification.sh",
    "aux_bench_sweep": "run_aux_bench_sweep.sh",
    "aux_cross_accel": "run_aux_cross_accel.sh",
    "aux_multi_connection": "run_aux_multi_connection.sh",
    "aux_cost_model_calibrate": "run_aux_cost_model_calibrate.sh",
}


@dataclass
class GateResult:
    name: str
    status: str
    data: dict[str, Any]
    path: Path | None = None


class GateFailure(RuntimeError):
    """Raised when a required gate does not pass."""


GATES_ALLOW_SKIP = frozenset({"baseline", "sf10", "profile"})


def gate_failure_message(result: GateResult) -> str:
    data = result.data
    detail = str(data.get("notes") or data.get("reason") or "").strip()
    if not detail and data.get("log_tail"):
        detail = str(data["log_tail"]).strip()[-500:]
    if detail:
        return f"gate {result.name} {result.status}: {detail}"
    return f"gate {result.name} {result.status}"


def require_gate_pass(result: GateResult, *, allow_skip: bool = False) -> None:
    if result.status == "pass":
        return
    if result.status == "skip" and allow_skip:
        return
    raise GateFailure(gate_failure_message(result))


def _gate_output_json(
    pipeline_config: PipelineConfig,
    gate_name: str,
    query: int,
    workload: dict[str, Any] | None,
) -> Path:
    if workload:
        art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
        return gates_dir_for_query(art) / f"{gate_name}.json"
    return (
        pipeline_config.repo_root
        / "pipeline"
        / "artifacts"
        / f"q{query}"
        / "gates"
        / f"{gate_name}.json"
    )


def run_gate(
    pipeline_config: PipelineConfig,
    gate_name: str,
    query: int,
    workload: dict | None = None,
    *,
    raise_on_failure: bool = False,
    allow_skip: bool | None = None,
    change_kind: str = "",
    iteration: int = 0,
) -> GateResult:
    if gate_name not in GATE_SCRIPTS:
        raise ValueError(f"unknown gate: {gate_name}")

    script = pipeline_config.gates_dir / GATE_SCRIPTS[gate_name]
    if not script.is_file():
        raise FileNotFoundError(script)

    if workload:
        write_gate_context(pipeline_config, workload, query)

    proc = subprocess.run(
        ["bash", str(script), str(query)],
        cwd=pipeline_config.repo_root,
        capture_output=True,
        text=True,
        check=False,
    )

    out_json = _gate_output_json(pipeline_config, gate_name, query, workload)
    data: dict[str, Any] = {}
    if out_json.is_file():
        data = json.loads(out_json.read_text(encoding="utf-8"))
    elif proc.stdout.strip():
        try:
            data = json.loads(proc.stdout.strip().splitlines()[-1])
        except json.JSONDecodeError:
            data = {"stdout": proc.stdout[-2000:], "stderr": proc.stderr[-2000:]}

    status = data.get("status", "pass" if proc.returncode == 0 else "fail")
    result = GateResult(name=gate_name, status=status, data=data, path=out_json if out_json.is_file() else None)
    if gate_name == "aux_bench_sweep":
        from accel_pipeline.pipeline_log import log_bench_sweep_results

        log_bench_sweep_results(
            pipeline_config.repo_root,
            query,
            gate_status=status,
            gate_data=data,
        )
    if gate_name in {"correctness", "baseline", "profile"}:
        from accel_pipeline.pipeline_log import log_base_bench_gate

        log_base_bench_gate(
            pipeline_config.repo_root,
            query,
            gate_name=gate_name,
            gate_status=status,
            gate_data=data,
            change_kind=change_kind,
            iteration=iteration,
            workload=workload,
        )
    if raise_on_failure:
        skip_ok = allow_skip if allow_skip is not None else gate_name in GATES_ALLOW_SKIP
        require_gate_pass(result, allow_skip=skip_ok)
    return result
