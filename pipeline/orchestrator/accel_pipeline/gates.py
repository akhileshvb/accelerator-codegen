from __future__ import annotations

import json
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from accel_pipeline.config import PipelineConfig
from accel_pipeline.gate_context import write_gate_context
from accel_pipeline.paths import gates_dir_for_query, query_artifacts_dir


GATE_SCRIPTS = {
    "scaffold": "scaffold_query.sh",
    "host_bootstrap": "run_host_bootstrap.sh",
    "correctness": "run_correctness.sh",
    "baseline": "run_baseline.sh",
    "sf10": "run_sf10.sh",
    "profile": "run_profile.sh",
    "fairness": "check_fairness.sh",
    "aux_correctness": "run_aux_correctness.sh",
    "aux_unit": "run_aux_unit_tests.sh",
    "prims_tests": "run_prims_tests.sh",
    "aux_mutation_scenarios": "run_aux_mutation_scenarios.sh",
    "aux_speedup_verification": "run_aux_speedup_verification.sh",
    "aux_bench_sweep": "run_aux_bench_sweep.sh",
    "aux_cross_accel": "run_aux_cross_accel.sh",
    "aux_multi_connection": "run_aux_multi_connection.sh",
    "aux_tsan": "run_aux_tsan.sh",
    "aux_cost_model_calibrate": "run_aux_cost_model_calibrate.sh",
    "aux_optimization_probe": "run_aux_optimization_probe.sh",
    "engine_build": "run_engine_build.sh",
    "engine_scan_conformance": "run_engine_scan_conformance.sh",
    "engine_api_conformance": "run_engine_api_conformance.sh",
    "engine_contract": "run_engine_contract.sh",
    "engine_adversarial": "run_engine_adversarial.sh",
    "engine_aux_smoke": "run_engine_aux_smoke.sh",
    "engine_capture": "run_engine_layer.sh",
    "engine_epochs": "run_engine_layer.sh",
    "engine_scan": "run_engine_layer.sh",
    "engine_runtime": "run_engine_layer.sh",
    "engine_host": "run_engine_layer.sh",
    "slot_epoch_stress": "run_slot_gates.sh",
    "slot_hits_under_writes": "run_slot_gates.sh",
    "slot_read_after_commit": "run_slot_gates.sh",
    "slot_txn_edges": "run_slot_gates.sh",
    "slot_restart": "run_slot_gates.sh",
    "slot_own_writes": "run_slot_gates.sh",
    "slot_ddl_events": "run_slot_gates.sh",
    "slot_tsan": "run_aux_tsan.sh",
    "slot_differential": "run_slot_gates.sh",
}


@dataclass
class GateResult:
    name: str
    status: str
    data: dict[str, Any]
    path: Path | None = None


class GateFailure(RuntimeError):
    """Raised when a required gate does not pass."""


GATES_ALLOW_SKIP = frozenset(
    {
        "baseline",
        "sf10",
        "profile",
        "aux_cost_model_calibrate",
        "aux_speedup_verification",
        "engine_scan_conformance",
        "engine_aux_smoke",
        "engine_host",
        "slot_restart",
        "slot_own_writes",
        "slot_ddl_events",
        "aux_tsan",
        "slot_tsan",
    }
)


def gate_failure_message(result: GateResult) -> str:
    from accel_pipeline.gate_capture import describe_returncode, log_tail_for_agent

    data = result.data
    detail = str(data.get("notes") or data.get("reason") or "").strip()
    tail = log_tail_for_agent(data, max_chars=500)
    rc = data.get("returncode")
    if rc is None:
        rc = data.get("exit_code")
    rc_bit = ""
    if rc is not None and str(rc).lstrip("-").isdigit():
        rc_bit = describe_returncode(int(rc))
    parts = [p for p in (detail, rc_bit, tail) if p]
    # De-dupe if tail already contains notes.
    uniq: list[str] = []
    for part in parts:
        if not any(part in seen for seen in uniq):
            uniq.append(part)
    if uniq:
        return f"gate {result.name} {result.status}: " + " | ".join(uniq)
    return f"gate {result.name} {result.status}"


def require_gate_pass(result: GateResult, *, allow_skip: bool = False) -> None:
    if result.status == "pass":
        return
    if result.status == "skip" and allow_skip:
        return
    raise GateFailure(gate_failure_message(result))


