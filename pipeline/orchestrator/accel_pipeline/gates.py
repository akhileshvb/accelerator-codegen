from __future__ import annotations

import json
import os
import shutil
import signal
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from accel_pipeline.config import PipelineConfig
from accel_pipeline.engine_host import instance_tag
from accel_pipeline.gate_context import write_gate_context
from accel_pipeline.gate_stamps import current_stamp, record_pass
from accel_pipeline.paths import gates_dir_for_query, query_artifacts_dir
from accel_pipeline.session import HEARTBEAT_FILE


GATE_SCRIPTS = {
    "scaffold": "scaffold_query.sh",
    "host_bootstrap": "run_host_bootstrap.sh",
    "correctness": "run_correctness.sh",
    "baseline": "run_baseline.sh",
    "sf10": "run_sf10.sh",
    "profile": "run_profile.sh",
    "fairness": "check_fairness.sh",
    "test_suite_validate": "run_test_suite_validate.sh",
    "aux_correctness": "run_aux_correctness.sh",
    "aux_correctness_quick": "run_aux_correctness.sh",
    "quick_check": "run_aux_quick_check.sh",
    "aux_unit": "run_aux_unit_tests.sh",
    "prims_tests": "run_prims_tests.sh",
    "aux_mutation_scenarios": "run_aux_mutation_scenarios.sh",
    "aux_speedup_verification": "run_aux_speedup_verification.sh",
    "aux_bench_sweep": "run_aux_bench_sweep.sh",
    "aux_cross_accel": "run_aux_cross_accel.sh",
    "aux_multi_connection": "run_aux_multi_connection.sh",
    "aux_tsan": "run_aux_tsan.sh",
    "module_tsan": "run_module_tsan.sh",
    "aux_cost_model_calibrate": "run_aux_cost_model_calibrate.sh",
    "aux_optimization_probe": "run_aux_optimization_probe.sh",
    "engine_build": "run_engine_build.sh",
    "engine_scan_conformance": "run_engine_scan_conformance.sh",
    "engine_api_conformance": "run_engine_api_conformance.sh",
    "engine_hostctl": "run_engine_hostctl.sh",
    "engine_contract": "run_engine_contract.sh",
    "engine_adversarial": "run_engine_adversarial.sh",
    "engine_hit_contract": "run_engine_hit_contract.sh",
    "engine_quick": "run_engine_quick.sh",
    "engine_canary": "run_engine_canary.sh",
    "engine_canary_asan": "run_engine_canary.sh",
    "engine_canary_tsan": "run_engine_canary.sh",
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
}


# Gates that run a cheaper tier of another gate's script; each has its own result file.
GATE_TIERS = {"aux_correctness_quick": "quick", "quick_check": "quick"}


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
        "engine_hit_contract",
        "engine_canary",
        "engine_canary_asan",
        "engine_canary_tsan",
        "slot_restart",
        "aux_tsan",
        "module_tsan",
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


GATE_TIMEOUT_S = float(os.environ.get("ACCEL_GATE_TIMEOUT", 7200))
CANCEL_POLL_S = 1.0


def read_phase_log(gate_json: Path) -> dict[str, float]:
    """Seconds per phase from the sidecar the gate script's EXIT trap wrote (repeated phases add up)."""
    phases: dict[str, float] = {}
    try:
        lines = gate_json.with_suffix(".phases").read_text(encoding="utf-8").splitlines()
    except OSError:
        return phases
    for line in lines:
        name, _, seconds = line.partition("=")
        if seconds:
            phases[name] = round(phases.get(name, 0.0) + float(seconds), 3)
    return phases


