"""Running one gate script: its context file, its result, and the text a failure hands the agent."""

from __future__ import annotations

import atexit
import contextlib
import contextvars
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
from typing import Any, Mapping

from accel_pipeline.config import (
    PipelineConfig,
    engine_host_conventions,
    resolve_workload_paths,
    sanitize_engine_name,
)
from accel_pipeline.engine_host import host_script, inject_host_runtime_into_gate_env, instance_tag
from accel_pipeline.gates.stamps import current_stamp, record_pass
from accel_pipeline.jsonio import read_json
from accel_pipeline.layout import workload_schema_sql
from accel_pipeline.paths import gates_dir_for_query, query_artifacts_dir
from accel_pipeline.session import HEARTBEAT_FILE
from accel_pipeline.test_suite import gate_settings
from accel_pipeline.workload_facts import FACTS_FILE, workload_facts


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
    "aux_correctness_iterate": "run_aux_correctness.sh",
    "quick_check": "run_aux_quick_check.sh",
    "aux_unit": "run_aux_unit_tests.sh",
    "prims_tests": "run_prims_tests.sh",
    "aux_mutation_scenarios": "run_aux_mutation_scenarios.sh",
    "aux_speedup_verification": "run_aux_speedup_verification.sh",
    "aux_bench_sweep": "run_aux_bench_sweep.sh",
    "aux_exit": "run_aux_exit.sh",
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
# The sanitizer gate runs on the small test subset too: races do not need the full dataset, and an instrumented
# build and differential on it took 20+ min per run (past the MCP tool-call limit).
# The iterate tier is the stage-exit gate of an optimization iteration: the full dataset, a lighter suite and fewer
# write probes, one thread count (the design already passed the full tier; the audit runs it again).
GATE_TIERS = {"aux_correctness_quick": "quick", "quick_check": "quick", "aux_tsan": "quick", "aux_correctness_iterate": "iterate"}
# The exit run has a phase on the subset dataset (the sanitizer run) beside the full-tier ones.
SUBSET_DATASET_GATES = frozenset({"aux_exit"})


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


def build_jobs() -> int:
    """Parallel compile jobs a build in this process may use."""
    return max(1, int(os.environ.get("ACCEL_BUILD_JOBS") or len(os.sched_getaffinity(0))))