def run_gate(
    pipeline_config: PipelineConfig,
    gate_name: str,
    query: int,
    workload: dict,
    *,
    raise_on_failure: bool = False,
    allow_skip: bool | None = None,
    change_kind: str = "",
    iteration: int = 0,
    gate_index: int = 0,
    gate_count: int = 0,
) -> GateResult:
    if gate_name not in GATE_SCRIPTS:
        raise ValueError(f"unknown gate: {gate_name}")

    script = pipeline_config.gates_dir / GATE_SCRIPTS[gate_name]
    if not script.is_file():
        raise FileNotFoundError(script)

    import os

    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    out_json = gates_dir_for_query(art) / f"{gate_name}.json"
    out_json.parent.mkdir(parents=True, exist_ok=True)

    gate_env = os.environ.copy()
    if change_kind:
        gate_env["ACCEL_GATE_CHANGE_KIND"] = change_kind
    gate_env["ACCEL_GATE_ITERATION"] = str(iteration)
    # Per-worker build dir for concurrent packs (shared tree + separate builds).
    build_dir = str(pipeline_config.build_dir)
    gate_env["ACCEL_PIPELINE_BUILD_DIR"] = build_dir
    gate_env["ACCEL_BUILD_DIR"] = build_dir
    if pipeline_config.worker_id:
        gate_env["ACCEL_PIPELINE_WORKER_ID"] = pipeline_config.worker_id
    # Engine gates (and others that honor these) need explicit paths — writing
    # _gate_context.json alone is not enough; scripts read ACCEL_GATE_*.
    gate_env["ACCEL_GATE_OUT"] = str(out_json)
    gate_env["ACCEL_GATE_NAME"] = gate_name
    ctx_path = write_gate_context(pipeline_config, workload, query)
    gate_env["ACCEL_GATE_CONTEXT"] = str(ctx_path)
    gate_env["ACCEL_AUX_BENCHMARK"] = str(workload.get("benchmark_name") or "tpch")
    gate_env["ACCEL_PIPELINE_BENCHMARK"] = gate_env["ACCEL_AUX_BENCHMARK"]
    engine_name = str(workload.get("engine_name") or "").strip().lower()
    if engine_name:
        gate_env["ACCEL_ENGINE_NAME"] = engine_name
    from accel_pipeline.host_runtime import inject_host_runtime_into_gate_env

    inject_host_runtime_into_gate_env(gate_env, workload)
    kind = str(workload.get("workload_kind") or "")
    if kind:
        gate_env["ACCEL_WORKLOAD_KIND"] = kind

    # Codex may never create the worker build dir; configure it before gates.
    from accel_pipeline.worker_isolation import ensure_worker_build_dir

    ensure_worker_build_dir(pipeline_config, configure=True)

    from accel_pipeline.pipeline_log import log_gate_complete, log_gate_start

    log_gate_start(
        gate_name,
        query,
        change_kind=change_kind,
        iteration=iteration,
        index=gate_index,
        count=gate_count,
    )
    started = time.monotonic()
    proc = subprocess.run(
        ["bash", str(script), str(query)],
        cwd=pipeline_config.repo_root,
        capture_output=True,
        text=True,
        check=False,
        env=gate_env,
    )
    duration_ms = int((time.monotonic() - started) * 1000)
    data: dict[str, Any] = {}
    if out_json.is_file():
        data = json.loads(out_json.read_text(encoding="utf-8"))
    elif proc.stdout.strip():
        try:
            data = json.loads(proc.stdout.strip().splitlines()[-1])
        except json.JSONDecodeError:
            data = {"stdout": proc.stdout[-2000:], "stderr": proc.stderr[-2000:]}

    status = data.get("status", "pass" if proc.returncode == 0 else "fail")
    if "status" not in data:
        data["status"] = status
    if status == "fail":
        from accel_pipeline.gate_capture import enrich_failed_gate_data

        data = enrich_failed_gate_data(
            data,
            proc_returncode=proc.returncode,
            proc_output=((proc.stdout or "") + (proc.stderr or "")),
        )
        try:
            out_json.parent.mkdir(parents=True, exist_ok=True)
            out_json.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        except OSError:
            pass
    detail = str(data.get("reason") or data.get("notes") or "").strip()
    if not detail and status == "fail" and proc.stderr.strip():
        detail = proc.stderr.strip()[-200:]
    log_gate_complete(
        gate_name,
        status=status,
        duration_ms=duration_ms,
        change_kind=change_kind,
        iteration=iteration,
        index=gate_index,
        count=gate_count,
        detail=detail,
    )
    result = GateResult(name=gate_name, status=status, data=data, path=out_json if out_json.is_file() else None)
    if gate_name == "aux_bench_sweep":
        from accel_pipeline.pipeline_log import log_bench_sweep_results

        log_bench_sweep_results(
            pipeline_config.repo_root,
            query,
            workload,
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