def _run_gate_script(
    script: Path, query: int, cwd: Path, env: dict[str, str],
    cancel: threading.Event | None = None, timeout_s: float | None = None,
) -> subprocess.CompletedProcess[str]:
    """Run a gate script in its own process group; past the timeout (GATE_TIMEOUT_S), or once `cancel` is set,
    the whole group is killed (a hung harness and its children) and the gate fails with the reason."""
    proc = subprocess.Popen(
        ["bash", str(script), str(query)], cwd=cwd, env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True,
    )
    limit = GATE_TIMEOUT_S if timeout_s is None else timeout_s
    deadline = time.monotonic() + limit
    while True:
        try:
            out, err = proc.communicate(timeout=min(CANCEL_POLL_S, max(0.0, deadline - time.monotonic())))
            return subprocess.CompletedProcess(proc.args, proc.returncode, out, err)
        except subprocess.TimeoutExpired:
            cancelled = cancel is not None and cancel.is_set()
            if not cancelled and time.monotonic() < deadline:
                continue
        os.killpg(proc.pid, signal.SIGKILL)
        out, err = proc.communicate()
        verdict = (
            {"status": "cancelled", "reason": "cancelled: a sibling gate failed"}
            if cancelled
            else {"status": "fail", "reason": f"gate did not finish within {limit:.0f}s"}
        )
        return subprocess.CompletedProcess(proc.args, proc.returncode, out + "\n" + json.dumps(verdict), err)


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
    env: dict[str, str] | None = None,
    cancel: threading.Event | None = None,
    out_dir: Path | None = None,
    timeout_s: float | None = None,
) -> GateResult:
    """Run one gate script. `env` adds to the script's environment (gate_parallel sets a distinct
    ACCEL_ENGINE_INSTANCE per gate); once `cancel` is set the gate is killed and reports "cancelled".
    `out_dir` puts the result files there instead of <artifacts>/gates (background gates, so a run on
    code that moves on never overwrites the canonical result); `timeout_s` replaces GATE_TIMEOUT_S.
    A passing foreground gate is recorded in gate_stamps against the code it ran on."""
    if gate_name not in GATE_SCRIPTS:
        raise ValueError(f"unknown gate: {gate_name}")

    script = pipeline_config.gates_dir / GATE_SCRIPTS[gate_name]
    if not script.is_file():
        raise FileNotFoundError(script)

    import os

    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    out_json = (out_dir or gates_dir_for_query(art)) / f"{gate_name}.json"
    out_json.parent.mkdir(parents=True, exist_ok=True)
    out_json.with_suffix(".phases").unlink(missing_ok=True)
    out_json.unlink(missing_ok=True)  # a gate that dies before writing never reads as its last result

    gate_env = os.environ.copy()
    # Gate scripts' `python3` is the pipeline's own interpreter (its packages), not the system one.
    gate_env["PATH"] = f"{Path(sys.executable).parent}{os.pathsep}{gate_env.get('PATH', '')}"
    if change_kind:
        gate_env["ACCEL_GATE_CHANGE_KIND"] = change_kind
    gate_env["ACCEL_GATE_ITERATION"] = str(iteration)
    gate_env["ACCEL_PIPELINE_PYTHONPATH"] = str(Path(__file__).resolve().parents[1])
    # Per-worker build dir for concurrent packs (shared tree + separate builds).
    build_dir = str(pipeline_config.build_dir)
    gate_env["ACCEL_PIPELINE_BUILD_DIR"] = build_dir
    gate_env["ACCEL_BUILD_DIR"] = build_dir
    if pipeline_config.worker_id:
        gate_env["ACCEL_PIPELINE_WORKER_ID"] = pipeline_config.worker_id
    gate_env["ACCEL_INSTANCE_TAG"] = instance_tag(pipeline_config.worker_id, pipeline_config.build_dir)
    gate_env["ACCEL_HEARTBEAT_FILE"] = str(art / HEARTBEAT_FILE)  # queueing for the measurement lease beats it
    # Engine gates (and others that honor these) need explicit paths — writing
    # _gate_context.json alone is not enough; scripts read ACCEL_GATE_*.
    gate_env["ACCEL_GATE_OUT"] = str(out_json)
    gate_env["ACCEL_GATE_NAME"] = gate_name
    ctx_path = write_gate_context(pipeline_config, workload, query)
    if out_dir:
        ctx_path = Path(shutil.copy(ctx_path, out_dir / ctx_path.name))
        gate_env["ACCEL_GATE_OUT_DIR"] = str(out_dir)
    gate_env["ACCEL_GATE_CONTEXT"] = str(ctx_path)
    gate_env["ACCEL_AUX_BENCHMARK"] = str(workload.get("benchmark_name") or "tpch")
    gate_env["ACCEL_PIPELINE_BENCHMARK"] = gate_env["ACCEL_AUX_BENCHMARK"]
    engine_name = str(workload.get("engine_name") or "").strip().lower()
    if engine_name:
        gate_env["ACCEL_ENGINE_NAME"] = engine_name
    from accel_pipeline.engine_host import inject_host_runtime_into_gate_env

    inject_host_runtime_into_gate_env(
        gate_env, workload, pipeline_config.repo_root, pipeline_config.build_dir
    )
    kind = str(workload.get("workload_kind") or "")
    if kind:
        gate_env["ACCEL_WORKLOAD_KIND"] = kind
    tier = GATE_TIERS.get(gate_name)
    if tier:
        from accel_pipeline.quick import ensure_subset_dataset

        gate_env["ACCEL_GATE_TIER"] = tier
        subset = ensure_subset_dataset(pipeline_config, workload)
        if subset:
            gate_env["ACCEL_SUBSET_DATASET_DIR"] = str(subset)
    gate_env.update(env or {})

    # Codex may never create the worker build dir. Once the engine has a host.sh, only its
    # `build` verb configures the dir (host_contract.md, $ACCEL_CMAKE_ARGS); a bare configure here
    # would reset the backend switch and the generator, or fight a build that runs in the
    # engine's own environment under another source path.
    from accel_pipeline.engine_host import host_script
    from accel_pipeline.worker_isolation import ensure_worker_build_dir

    engine = pipeline_config.engine_name
    has_host = bool(engine) and host_script(Path(pipeline_config.repo_root), engine).is_file()
    ensure_worker_build_dir(pipeline_config, configure=not has_host)

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
    stamp_before = current_stamp(pipeline_config, workload, query)
    limits = {"cancel": cancel, "timeout_s": timeout_s}
    proc = _run_gate_script(
        script, query, pipeline_config.repo_root, gate_env, **{k: v for k, v in limits.items() if v is not None}
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

    data["phase_timings"] = {**read_phase_log(out_json), "gate_wall": round(duration_ms / 1000, 3)}
    if out_json.is_file():
        out_json.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    status = data.get("status", "pass" if proc.returncode == 0 else "fail")
    if proc.returncode != 0 and status == "pass" and not out_json.is_file():
        # A script that failed after printing an early pass line has not passed.
        status = "fail"
        data["reason"] = f"gate script exited {proc.returncode} after reporting pass"
        data["stderr"] = (proc.stderr or "")[-2000:]
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
        phase_timings=data["phase_timings"],
    )
    result = GateResult(name=gate_name, status=status, data=data, path=out_json if out_json.is_file() else None)
    unchanged = bool(stamp_before) and current_stamp(pipeline_config, workload, query) == stamp_before
    if status == "pass" and unchanged and out_dir is None:
        record_pass(pipeline_config, workload, query, gate_name, stamp_before, change_kind=change_kind, result=out_json)
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