def jobs_per_gate(parallel_gates: int) -> int:
    """Build/ctest job cap of each of `parallel_gates` gates running at once inside this worker's set."""
    return max(1, len(os.sched_getaffinity(0)) // max(1, parallel_gates))


GATE_TIMEOUT_S = float(os.environ.get("ACCEL_GATE_TIMEOUT", 7200))
CANCEL_POLL_S = 1.0
TOOL_CALL_LIMIT_S = float(os.environ.get("ACCEL_TOOL_CALL_LIMIT", 600))  # the agent client's limit on one tool call
TOOL_CALL_MARGIN_S = 20.0  # the gate is killed this long before the client gives up on the call
_tool_deadline: contextvars.ContextVar[float | None] = contextvars.ContextVar("tool_deadline", default=None)
_live_groups: set[int] = set()  # process groups of running gates, killed if this process exits first


@contextlib.contextmanager
def tool_call_limit(seconds: float = TOOL_CALL_LIMIT_S):
    """Within a tool call: a gate run here is killed (its whole process group) before the client's limit on the call,
    instead of running on as an orphan after the client has given up."""
    token = _tool_deadline.set(time.monotonic() + seconds - TOOL_CALL_MARGIN_S)
    try:
        yield
    finally:
        _tool_deadline.reset(token)


def _kill_live_groups() -> None:
    for pgid in list(_live_groups):
        with contextlib.suppress(OSError):
            os.killpg(pgid, signal.SIGKILL)


atexit.register(_kill_live_groups)


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
    hint = ""
    if (tool_deadline := _tool_deadline.get()) is not None and tool_deadline < deadline:
        limit, deadline = max(0.0, tool_deadline - time.monotonic()), tool_deadline
        hint = ": the tool-call limit; long gates go in the background (request_gate(gate, background=True))"
    _live_groups.add(proc.pid)
    while True:
        try:
            out, err = proc.communicate(timeout=min(CANCEL_POLL_S, max(0.0, deadline - time.monotonic())))
            _live_groups.discard(proc.pid)
            return subprocess.CompletedProcess(proc.args, proc.returncode, out, err)
        except subprocess.TimeoutExpired:
            cancelled = cancel is not None and cancel.is_set()
            if not cancelled and time.monotonic() < deadline:
                continue
        os.killpg(proc.pid, signal.SIGKILL)
        out, err = proc.communicate()
        _live_groups.discard(proc.pid)
        verdict = (
            {"status": "cancelled", "reason": "cancelled: a sibling gate failed"}
            if cancelled
            else {"status": "fail", "reason": f"gate did not finish within {limit:.0f}s{hint}"}
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
    """Run one gate script. `env` adds to the script's environment (gates/parallel.py sets a distinct
    ACCEL_ENGINE_INSTANCE per gate); once `cancel` is set the gate is killed and reports "cancelled".
    `out_dir` puts the result files there instead of <artifacts>/gates (background gates, so a run on
    code that moves on never overwrites the canonical result); `timeout_s` replaces GATE_TIMEOUT_S.
    A passing foreground gate is recorded in gates/stamps.py against the code it ran on."""
    if gate_name not in GATE_SCRIPTS:
        raise ValueError(f"unknown gate: {gate_name}")

    script = pipeline_config.gates_dir / GATE_SCRIPTS[gate_name]
    if not script.is_file():
        raise FileNotFoundError(script)

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
    inject_host_runtime_into_gate_env(
        gate_env, workload, pipeline_config.repo_root, pipeline_config.build_dir
    )
    kind = str(workload.get("workload_kind") or "")
    if kind:
        gate_env["ACCEL_WORKLOAD_KIND"] = kind
    tier = GATE_TIERS.get(gate_name)
    if tier:
        gate_env["ACCEL_GATE_TIER"] = tier
    if tier == "quick" or gate_name in SUBSET_DATASET_GATES:
        from accel_pipeline.test_db import ensure_cut_datasets

        if cuts := ensure_cut_datasets(pipeline_config, workload, query):
            gate_env["ACCEL_SUBSET_DATASET_DIR"] = str(cuts.subset)
    gate_env.update(env or {})

    # Codex may never create the worker build dir. Once the engine has a host.sh, only its
    # `build` verb configures the dir (host_contract.md, $ACCEL_CMAKE_ARGS); a bare configure here
    # would reset the backend switch and the generator, or fight a build that runs in the
    # engine's own environment under another source path.

    engine = pipeline_config.engine_name
    has_host = bool(engine) and host_script(Path(pipeline_config.repo_root), engine).is_file()
    ensure_build_dir(pipeline_config, configure=not has_host)

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
    if gate_name in ("aux_bench_sweep", "aux_exit"):
        from accel_pipeline.pipeline_log import log_bench_sweep_results

        bench = data if gate_name == "aux_bench_sweep" else read_json(Path(data.get("phases", {}).get("bench", {}).get("result", "")))
        if bench:
            log_bench_sweep_results(
                pipeline_config.repo_root,
                query,
                workload,
                gate_status=bench.get("status", status),
                gate_data=bench,
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


# --- the context file a gate script reads


def build_gate_context(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
) -> dict[str, Any]:
    paths = resolve_workload_paths(pipeline_config, workload, query)
    benchmark = str(workload.get("benchmark_name", "tpch"))
    kind = str(workload.get("workload_kind", "base"))
    engine = sanitize_engine_name(str(workload.get("engine_name") or ""))
    host = dict(workload.get("engine_host") or engine_host_conventions(engine, repo_root=pipeline_config.repo_root))
    ctx: dict[str, Any] = {
        "query_number": query,
        "benchmark_name": benchmark,
        "artifact_namespace": str(workload.get("artifact_namespace", benchmark)),
        "artifacts_dir": str(query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)),
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
        "dataset_dir": str(workload.get("dataset_dir") or os.environ.get("ACCEL_DATASET_DIR", "")),
        "schema_sql": workload_schema_sql(str(workload.get("dataset_benchmark") or benchmark)),
        "scale_dataset_dir": str(workload.get("scale_dataset_dir") or ""),
        "scale_db": str(paths.get("scale_db", "")),
        "db_env_var": paths["db_env_var"],
        "ctest_resource_lock": str(workload.get("ctest_resource_lock", f"{benchmark}_db")),
        "build_dir": str(pipeline_config.build_dir),
        "fairness": dict(workload.get("fairness", {})),
        "header_path": paths["header_path"],
        "kernel_path": paths["kernel_path"],
        "package_path": paths["package_path"],
    }
    if kind != "engine_backend":
        ctx["test_suite"] = gate_settings(pipeline_config, workload, query)
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
    if kind != "engine_backend":
        workload_facts(pipeline_config.repo_root, workload, query)
        ctx["workload_facts_path"] = str(Path(ctx["artifacts_dir"]) / FACTS_FILE)
    if kind == "engine_backend":
        ctx.update(
            {
                "target_paths": dict(workload.get("target_paths") or {}),
                "cmake": dict(workload.get("cmake") or {}),
                "placement_candidates": list(workload.get("placement_candidates") or []),
                "requires_in_process_host": bool(workload.get("requires_in_process_host", False)),
                "implementation_layers": list(workload.get("implementation_layers") or []),
                "host_runtime": dict(workload.get("host_runtime") or {}),
                "known_gaps": list(workload.get("known_gaps") or []),
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
    # Gates run in parallel rewrite it: a reader never sees a half-written file.
    scratch = path.with_name(f"{path.name}.{os.getpid()}.{threading.get_ident()}")
    scratch.write_text(json.dumps(build_gate_context(pipeline_config, workload, query), indent=2), encoding="utf-8")
    os.replace(scratch, path)
    return path


# --- failure text for the agent: a gate that abort()s leaves an empty log, so the return code and signal always show

_SIGNAL_NAMES = {
    132: "SIGILL",
    134: "SIGABRT/abort",
    136: "SIGFPE",
    137: "SIGKILL",
    139: "SIGSEGV",
    143: "SIGTERM",
}


def describe_returncode(returncode: int | None) -> str:
    if returncode is None:
        return ""
    if returncode == 0:
        return "exit 0"
    if returncode < 0:
        sig = _SIGNAL_NAMES.get(128 + (-returncode), f"signal {-returncode}")
        return f"killed by {sig} (returncode={returncode})"
    name = _SIGNAL_NAMES.get(int(returncode))
    if name:
        return f"exit {returncode} ({name})"
    return f"exit {returncode}"


def log_tail_for_agent(data: Mapping[str, Any], *, max_chars: int = 8000) -> str:
    """Never-empty failure text for reloop prompts / MCP failure_summary."""
    tail = str(data.get("log_tail") or "").strip()
    if tail:
        return tail[-max_chars:]
    rc = data.get("returncode")
    if rc is None:
        rc = data.get("exit_code")
    bits = ["(no captured stdout/stderr from the gate process)"]
    desc = describe_returncode(int(rc) if rc is not None and str(rc).lstrip("-").isdigit() else None)
    if desc:
        bits.append(desc)
    notes = str(data.get("notes") or data.get("reason") or "").strip()
    if notes:
        bits.append(notes)
    return "\n".join(bits)[-max_chars:]


def enrich_failed_gate_data(
    data: Mapping[str, Any],
    *,
    proc_returncode: int | None = None,
    proc_output: str = "",
) -> dict[str, Any]:
    """Ensure a failing gate JSON always has returncode + non-empty log_tail."""
    out = dict(data)
    status = str(out.get("status") or "").strip().lower()
    if status != "fail":
        return out
    if out.get("returncode") is None and out.get("exit_code") is None:
        if proc_returncode is not None:
            out["returncode"] = proc_returncode
    tail = str(out.get("log_tail") or "").strip()
    extra = str(proc_output or "").strip()
    if not tail and extra:
        out["log_tail"] = extra[-8000:]
    if not str(out.get("log_tail") or "").strip():
        out["log_tail"] = log_tail_for_agent(out)
    return out


def format_reloop_failure_sections(
    data: Mapping[str, Any],
    *,
    max_chars: int = 8000,
) -> list[str]:
    """Markdown sections appended to layer/host-bootstrap gate-fail prompts."""
    rc = data.get("returncode")
    if rc is None:
        rc = data.get("exit_code")
    desc = ""
    if rc is not None and str(rc).lstrip("-").isdigit():
        desc = describe_returncode(int(rc))
    notes = str(data.get("notes") or data.get("reason") or "").strip()
    tail = log_tail_for_agent(data, max_chars=max_chars)
    lines = [
        "",
        "### Notes",
        notes or "(none)",
        "",
        "### returncode",
        desc or "(unset)",
        "",
        "### log_tail",
        "```",
        tail,
        "```",
    ]
    return lines


# --- the build directory the gates build in


def _wipe_cmake_configure_state(bdir: Path) -> None:
    """Remove CMake configure artifacts so ``-S`` can retarget the build dir."""
    cache = bdir / "CMakeCache.txt"
    if cache.is_file() or cache.is_symlink():
        cache.unlink()
    cmake_files = bdir / "CMakeFiles"
    if cmake_files.is_dir():
        shutil.rmtree(cmake_files, ignore_errors=True)
    for name in ("cmake_install.cmake", "Makefile", "compile_commands.json"):
        p = bdir / name
        if p.is_file() or p.is_symlink():
            try:
                p.unlink()
            except OSError:
                pass


def ensure_build_dir(
    pipeline_config: PipelineConfig,
    *,
    configure: bool = True,
) -> Path:
    """Create (and optionally cmake-configure) the build directory.

    Codex may finish implementation without ever running ``cmake -S . -B <build_dir>``; gates must
    still be able to build rather than skip with ``build dir missing``. A cache that points at another
    source tree (a pack worker's git worktree) is wiped and reconfigured, so builds cannot compile
    a peer's mid-edits from the shared checkout.
    """
    bdir = Path(pipeline_config.build_dir)
    bdir.mkdir(parents=True, exist_ok=True)
    if not configure:
        return bdir
    cache = bdir / "CMakeCache.txt"
    src_root = Path(pipeline_config.repo_root).resolve()
    needs_configure = not cache.is_file()
    if cache.is_file():
        try:
            text = cache.read_text(encoding="utf-8", errors="replace")
        except OSError:
            text = ""
        # CMake stores the source tree as CMAKE_HOME_DIRECTORY.
        cached_src = ""
        for line in text.splitlines():
            if line.startswith("CMAKE_HOME_DIRECTORY:"):
                cached_src = line.split("=", 1)[-1].strip()
                break
        if cached_src and Path(cached_src).resolve() != src_root:
            print(
                f"[pipeline] build cache source mismatch "
                f"({cached_src} != {src_root}); reconfiguring {bdir.name}",
                file=sys.stderr,
                flush=True,
            )
            # CMake refuses in-place -S changes while CMakeCache.txt still names
            # the old home directory. Wipe cache only — keep database copies etc.
            _wipe_cmake_configure_state(bdir)
            needs_configure = True
    if not needs_configure:
        return bdir
    print(
        f"[pipeline] configuring worker build dir: {bdir.name} (-S {src_root.name})",
        file=sys.stderr,
        flush=True,
    )
    cmake_env = os.environ.copy()
    proc = subprocess.run(
        ["cmake", "-S", str(src_root), "-B", str(bdir)],
        cwd=str(src_root),
        env=cmake_env,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        tail = ((proc.stdout or "") + (proc.stderr or ""))[-2000:]
        raise RuntimeError(f"cmake configure failed for {bdir}:\n{tail}")
    return bdir
