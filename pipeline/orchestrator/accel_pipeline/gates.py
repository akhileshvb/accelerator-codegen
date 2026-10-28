"""Running a gate, its result, the stamps that say it already passed, and the quick check that wraps the gates.

A gate is a script under pipeline/gates/ run with a context file (build_gate_context); its result (GateResult)
and the text a failure hands the agent come back from run_gate. run_gates_parallel runs a batch fast: cheap
likely-to-fail gates first, independent gates in parallel; it returns one GateResult per name, in the order
given. The caller decides what a result means (require_gate_pass); a gate that was not run because an earlier one
failed has status "cancelled".

Background gates keep running while the agent keeps working. request_gate(background=True) starts one
(start_background_gate, which runs `python -m accel_pipeline.gates`); gate_status reads it (background_status).
At stage exit the orchestrator asks reusable_background_result before running a gate: a passing background run is
reused only if the code stamp is the same at its start, at its end and now. Background state:
<artifacts>/gates/bg/<gate>.json; the gate's own files go under <artifacts>/gates/bg/<gate>/, so a background run
never overwrites the canonical gate result. Each background gate runs on its own engine instance; builds share the
build dir under build_lock.

Stamps: run_gate records the code stamp (layout.code_stamp) of every passing gate; reusable_gate_result is how a
caller skips a gate. A pass also carries the check version (check_version): the digest of the files that define
the checks (gate scripts, harness), which the code stamp leaves out. Timing gates (TIMING_GATES) measure acceptance
numbers: they are reused only from a run of the same stage's acceptance tier, never from another stage and never
from the iteration tier. The exit run is reused by any stage's acceptance tier, as the code is unchanged.

The quick check is the seconds-level inner loop: incremental build, the query's tests, a subset differential.
run_quick_check runs the quick_check gate on the test databases (data.ensure_cut_datasets) and turns its result
into a QuickResult: correctness, and on a plain pass the compact report (build time and scaling, read latency,
write cost per kind, perf counters). Its measurements run the aux bench binary through the host's `run` verb in the
engine instance the gate just loaded the subset into: read latency against the engine's own SQL for the same
bindings, `perf stat` counters of the read loop and of the build, and `perf record` samples of the read, write and
build paths. The write cost (write_cost section) scales the differential harness's per-write timings on the subset
to the full dataset and sums what the full aux_correctness gate would spend on writes: conservative and linear
where it cannot tell (see project()); builds are projected by build_report on a subset big enough to measure.
"""
from __future__ import annotations

import argparse
import atexit
import contextlib
import contextvars
import fcntl
import hashlib
import json
import math
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from collections.abc import Callable, Iterable
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterator, Literal, Mapping, Sequence

from accel_pipeline.config import (
    apply_workload_overrides,
    benchmark_of,
    load_pipeline_config,
    load_workload,
    engine_host_conventions,
    FACTS_FILE,
    gates_dir_for_query,
    HEARTBEAT_FILE,
    load_query_analysis,
    PipelineConfig,
    query_artifacts_dir,
    read_json,
    resolve_workload_paths,
    sanitize_engine_name,
    TOOL_CALL_LIMIT_S,
    workload_facts,
)
from accel_pipeline.data import (
    CutDatasets,
    MANIFEST,
    read_yaml,
    statements,
    txn_files,
    txn_profiles,
    CutError,
    dependent_tables,
    ensure_cut_datasets,
    ensure_dataset_loaded,
    gate_settings,
    host_info,
    host_script,
    inject_host_runtime_into_gate_env,
    run_memory_budget,
    instance_env,
    instance_tag,
    parse_aux_benchmark_output,
    run_host_verb,
    suite_path,
    workload_dataset_dir,
)
from accel_pipeline.layout import DATA, code_stamp, query_dir, workload_schema_sql
from accel_pipeline.repair import Failure, render_evidence, render_failure
from accel_pipeline.runlog import (
    log_base_bench_gate,
    log_bench_sweep_results,
    log_gate_batch_start,
    log_gate_complete,
    log_gate_start,
)


GATE_SCRIPTS = {
    "scaffold": "scaffold_query.sh",
    "host_bootstrap": "run_host_bootstrap.sh",
    "harness_conformance": "run_harness_conformance.sh",
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
GATE_TIERS = {"aux_correctness_quick": "quick", "quick_check": "quick", "aux_tsan": "quick", "aux_correctness_iterate": "iterate",
              "harness_conformance": "quick", "host_bootstrap": "quick"}
# The exit run has a phase on the subset dataset (the sanitizer run) beside the full-tier ones.
SUBSET_DATASET_GATES = frozenset({"aux_exit"})


@dataclass
class GateResult:
    name: str
    status: str
    data: dict[str, Any]
    path: Path | None = None


class GateFailure(RuntimeError):
    """Raised when a required gate does not pass; `result` is that gate's, with the owner it named."""

    def __init__(self, result: "GateResult") -> None:
        super().__init__(gate_failure_message(result))
        self.result = result


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
        "module_tsan",  # skips when the engine-free TSAN driver has no case for the query (aux_tsan covers it)
        "slot_tsan",
        "slot_own_writes",
        "slot_ddl_events",
    }
)


def gate_failure_message(result: GateResult) -> str:
    """One line: the gate, its reason and how its run ended (the traces go in render_evidence)."""
    parts = [p for p in (_reason(result.data), describe_returncode(_exit_code(result.data))) if p]
    return f"gate {result.name} {result.status}" + (": " + " | ".join(parts) if parts else "")


def _reason(data: Mapping[str, Any]) -> str:
    return str(data.get("reason") or data.get("notes") or "").strip()


def _exit_code(data: Mapping[str, Any]) -> int | None:
    rc = data.get("returncode", data.get("exit_code"))
    return int(rc) if rc is not None and str(rc).lstrip("-").isdigit() else None


def require_gate_pass(result: GateResult, *, allow_skip: bool = False) -> None:
    if result.status == "pass":
        return
    if result.status == "skip" and allow_skip:
        return
    raise GateFailure(result)


def build_jobs() -> int:
    """Parallel compile jobs a build in this process may use."""
    return max(1, int(os.environ.get("ACCEL_BUILD_JOBS") or len(os.sched_getaffinity(0))))


def jobs_per_gate(parallel_gates: int) -> int:
    """Build/ctest job cap of each of `parallel_gates` gates running at once inside this worker's set."""
    return max(1, len(os.sched_getaffinity(0)) // max(1, parallel_gates))


GATE_TIMEOUT_S = float(os.environ.get("ACCEL_GATE_TIMEOUT", 7200))
CANCEL_POLL_S = 1.0
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
    for sidecar in (".phases", ".run.json", ".run.json.stacks"):
        out_json.with_suffix(sidecar).unlink(missing_ok=True)
    out_json.unlink(missing_ok=True)  # a gate that dies before writing never reads as its last result

    tier = GATE_TIERS.get(gate_name)
    subset = None
    if tier == "quick" or gate_name in SUBSET_DATASET_GATES:
        try:
            subset = ensure_cut_datasets(pipeline_config, workload, query).subset
        except CutError as exc:  # the data's failure: the gate does not run on a dataset it was not meant for
            result = GateResult(gate_name, "fail", {"gate": gate_name, "status": "fail", "owner": DATA, "reason": str(exc)})
            out_json.write_text(json.dumps(result.data, indent=2) + "\n", encoding="utf-8")
            if raise_on_failure:
                require_gate_pass(result)
            return result

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
    if tier:
        gate_env["ACCEL_GATE_TIER"] = tier
    if subset:
        gate_env["ACCEL_SUBSET_DATASET_DIR"] = str(subset)
    gate_env.update(env or {})

    # Codex may never create the worker build dir. Once the engine has a host.sh, only its
    # `build` verb configures the dir (host_contract.md, $ACCEL_CMAKE_ARGS); a bare configure here
    # would reset the backend switch and the generator, or fight a build that runs in the
    # engine's own environment under another source path.

    engine = pipeline_config.engine_name
    has_host = bool(engine) and host_script(Path(pipeline_config.repo_root), engine).is_file()
    ensure_build_dir(pipeline_config, configure=not has_host)


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
    benchmark = benchmark_of(workload)
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


def tail_marked(text: str, n: int = 6000) -> str:
    return text if len(text) <= n else "... [truncated]\n" + text[-n:]


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

    head = " | ".join(p for p in (_reason(data), describe_returncode(_exit_code(data))) if p)
    return ["", head or "(no reason recorded)",
            *render_evidence(str(data.get("stacks") or ""), log_tail_for_agent(data, max_chars=max_chars),
                             max_chars=max_chars)]


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


@dataclass(frozen=True)
class GateGroup:
    """Gates that run together. "serial": one after the other; "parallel": at once, each on its own
    engine instance (the instance loads its dataset once, then stays warm)."""

    names: tuple[str, ...]
    mode: Literal["serial", "parallel"]


# Order matters: failures surface in the cheap gates before the minutes-long ones start.
DEFAULT_GROUPS: tuple[GateGroup, ...] = (
    GateGroup(("aux_unit", "aux_correctness", "aux_correctness_iterate", "prims_tests"), "serial"),
    GateGroup(("aux_exit",), "serial"),  # runs its own phases side by side
    GateGroup(("aux_multi_connection", "aux_cross_accel", "aux_cost_model_calibrate"), "parallel"),
    GateGroup(("aux_bench_sweep", "aux_tsan", "module_tsan", "slot_tsan"), "serial"),
)


def _ok(result: GateResult) -> bool:
    return result.status == "pass" or (result.status == "skip" and result.name in GATES_ALLOW_SKIP)


def first_failure(results: Sequence[GateResult]) -> GateResult | None:
    """The first result that is neither a pass nor cancelled: what a batch failed on."""
    return next((r for r in results if not _ok(r) and r.status != "cancelled"), None)


def _cancelled(name: str, culprit: str) -> GateResult:
    return GateResult(name=name, status="cancelled", data={"status": "cancelled", "reason": f"not run: {culprit} failed"})


def _instance_env(workload: dict[str, Any], tag: str, name: str, jobs: int) -> dict[str, str]:
    """A distinct engine instance (and state dir, when the caller pinned one) for a gate that runs beside
    others, and its share of the worker's build/ctest jobs."""
    engine = str(workload.get("engine_name") or "engine")
    env = {
        "ACCEL_ENGINE_INSTANCE": f"{tag}-{engine}-{name}",
        "ACCEL_BUILD_JOBS": str(jobs),
        "ACCEL_CTEST_JOBS": str(jobs),
        "CMAKE_BUILD_PARALLEL_LEVEL": str(jobs),
    }
    if os.environ.get("ACCEL_ENGINE_STATE_DIR"):
        env["ACCEL_ENGINE_STATE_DIR"] = f"{os.environ['ACCEL_ENGINE_STATE_DIR']}-{name}"
    return env


def run_gates_parallel(
    pipeline_config: PipelineConfig,
    names: Sequence[str],
    query: int,
    workload: dict[str, Any],
    *,
    groups: Sequence[GateGroup] = DEFAULT_GROUPS,
    jobs: int | None = None,
    fail_fast: bool = True,
    reuse: bool = False,
    change_kind: str = "",
    iteration: int = 0,
) -> list[GateResult]:
    """Run `names` group by group (gates in no group first, serially, in the order given).

    jobs: concurrent gates in a "parallel" group (default quick_tier.gate_jobs, 0 = one per gate).
    fail_fast: after the first failing gate, running siblings are killed and the rest are not
      started (status "cancelled"); the failing result is the first non-pass in the returned list.
    reuse: a gate that already passed on the code as it is now (gates/stamps.py, background gates) is not
      run again; its result carries reused. Timing gates only count from this stage's acceptance tier.
    """
    grouped = {name for group in groups for name in group.names}
    wanted = list(dict.fromkeys(names))
    plan: list[tuple[str, list[str]]] = [("serial", [n for n in wanted if n not in grouped])]
    plan += [(group.mode, [n for n in group.names if n in wanted]) for group in groups]
    limit = jobs if jobs is not None else pipeline_config.quick_tier.gate_jobs
    cancel = threading.Event()
    failed: list[str] = []
    tag = instance_tag(pipeline_config.worker_id, pipeline_config.build_dir)
    parallel_width = [1]
    log_gate_batch_start(query, wanted, change_kind=change_kind, iteration=iteration)

    def run_one(name: str, *, isolated: bool) -> GateResult:
        if cancel.is_set():
            return _cancelled(name, failed[0])
        if reuse and (reused := reusable_gate_result(pipeline_config, workload, query, name, change_kind=change_kind)):
            print(f"[pipeline] gate {name}: reused (passed on identical code)", file=sys.stderr, flush=True)
            return reused
        result = run_gate(
            pipeline_config, name, query, workload,
            change_kind=change_kind, iteration=iteration, gate_index=wanted.index(name) + 1, gate_count=len(wanted),
            env=_instance_env(workload, tag, name, jobs_per_gate(parallel_width[0])) if isolated else None,
            cancel=cancel,
        )
        if fail_fast and not _ok(result) and result.status != "cancelled" and not cancel.is_set():
            failed.append(name)
            cancel.set()
        return result

    results: dict[str, GateResult] = {}
    for mode, batch in plan:
        if mode == "parallel" and len(batch) > 1:
            parallel_width[0] = min(limit or len(batch), len(batch), len(os.sched_getaffinity(0)))
            with ThreadPoolExecutor(max_workers=parallel_width[0]) as pool:
                futures = {name: pool.submit(run_one, name, isolated=True) for name in batch}
            results.update({name: future.result() for name, future in futures.items()})
        else:
            for name in batch:
                results[name] = run_one(name, isolated=False)
    return [results[name] for name in wanted]


BG_DIRNAME = "bg"
PACKAGE_ROOT = str(Path(__file__).resolve().parents[1])


def _bg_dir(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int) -> Path:
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    return gates_dir_for_query(art) / BG_DIRNAME


def _state_path(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int, gate: str) -> Path:
    return _bg_dir(pipeline_config, workload, query) / f"{gate}.json"


def _read_state(path: Path) -> dict[str, Any]:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def _alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    return True


def start_background_gate(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int, gate: str,
                          *, workload_name: str) -> dict[str, Any]:
    """Start `gate` in a detached process and return its state; a gate already running is left alone."""
    path = _state_path(pipeline_config, workload, query, gate)
    state = _read_state(path)
    if state.get("status") == "running" and _alive(int(state.get("pid", 0))):
        return state
    path.parent.mkdir(parents=True, exist_ok=True)
    state = {"gate": gate, "status": "running", "started_at": time.time(),
             "stamp_start": current_stamp(pipeline_config, workload, query),
             "check_version": check_version(pipeline_config.repo_root)}
    path.write_text(json.dumps(state), encoding="utf-8")
    log = (path.parent / f"{gate}.log").open("w")
    proc = subprocess.Popen(
        [sys.executable, "-m", "accel_pipeline.gates", "--repo-root", str(pipeline_config.repo_root),
         "--workload", workload_name, "--engine", str(workload.get("engine_name") or ""),
         "--query", str(query), "--gate", gate],
        cwd=pipeline_config.repo_root, stdout=log, stderr=subprocess.STDOUT, start_new_session=True,
        env={**os.environ, "PYTHONPATH": os.pathsep.join(filter(None, [PACKAGE_ROOT, os.environ.get("PYTHONPATH", "")]))},
    )
    state["pid"] = proc.pid
    path.write_text(json.dumps(state), encoding="utf-8")
    return state


def _tail(path: Path, size: int = 4096) -> str:
    with path.open("rb") as handle:
        handle.seek(0, os.SEEK_END)
        handle.seek(max(0, handle.tell() - size))
        return handle.read().decode("utf-8", "replace")


def _live(out_dir: Path, gate: str) -> dict[str, Any]:
    """What a running gate is doing: its current step, and the last line each of its runs has printed so far."""
    live: dict[str, Any] = {}
    with contextlib.suppress(OSError):
        live["phase"] = (out_dir / f"{gate}.phase").read_text(encoding="utf-8").strip()
    progress = {}
    for log in sorted(out_dir.glob("*.json.run.*.log")):
        with contextlib.suppress(OSError):
            lines = _tail(log).strip().splitlines()
            if lines:
                progress[log.name.split(".json.run.")[0]] = lines[-1][:160]
    return {**live, "progress": progress} if progress else live


def background_status(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int, gate: str) -> dict[str, Any]:
    """State of the background run of `gate`: running (with seconds elapsed, its current step and what its runs
    last printed), or its result and whether the code has changed since it ran ("stale": rerun it for acceptance)."""
    state = _read_state(_state_path(pipeline_config, workload, query, gate))
    if not state:
        return {"gate": gate, "status": "none", "reason": "no background run; request_gate(gate, background=True) starts one"}
    if state.get("status") == "running":
        if not _alive(int(state.get("pid", 0))):
            return {**state, "status": "died", "reason": "the background process ended without a result"}
        return {**state, "elapsed_sec": round(time.time() - float(state["started_at"]), 1),
                **_live(_state_path(pipeline_config, workload, query, gate).parent / gate, gate),
                "note": "still running: keep working, check again after your next edit or test"}
    state["stale"] = state.get("stamp_end") != current_stamp(pipeline_config, workload, query)
    if "finished_at" in state:
        state["seconds"] = round(state["finished_at"] - state["started_at"], 1)
    return state


def reusable_background_result(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int,
                               gate: str) -> GateResult | None:
    """The passing background result of `gate` if it ran on exactly the current code, else None
    (run the gate)."""
    state = _read_state(_state_path(pipeline_config, workload, query, gate))
    if state.get("status") != "pass" or not state.get("stamp_start"):
        return None
    if state.get("check_version") != check_version(pipeline_config.repo_root):
        return None
    if not (state["stamp_start"] == state.get("stamp_end") == current_stamp(pipeline_config, workload, query)):
        return None
    data = {**(state.get("data") or {}), "status": "pass", "reused": "background"}
    return GateResult(name=gate, status="pass", data=data, path=_state_path(pipeline_config, workload, query, gate))


def _run(args: argparse.Namespace) -> int:
    repo_root = Path(args.repo_root)
    workload = load_workload(args.workload, repo_root, engine=args.engine)
    pipeline_config = apply_workload_overrides(load_pipeline_config(repo_root=repo_root), workload)
    path = _state_path(pipeline_config, workload, args.query, args.gate)
    state = _read_state(path)
    out_dir = path.parent / args.gate
    out_dir.mkdir(parents=True, exist_ok=True)
    env = {"ACCEL_ENGINE_INSTANCE": f"{pipeline_config.worker_id or 'w0'}-{args.engine}-{args.gate}-bg"}
    if os.environ.get("ACCEL_ENGINE_STATE_DIR"):
        env["ACCEL_ENGINE_STATE_DIR"] = f"{os.environ['ACCEL_ENGINE_STATE_DIR']}-{args.gate}-bg"
    result = run_gate(pipeline_config, args.gate, args.query, workload, env=env, out_dir=out_dir)
    state.update(status=result.status, data=result.data, finished_at=time.time(),
                 stamp_end=current_stamp(pipeline_config, workload, args.query))
    path.write_text(json.dumps(state, indent=2), encoding="utf-8")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Run one gate in the background (started by request_gate)")
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--workload", required=True)
    parser.add_argument("--engine", default="")
    parser.add_argument("--query", type=int, required=True)
    parser.add_argument("--gate", required=True)
    return _run(parser.parse_args(argv))




STAMPS_FILE = "gate_stamps.json"
ITERATION_TIER = "optimization"  # the change_kind whose gate runs use the cheaper iteration bench settings
TIMING_GATES = frozenset({
    "aux_bench_sweep", "aux_cost_model_calibrate", "aux_speedup_verification", "baseline", "fairness", "profile", "sf10",
})
ACCEPTANCE_TIER_GATES = frozenset({"aux_exit"})
CHECK_FILES = ("pipeline/gates/*", "eval/harness/*", "backends/*/tests/harness/*",
               "pipeline/orchestrator/accel_pipeline/quick.py", "pipeline/orchestrator/accel_pipeline/write_cost.py")  # what defines the checks


def check_version(repo_root: Path) -> str:
    """Digest of the files that define the checks: a pass (or an admission) recorded under another version is void."""
    digest = hashlib.sha256()
    for path in sorted(p for pattern in CHECK_FILES for p in repo_root.glob(pattern) if p.is_file()):
        digest.update(f"\0{path.relative_to(repo_root)}\0".encode() + path.read_bytes())
    return digest.hexdigest()[:16]


def current_stamp(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int) -> str:
    """The code stamp of this query's tree now."""
    bench = str(workload.get("benchmark_name") or "")
    return code_stamp(pipeline_config.repo_root, query_dir=query_dir(bench, query) if bench else "")


def _stamps_path(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int) -> Path:
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    return gates_dir_for_query(art) / STAMPS_FILE


def _read(path: Path) -> dict[str, dict[str, str]]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    return {gate: entry for gate, entry in data.items() if isinstance(entry, dict)}


def record_pass(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int, gate: str, stamp: str,
                *, change_kind: str = "", result: Path | None = None) -> None:
    """`gate` passed on the code with `stamp`, in a run of `change_kind`, and wrote `result`."""
    path = _stamps_path(pipeline_config, workload, query)
    path.parent.mkdir(parents=True, exist_ok=True)
    entry = {"stamp": stamp, "check_version": check_version(pipeline_config.repo_root), "change_kind": change_kind,
             "result": str(result or "")}
    with path.with_suffix(".lock").open("a") as lock:  # parallel gates record concurrently
        fcntl.flock(lock, fcntl.LOCK_EX)
        path.write_text(json.dumps({**_read(path), gate: entry}, indent=2), encoding="utf-8")


def passed_at_current_stamp(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int, gate: str,
                            *, change_kind: str = "") -> bool:
    """True when `gate` last passed on exactly the code the tree has now (and, for a timing gate,
    in an acceptance-tier run of `change_kind`)."""
    entry = _read(_stamps_path(pipeline_config, workload, query)).get(gate) or {}
    stamp = entry.get("stamp")
    if not stamp or stamp != current_stamp(pipeline_config, workload, query):
        return False
    if entry.get("check_version") != check_version(pipeline_config.repo_root):
        return False
    if gate in ACCEPTANCE_TIER_GATES:
        return ITERATION_TIER not in (change_kind, entry.get("change_kind"))
    if gate in TIMING_GATES:
        return bool(change_kind) and change_kind != ITERATION_TIER and entry.get("change_kind") == change_kind
    return True


def reusable_gate_result(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int, gate: str,
                         *, change_kind: str = "") -> GateResult | None:
    """The passing result of `gate` on exactly the current code (a foreground run, else a background
    run), or None: run the gate."""

    if passed_at_current_stamp(pipeline_config, workload, query, gate, change_kind=change_kind):
        entry = _read(_stamps_path(pipeline_config, workload, query))[gate]
        path = Path(entry["result"]) if entry.get("result") else None
        data = read_json(path) if path else {}
        if data.get("status") == "pass":
            return GateResult(name=gate, status="pass", data={**data, "reused": True}, path=path)
    if gate in TIMING_GATES:
        return None
    return reusable_background_result(pipeline_config, workload, query, gate)


@contextmanager
def build_lock(build_dir: Path) -> Iterator[None]:
    """Exclusive lock on a build directory: concurrent gates and tools serialize their builds on it.
    The lock file is the one the gate scripts' flock uses (aux_gate_common.sh engine_steps)."""
    lock_path = Path(f"{build_dir}.build.lock")
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    with lock_path.open("a") as handle:
        fcntl.flock(handle, fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(handle, fcntl.LOCK_UN)


QUICK_GATE = "quick_check"
# What kills one differential run inside a gate (aux_gate_common.sh engine_run): the budget the projection must fit.
GATE_RUN_TIMEOUT_S = float(os.environ.get("ACCEL_GATE_RUN_TIMEOUT", 900))
ADMISSION_FILE = f"{QUICK_GATE}.admitted"
PROJECTION_LINES = 5  # the costliest write kinds a passing check reports
INSTANCE_KEYS = ("engine_instance", "engine_state_dir", "dataset")  # what the gate result names of the instance it ran in
REPORT_FILE = f"{QUICK_GATE}.report.json"  # what plain passes left: their reports and the engine instance


@dataclass
class QuickResult:
    status: str  # "pass" | "fail" | "skip"
    phase_timings: dict[str, float] = field(default_factory=dict)
    failure: Failure | None = None
    summary: str = ""
    report: dict[str, Any] = field(default_factory=dict)  # build / read / writes / counters, see check_report.py


def _result_stamp(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int,
                  targets: list[str], differential: bool, sanitizer: str = "", threads: int = 0) -> str:
    bench = str(workload.get("benchmark_name") or "")
    return code_stamp(
        pipeline_config.repo_root,
        query_dir=query_dir(bench, query) if bench else "",
        extra=[QUICK_GATE, *targets, f"differential={differential}", f"sanitizer={sanitizer}:{threads}",
               str(pipeline_config.build_dir), check_version(pipeline_config.repo_root)],
    )


def _passed_stamp_path(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int) -> Path:
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    return gates_dir_for_query(art) / f"{QUICK_GATE}.stamp"


def _admission_key(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int) -> str:
    """The code and the checks an admission holds for ("" when there is no code stamp)."""
    stamp = current_stamp(pipeline_config, workload, query)
    return f"{stamp}:{check_version(pipeline_config.repo_root)}" if stamp else ""


def record_admission(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int) -> None:
    """quick_check passed on the code as it is now: the heavy gates may run."""
    if stamp := _admission_key(pipeline_config, workload, query):
        _passed_stamp_path(pipeline_config, workload, query).with_name(ADMISSION_FILE).write_text(stamp, encoding="utf-8")


def admission_refusal(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int, gate: str) -> str:
    """Why `gate` may not run yet ("" when it may): a heavy aux gate needs a passing quick_check on the current code."""
    quick = pipeline_config.quick_tier
    if not quick.enabled or gate not in quick.admission_gates or workload.get("workload_kind") != "aux_state":
        return ""
    path = _passed_stamp_path(pipeline_config, workload, query).with_name(ADMISSION_FILE)
    key = _admission_key(pipeline_config, workload, query)
    if key and path.is_file() and path.read_text(encoding="utf-8") == key:
        return ""
    return f"{gate} refused: run quick_check first and iterate until it passes on the current code (it takes seconds), then request {gate} once."


def _project_write_cost(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int, cuts: CutDatasets) -> Projection | None:
    """The full gate's write time, projected from the timings of the quick differential (None: no timings)."""
    bench = str(workload.get("benchmark_name") or "")
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    reports = load_reports(gates_dir_for_query(art))
    suite = suite_path(pipeline_config.repo_root, bench, query)
    if not reports or not (suite / MANIFEST).is_file():
        return None
    probes = int(((read_yaml(suite / MANIFEST).get("point_writes") or {}).get("max_probes")) or 5)
    return project(
        reports, profiles=txn_profiles(suite), dependent=dependent_tables(art),
        scale=cuts.scale, probes_full=probes,
        budget_s=GATE_RUN_TIMEOUT_S * pipeline_config.quick_tier.write_cost_budget_share)


def _unfaithful_rebuilds(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int) -> list[str]:
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    analysis = art / "analysis.json"
    reports = load_reports(gates_dir_for_query(art))
    if not reports or not analysis.is_file():
        return []
    coverage = (json.loads(analysis.read_text(encoding="utf-8")).get("correctness_facts") or {}).get("mutation_coverage") or []
    suite = suite_path(pipeline_config.repo_root, str(workload.get("benchmark_name", "")), query)
    return unfaithful_rebuilds(reports, txn_files(suite), coverage, statements)


def _last_sweep_scaling(art: Path) -> dict[str, Any]:
    """The thread_scaling of the last passing bench sweep (the exit run's), {} when there is none."""
    try:
        sweep = json.loads((gates_dir_for_query(art) / "aux_bench_sweep.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    return sweep.get("thread_scaling") or {} if sweep.get("status") == "pass" else {}


def _plain_report(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int, cuts: CutDatasets,
                  instance: dict[str, str]) -> tuple[dict[str, Any], list[str]]:
    """(the compact report of a passing plain check, the defects it shows): rebuild-on-write, writes and a build that
    would not fit the gates, a build that does not scale with threads; then the reads and perf counters."""
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    defects = _unfaithful_rebuilds(pipeline_config, workload, query)
    defects = [f"{len(defects)} write transaction(s) rebuild the state where the analysis maintains them incrementally "
               f"(at full scale each is a full scan). First: {defects[0]}"] if defects else []
    report: dict[str, Any] = {}
    reports = load_reports(gates_dir_for_query(art))
    dependent = dependent_tables(art)
    line, defect = build_report(
        reports, scale=cuts.scale(dependent), threads=gate_threads(),
        budget_s=pipeline_config.quick_tier.build_budget_sec,
        expected_scaling=expects_scaling(load_query_analysis(pipeline_config.repo_root, query, workload=workload)[1]),
        sweep_scaling=_last_sweep_scaling(art))
    report.update({"build": line} if line else {})
    defects += [defect] if defect else []
    projection = _project_write_cost(pipeline_config, workload, query, cuts)
    if projection:
        report["writes"] = projection.lines[:PROJECTION_LINES]
        report["writes_total"] = projection.summary()
        if projection.over:
            defects.append(f"{projection.summary()}: the full gate would time out. Costliest: {projection.worst}")
    report.update(_measure(pipeline_config, workload, query, instance))
    return report, defects


def _measure(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int, instance: dict[str, str]) -> dict[str, str]:
    """Read latency against the engine's SQL, and perf counters, from the aux bench on the subset."""
    if not instance:
        return {}
    bench = Bench(pipeline_config, workload, query, instance)
    try:
        row = bench.row()
    except (RuntimeError, OSError, subprocess.SubprocessError) as exc:
        return {"read": f"unavailable ({str(exc)[-300:]})"}
    missing = [c for c in READ_COLUMNS if c not in row]
    if missing:  # the bench harness breaks its contract: host_bootstrap's gate refuses it
        return {"read": f"unavailable (the bench printed no {', '.join(missing)}: bench_sweep_contract.md)"}
    return {"read": read_line(row), "counters": counters_line(bench.counters(row))}


def _sanitizer_evidence(data: dict[str, Any]) -> str:
    found = data.get("sanitizer_findings") or {}
    lines = []
    for item in found.get("yours") or []:
        lines += [item["summary"], *[f"    {f}" for f in item["frames"]]]
    infra = found.get("infra") or {}
    if infra.get("count"):
        lines.append(f"infrastructure reports (not failures): {infra['count']}, e.g. " + "; ".join(infra.get("summaries") or []))
    return "\n".join(lines)


def _failure(data: dict[str, Any], timings: dict[str, float]) -> Failure:
    reason = str(data.get("reason") or "")
    step = str(data.get("step") or "")
    return Failure(
        kind="timeout" if "did not finish within" in reason else "code",
        stage=QUICK_GATE,
        source=QUICK_GATE,
        summary=f"quick_check: {reason or data.get('status')}" + (f" (step {step})" if step else ""),
        evidence=_sanitizer_evidence(data) or str(data.get("log_tail") or ""),
        reproduce=str(data.get("reproduce") or ""),
        diagnosis=[f"{name}: {seconds:.1f}s" for name, seconds in timings.items()],
        owner=str(data.get("owner") or ""),
    )


SANITIZERS = ("thread", "address")
PROFILE_SECONDS = 8.0  # of each sampled loop in run_profile (read, write)
SANITIZER_TIMEOUT_SEC = 3600  # the first instrumented build of a worker; later checks are incremental


def prepare_inner_loop(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int) -> threading.Thread | None:
    """Once per run, before analysis: make the test databases, and start building the sanitizer variant of the
    substrate and harness in the background, so the agent's first sanitizer check compiles only the query's files."""
    if not pipeline_config.quick_tier.enabled or workload.get("workload_kind") != "aux_state":
        return None
    try:
        cuts = ensure_cut_datasets(pipeline_config, workload, query)
    except CutError as exc:  # every gate on the cuts reports it as the data's; there is nothing to prebuild against
        print(f"[pipeline] {exc}", file=sys.stderr, flush=True)
        return None
    gates_dir = gates_dir_for_query(query_artifacts_dir(pipeline_config.repo_root, query, workload=workload))

    def build() -> None:
        run_gate(pipeline_config, QUICK_GATE, query, workload, out_dir=gates_dir / "prebuild", timeout_s=SANITIZER_TIMEOUT_SEC,
                 env={"ACCEL_QUICK_SANITIZER": "thread", "ACCEL_QUICK_PREBUILD": "1",
                      "ACCEL_SUBSET_DATASET_DIR": str(cuts.tiny)})
    thread = threading.Thread(target=build, name="sanitizer-prebuild", daemon=True)
    thread.start()
    return thread


def run_quick_check(pipeline_config: PipelineConfig, workload: dict, query: int, *, targets: list[str] | None = None,
                    differential: bool = True, sanitizer: str = "", threads: int = 4) -> QuickResult:
    """Build the query's targets (skipped when unchanged), run its tests, and, when `differential`,
    the SQL-vs-accelerator differential on the subset dataset. `targets` are extra build targets
    (the agent's own test binaries) that are built and run. A tree unchanged since the last passing
    check passes without running anything.

    `sanitizer` ("thread" or "address") builds in an instrumented build dir of the worker's own (incremental),
    runs the same steps with every phase of the thread budget at `threads`, and reports the sanitizer findings:
    only reports with a frame in the query's own directory fail the check, the rest is counted as
    infrastructure. A host that cannot build the variant reports "not applicable"."""
    if sanitizer and sanitizer not in SANITIZERS:
        return QuickResult(status="fail", summary=f"sanitizer must be one of {SANITIZERS}, not {sanitizer!r}")
    quick = pipeline_config.quick_tier
    if not quick.enabled:
        return QuickResult(status="skip", summary="quick tier disabled (quick_tier.enabled)")
    if workload.get("workload_kind") != "aux_state":
        return QuickResult(status="skip", summary="quick_check covers aux-state workloads")
    extra_targets = list(targets or [])
    stamp = _result_stamp(pipeline_config, workload, query, extra_targets, differential, sanitizer, threads)
    passed_path = _passed_stamp_path(pipeline_config, workload, query)
    admits = differential and not sanitizer  # only the plain run with the differential opens the heavy gates
    if stamp and passed_path.is_file() and passed_path.read_text(encoding="utf-8") == stamp:
        if admits:
            record_admission(pipeline_config, workload, query)
        return QuickResult(status="pass", summary="unchanged since the last passing quick_check",
                           report=saved_report(pipeline_config, workload, query).get("reports", {}).get(stamp, {}))

    started = time.monotonic()
    try:
        cuts = ensure_cut_datasets(pipeline_config, workload, query)
    except CutError as exc:
        failure = Failure(kind="infra", stage=QUICK_GATE, source=QUICK_GATE, summary=str(exc), owner=DATA, infra_reason="data_defect")
        return QuickResult(status="fail", failure=failure, summary=failure.summary)
    gates_dir = gates_dir_for_query(query_artifacts_dir(pipeline_config.repo_root, query, workload=workload))
    for stale in report_paths(gates_dir):
        stale.unlink(missing_ok=True)
    timings = {"test_databases": round(time.monotonic() - started, 3)}
    result = run_gate(
        pipeline_config, QUICK_GATE, query, workload,
        env={
            "ACCEL_QUICK_TARGETS": " ".join(extra_targets),
            "ACCEL_QUICK_DIFFERENTIAL": "1" if differential else "0",
            "ACCEL_QUICK_PROBES": str(quick.differential_probes),
            "ACCEL_BUILD_STAMP_CACHE": "1" if quick.build_stamp_cache else "0",
            "ACCEL_SUBSET_DATASET_DIR": str(cuts.tiny if sanitizer else cuts.subset),
            **({"ACCEL_QUICK_SANITIZER": sanitizer, "ACCEL_QUICK_THREADS": str(threads)} if sanitizer else {}),
        },
        timeout_s=max(quick.timeout_sec, SANITIZER_TIMEOUT_SEC) if sanitizer else quick.timeout_sec,
    )
    timings.update(result.data.get("phase_timings") or {})
    if result.status == "skip":
        return QuickResult(status="skip", phase_timings=timings, summary=str(result.data.get("reason") or "skipped"))
    if result.status != "pass":
        passed_path.unlink(missing_ok=True)
        failure = _failure(result.data, timings)
        return QuickResult(status="fail", phase_timings=timings, failure=failure, summary=failure.summary)
    instance = {} if sanitizer else {k: str(result.data[k]) for k in INSTANCE_KEYS if result.data.get(k)}
    report, defects = _plain_report(pipeline_config, workload, query, cuts, instance) if admits else ({}, [])
    if defects:
        passed_path.unlink(missing_ok=True)
        failure = Failure(kind="code", stage=QUICK_GATE, source=QUICK_GATE, summary=f"quick_check: {defects[0]}",
                          diagnosis=defects[1:])
        return QuickResult(status="fail", phase_timings=timings, failure=failure, summary=failure.summary, report=report)
    if stamp:
        passed_path.parent.mkdir(parents=True, exist_ok=True)
        passed_path.write_text(stamp, encoding="utf-8")
        if not sanitizer:
            _save_report(pipeline_config, workload, query, stamp, report, instance)
    if admits:
        record_admission(pipeline_config, workload, query)
    return QuickResult(status="pass", phase_timings=timings, summary="quick_check passed", report=report)


def profile_hot_spots(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int, scale: str = "subset") -> str:
    """run_profile: samples of the read, write and build paths as JSON, on the subset in the quick_check's instance or
    (scale "full") on the full gate dataset in an instance of its own. A tree that does not pass the plain check, or a
    profiler that cannot run, is a failure to fix or report; a server engine is not applicable."""
    checked = run_quick_check(pipeline_config, workload, query, differential=False)
    instance = saved_report(pipeline_config, workload, query).get("instance")
    if checked.status != "pass" or not instance:
        return render_quick_result(checked) if checked.status != "pass" else json.dumps(
            {"status": "fail", "summary": "no quick_check instance to profile in: run quick_check first"})
    if scale == "full":
        instance = _full_instance(workload, instance)
        if problem := ensure_dataset_loaded(workload, pipeline_config.repo_root, pipeline_config.build_dir,
                                            instance_env={"ACCEL_ENGINE_INSTANCE": instance["engine_instance"],
                                                          "ACCEL_ENGINE_STATE_DIR": instance["engine_state_dir"]}):
            return json.dumps({"status": "fail", "summary": f"the full dataset could not be loaded for profiling: {problem}"})
    profiles = Bench(pipeline_config, workload, query, instance).profile(PROFILE_SECONDS)
    broken = {path: p["reason"] for path, p in profiles.items() if p["status"] == "error"}
    if broken:
        failure = Failure(kind="infra", stage="run_profile", source="perf", summary="run_profile could not sample: " + "; ".join(
            f"{path}: {reason}" for path, reason in broken.items()),
            diagnosis=["profiling failed because of the host or the harness, not because there is nothing to profile: fix what "
                       "the message names, or report it (report_blocker) if it is outside the query's files"])
        return json.dumps({"status": "fail", "failure": render_failure(failure)}, indent=2)
    from accel_pipeline.optimization import bench as perf_bench

    return json.dumps({"status": "pass", "scale": scale,
                       **{f"{path}_path": perf_bench.render_for_agent(p).splitlines() for path, p in profiles.items()}}, indent=2)


def _full_instance(workload: dict[str, Any], quick_instance: dict[str, str]) -> dict[str, str]:
    """An instance of its own on the full gate dataset, as the stage exit gives its bench phase (the profile's
    write mix must not disturb the quick instance)."""
    return {"engine_instance": quick_instance["engine_instance"] + "-full",
            "engine_state_dir": quick_instance["engine_state_dir"] + "-full", "dataset": workload_dataset_dir(workload)}


def _report_path(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int) -> Path:
    return _passed_stamp_path(pipeline_config, workload, query).with_name(REPORT_FILE)


def saved_report(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int) -> dict[str, Any]:
    """What plain passes left: {"instance": the engine instance and dataset the quick gate loaded (the profile runs
    in it), "reports": {result stamp: report}}; {} before the first."""
    try:
        return json.loads(_report_path(pipeline_config, workload, query).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def _save_report(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int, stamp: str,
                 report: dict[str, Any], instance: dict[str, str]) -> None:
    saved = saved_report(pipeline_config, workload, query)
    saved = {"instance": instance, "reports": {**saved.get("reports", {}), stamp: report}}
    _report_path(pipeline_config, workload, query).write_text(json.dumps(saved), encoding="utf-8")


def render_quick_result(result: QuickResult) -> str:
    """Compact JSON for the agent: status, the report, seconds per phase, and on failure the rendered failure."""
    payload: dict[str, Any] = {"status": result.status}
    if not result.failure:  # the failure text carries the summary
        payload["summary"] = result.summary
    payload.update(result.report)
    if result.phase_timings:
        payload["phase_seconds"] = result.phase_timings
    if result.failure:
        payload["failure"] = render_failure(result.failure)
    return json.dumps(payload, indent=2)


BENCH_SECONDS = 120.0
READ_QUERIES = 20  # paired measured queries for the latency medians
COUNTER_LOOP_SECONDS = 0.5  # the read loop the counters cover
WRITE_MIX_PCT = 50  # share of operations that are writes in the write-path profile


RUN_OUTCOME = Path(__file__).resolve().parents[2] / "gates" / "run_outcome.py"  # this tree's: caps a run's memory


@dataclass(frozen=True)
class HostRun:
    """The one way to run anything on the engine: `host.sh run` in an engine instance (host_contract.md), after `up`,
    `load` and `build` through the same host. `instance`: engine_instance, engine_state_dir and the dataset loaded."""
    cfg: PipelineConfig
    workload: dict[str, Any]
    instance: dict[str, str]

    @classmethod
    def worker(cls, cfg: PipelineConfig, workload: dict[str, Any]) -> HostRun:
        """The instance this worker's own benchmarks and smoke run in, loaded with the workload's dataset: apart from
        the gates' instances (aux_exit's bench phase writes `<instance>-bench`), which may run at the same time."""
        own = instance_env(cfg.repo_root, str(workload.get("engine_name") or ""), cfg.build_dir)
        return cls(cfg, workload, {"engine_instance": own["ACCEL_ENGINE_INSTANCE"] + "-optimization",
                                   "engine_state_dir": own["ACCEL_ENGINE_STATE_DIR"] + "-optimization",
                                   "dataset": workload_dataset_dir(workload)})

    @property
    def engine(self) -> str:
        return str(self.workload.get("engine_name") or "")

    def env(self, extra: Mapping[str, str] = {}) -> dict[str, str]:
        """This process's environment, the instance's variables of the contract, then `extra`: the run's scratch on
        disk with its instance, and its memory budget (the run's cap, half of it its engines'), as the gates give them."""
        scratch = Path(self.instance["engine_state_dir"]) / "tmp"
        run_mb = int(os.environ.get("ACCEL_RUN_MEMORY_MB") or run_memory_budget())
        return {**os.environ, **instance_env(self.cfg.repo_root, self.engine, self.cfg.build_dir),
                "ACCEL_ENGINE_INSTANCE": self.instance["engine_instance"],
                "ACCEL_ENGINE_STATE_DIR": self.instance["engine_state_dir"],
                "ACCEL_DATASET_DIR": self.instance["dataset"], "TMPDIR": str(scratch),
                "ACCEL_RUN_MEMORY_MB": str(run_mb), "ACCEL_ENGINE_MEMORY_MB": str(run_mb // 2), **extra}

    def build(self, *targets: str) -> str | None:
        """`up`, then `build` of `targets` (one build of a build dir at a time); None, or why not."""
        env = self.env()
        with build_lock(self.cfg.build_dir):
            for verb, args in (("up", ()), ("build", targets)):
                done = run_host_verb(self.cfg.repo_root, self.engine, verb, *args, build_dir=self.cfg.build_dir, env=env)
                if done.returncode not in (0, 77):
                    return f"host.sh {verb} failed (exit {done.returncode})\n{((done.stdout or '') + (done.stderr or ''))[-2000:]}"
        return None

    def load(self) -> str | None:
        """The instance up with the dataset loaded; None, or why not."""
        return ensure_dataset_loaded(self.workload, self.cfg.repo_root, self.cfg.build_dir, instance_env={
            "ACCEL_ENGINE_INSTANCE": self.instance["engine_instance"],
            "ACCEL_ENGINE_STATE_DIR": self.instance["engine_state_dir"],
            "ACCEL_DATASET_DIR": self.instance["dataset"]})

    def ready(self, *targets: str) -> str | None:
        """The instance up with the dataset loaded and `targets` built; None, or why not."""
        return self.load() or self.build(*targets)

    def run(self, command: list[str], *, env: Mapping[str, str] | None = None, timeout: float | None = None,
            measured: str = "") -> subprocess.CompletedProcess[str]:
        """`command` through `host.sh run`, killed with its children at `timeout`. A non-empty `measured` (the reason)
        queues it for the measurement lease and pins it to the lease's CPUs; the queueing is outside `timeout` and
        the stderr ends with the line that reports it (pack.waited_seconds)."""
        from accel_pipeline.pack import Window, holder_name, measurement_window

        env = {**(env or self.env())}
        if Path(env["TMPDIR"]).parent.is_dir():  # the instance is up: its scratch lives in its state dir
            Path(env["TMPDIR"]).mkdir(exist_ok=True)
        with (measurement_window(holder_name(), measured) if measured else contextlib.nullcontext(Window())) as window:
            argv = window.argv([sys.executable, str(RUN_OUTCOME), "cap",
                                "--", str(host_script(self.cfg.repo_root, self.engine)), "run", *command])  # its memory cap
            with subprocess.Popen(argv, cwd=self.cfg.repo_root, env={**env, **window.env}, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True, start_new_session=True) as proc:
                try:
                    out, err = proc.communicate(timeout=timeout)
                except subprocess.TimeoutExpired:
                    os.killpg(proc.pid, signal.SIGKILL)
                    out, err = proc.communicate()
                    raise subprocess.TimeoutExpired(argv, timeout, out, err) from None
        return subprocess.CompletedProcess(argv, proc.returncode, out, err + (f"\n{window.line()}\n" if measured else ""))

    def perf_runner(self):
        """subprocess.run that sends the outermost `perf` command (the bench inside it) through host.sh."""
        def run(command, *, env, **kwargs):
            if Path(command[0]).name == "perf" and command[1] in ("stat", "record"):
                command = [str(host_script(self.cfg.repo_root, self.engine)), "run", *command]
            return subprocess.run(command, env=env, **kwargs)
        return run


@dataclass(frozen=True)
class Bench:
    """The aux bench binary on the quick instance (`instance`: the gate result's engine_instance, engine_state_dir, dataset)."""
    cfg: PipelineConfig
    workload: dict[str, Any]
    query: int
    instance: dict[str, str]

    @property
    def host(self) -> HostRun:
        return HostRun(self.cfg, self.workload, self.instance)

    @property
    def target(self) -> Path:
        return self.cfg.build_dir / resolve_workload_paths(self.cfg, self.workload, self.query)["aux_bench_target"]

    def env(self, work_dir: str, queries: int, pct: int = 0) -> dict[str, str]:
        """The bench's environment: the instance's, the write frequency and the repetitions (bench_sweep_contract.md)."""
        paths = resolve_workload_paths(self.cfg, self.workload, self.query)
        return self.host.env({
            f"ACCEL_Q{self.query}_BENCH_SUITE": paths["aux_bench_suites"],
            "ACCEL_AUX_BENCHMARK": str(self.workload.get("benchmark_name") or ""),
            "ACCEL_AUX_BENCH_WORK_DIR": work_dir,
            "ACCEL_AUX_WRITE_FREQUENCIES": str(pct),
            "ACCEL_BENCH_WARMUP": "3",
            "ACCEL_BENCH_QUERIES": str(queries),
            "ACCEL_BENCH_REF_SAMPLES": "2",
            "ACCEL_BENCH_BASE_WRITE_SAMPLES": "1",
            "ACCEL_BENCH_SKIP_PARTITION_REBUILD_CALIB": "1",
        })

    def command(self, pct: int = 0) -> list[str]:
        return [str(self.target), "--query", str(self.query), str(pct)]

    def run(self, command: list[str], env: dict[str, str]) -> subprocess.CompletedProcess[str]:
        return self.host.run(command, env=env, timeout=BENCH_SECONDS)

    def row(self, pct: int = 0) -> dict[str, Any]:
        """One short bench run at write frequency `pct`: its result row (medians of the engine's SQL and of the
        accelerator over the same bindings, hit rate, state build, visited-state counters)."""
        with tempfile.TemporaryDirectory(prefix="accel-bench-") as work:
            done = self.run(self.command(pct), self.env(work, READ_QUERIES, pct))
        rows = parse_aux_benchmark_output(done.stdout or "")
        if done.returncode != 0 or not rows:
            raise RuntimeError(f"the bench exited {done.returncode}: {((done.stderr or '') + (done.stdout or ''))[-600:]}")
        return rows[0]

    def counters(self, row: dict[str, Any]) -> dict[str, Any]:
        """perf stat counters: {"read": per query of the read loop, "build": the whole run of a one-query bench},
        or {"unavailable": why}. `row`: this bench's read-only row."""
        from accel_pipeline.optimization import bench as perf_bench

        if gap := self.profile_gap():
            return {"unavailable": gap}
        with tempfile.TemporaryDirectory(prefix="accel-bench-") as work:
            read = perf_bench.profile_hit_path(self.command(), self.env(work, 1), cwd=self.cfg.repo_root,
                                               runner=self.host.perf_runner(), hit_us=row["accel_hit_median_us"],
                                               loop_seconds=COUNTER_LOOP_SECONDS, record=False)
            build = self.run(["perf", "stat", "-x,", "-e", "instructions,cycles,cache-misses", "-o", f"{work}/build.stat",
                              "--", *self.command()], self.env(work, 1))
            stat = Path(work) / "build.stat"
            built = perf_bench.parse_stat(stat.read_text(encoding="utf-8")) if stat.is_file() else {}
        if read["status"] != "ok" or build.returncode != 0:
            return {"unavailable": read.get("reason") or f"perf stat of the build exited {build.returncode}"}
        return {"read": read["counters_per_query"], "build": built}

    def profile(self, seconds: float) -> dict[str, dict[str, Any]]:
        """Samples of the read path (read-only point), the write path (a WRITE_MIX_PCT% write mix) and the build (a
        one-query run), the read and write loops about `seconds` long: {path: perf profile result}."""
        from accel_pipeline.optimization import bench as perf_bench

        if gap := self.profile_gap():
            return {path: perf_bench.not_applicable(gap) for path in ("read", "write", "build")}
        out = {}
        for path, pct in (("read", 0), ("write", WRITE_MIX_PCT)):
            row = self.row(pct)
            op_us = (float(row["accel_hit_median_us"]) * (100 - pct) + float(row.get("accel_write_median_us") or 0) * pct) / 100.0
            with tempfile.TemporaryDirectory(prefix="accel-bench-") as work:
                out[path] = perf_bench.profile_hit_path(
                    self.command(pct), self.env(work, 1, pct), cwd=self.cfg.repo_root, runner=self.host.perf_runner(), hit_us=op_us,
                    loop_seconds=seconds, diagnostics=row if pct == 0 else None)
        with tempfile.TemporaryDirectory(prefix="accel-bench-") as work:
            out["build"] = perf_bench.profile_build(self.command(), self.env(work, 1), cwd=self.cfg.repo_root,
                                                    runner=self.host.perf_runner())
        return out

    def profile_gap(self) -> str:
        """Why the bench cannot be profiled with perf on this engine ("" when it can)."""
        from accel_pipeline.optimization import bench as perf_bench

        return perf_bench.engine_profile_gap(host_info(self.cfg.repo_root, self.host.engine, self.cfg.build_dir))


MIN_TIMED_BUILD_S = 1.0  # a subset build shorter than this is mostly fixed cost (per-partition scans, thread start):
                         # no verdict on its scaling or its full-scale projection
MIN_THREADS = 4
SCALING_FLOOR = 1.5  # the N-thread build must be at least this much faster than the one-thread build
_SERIAL_PLAN = re.compile(r"^\s*(n/a|none|serial|single)\b", re.IGNORECASE)


def gate_threads() -> int:
    """The thread cap of the gate's N-thread run: ACCEL_AUX_THREADS, else the CPUs of this process (aux_gate_common.sh)."""
    return int(os.environ.get("ACCEL_AUX_THREADS") or len(os.sched_getaffinity(0)))


def expects_scaling(analysis: dict[str, Any]) -> bool:
    """The analysis says the build splits over the thread cap (scale_plan.build_parallelism)."""
    plan = str(((analysis.get("correctness_facts") or {}).get("scale_plan") or {}).get("build_parallelism") or "")
    return bool(plan.strip()) and not _SERIAL_PLAN.match(plan)


def _not_scaling(at_n: float, at_1: float, threads: int, where: str) -> str:
    return (
        f"the state build does not scale with threads: {at_n:.2f} s at {threads} threads vs {at_1:.2f} s at 1 "
        f"({at_1 / at_n:.1f}x) {where}, but analysis.json's scale_plan.build_parallelism says it splits over the thread cap. "
        "Something in it is serial, usually one thread inserting rows one at a time into a shared structure. "
        "Build per-partition partials in core::PartitionedBuild (or the scan role's partitioned scan) and merge "
        "them; sort or bulk-load instead of inserting per row.")


def full_scale_defect(sweep_scaling: dict[str, Any]) -> str:
    """The thread_scaling of the last bench sweep (builds at N and at 1 thread on the iteration database) as a defect, for a subset build too
    short to judge."""
    at_n, at_1 = float(sweep_scaling.get("build_ms_at_N") or 0.0) / 1e3, float(sweep_scaling.get("build_ms_at_1") or 0.0) / 1e3
    threads = int(sweep_scaling.get("threads") or 0)
    if threads < MIN_THREADS or not at_n or at_1 / at_n >= SCALING_FLOOR:
        return ""
    return _not_scaling(at_n, at_1, threads, "in the last bench sweep on the full iteration database")


def build_report(reports: list[dict], *, scale: float, threads: int, budget_s: float, expected_scaling: bool,
                 sweep_scaling: dict[str, Any] | None = None) -> tuple[str, str]:
    """(the build line, the defect or ""): the build's seconds at N and at 1 thread on the subset, its speedup, and the
    N-thread build projected to the full dataset (rows scale). `reports`: the suite reports of the N-thread and the
    one-thread run, the second missing when the cap is one thread. A subset build too short to time is judged on
    `sweep_scaling`, the last bench sweep's builds at full scale, and not projected."""
    if not reports or not float(reports[0].get("build_seconds") or 0.0):
        return "", ""
    at_n = float(reports[0]["build_seconds"])
    at_1 = float(reports[1]["build_seconds"]) if len(reports) > 1 else 0.0
    speedup = at_1 / at_n if at_1 else 0.0
    full_s = at_n * scale
    line = f"{at_n:.2f} s at {threads} threads" + (f", {at_1:.2f} s at 1 thread ({speedup:.1f}x)" if at_1 else "")
    if max(at_n, at_1) < MIN_TIMED_BUILD_S:
        line += " on the subset, too short to time (the exit gate times the full build)"
        return line, full_scale_defect(sweep_scaling or {}) if expected_scaling else ""
    line += f" on the subset; projected {full_s:.0f} s on the full dataset (budget {budget_s:.0f} s)"
    if expected_scaling and threads >= MIN_THREADS and at_1 >= MIN_TIMED_BUILD_S and speedup < SCALING_FLOOR:
        return line, _not_scaling(at_n, at_1, threads, "on the subset")
    if full_s > budget_s:
        return line, (
            f"the state build is too slow for the full dataset: {at_n:.2f} s on the subset at {threads} threads, "
            f"projected ~{full_s:.0f} s at full scale; the gates build it several times (budget {budget_s:.0f} s). "
            "Cut the work per row (bulk-load a sorted run instead of one insert per row, size containers up front, "
            "avoid per-row allocation) and split it over threads; `run_profile` shows the build's hot spots.")
    return line, ""


READ_COLUMNS = ("engine_ref_query_median_us", "accel_hit_median_us", "accel_hit_rate")


def read_line(row: dict[str, Any]) -> str:
    """The engine's SQL against the accelerator for the same bindings, medians on the subset."""
    ref, accel = float(row["engine_ref_query_median_us"]), float(row["accel_hit_median_us"])
    hits = float(row["accel_hit_rate"])
    line = f"engine SQL {ref:.0f} us, accelerator {accel:.0f} us ({ref / accel:.1f}x faster)" if accel else "no accelerated reads"
    return line + ("" if hits >= 0.999 else f"; only {hits:.0%} of the reads were answered by the accelerator, the rest fell back to the SQL")


def _si(value: float | None) -> str:
    if value is None:
        return "?"
    for unit, size in (("G", 1e9), ("M", 1e6), ("k", 1e3)):
        if value >= size:
            return f"{value / size:.1f}{unit}"
    return f"{value:.0f}"


def counters_line(counters: dict[str, Any]) -> str:
    """perf stat counters: per read, and for the whole one-query run that builds the state."""
    if "unavailable" in counters:
        return f"unavailable ({counters['unavailable']})"

    def fmt(c: dict[str, float | None]) -> str:
        return ", ".join(f"{_si(c.get(k))} {name}" for k, name in (("instructions", "instructions"), ("cycles", "cycles"),
                                                                     ("cache-misses", "cache misses")))
    return f"per read {fmt(counters['read'])}; build run {fmt(counters['build'])}"


REBUILD_SHARE = 0.5
MIN_REBUILD_MS = 1.0
SUITE_REPORT_STEM = "suite_quick_report"


def report_paths(gates_dir: Path) -> list[Path]:
    return [gates_dir / f"{SUITE_REPORT_STEM}.json", gates_dir / f"{SUITE_REPORT_STEM}_threads1.json"]


@dataclass
class Projection:
    total_s: float = 0.0
    budget_s: float = 0.0
    lines: list[str] = field(default_factory=list)  # one per transaction kind, the costliest first
    worst: str = ""

    @property
    def over(self) -> bool:
        return self.total_s > self.budget_s

    def summary(self) -> str:
        return f"projected full-gate write time ~{self.total_s:.0f} s (budget {self.budget_s:.0f} s)"


def is_rebuild(cost_s: float, build_s: float, action: str = "") -> bool:
    """A write the accelerator maintains by rebuilding: it says so (the report's `action`, when it has one), or it
    costs about as much as building the state from scratch."""
    return "rebuild" in action.lower() or (cost_s * 1000 >= MIN_REBUILD_MS and cost_s >= REBUILD_SHARE * build_s)


def unfaithful_rebuilds(reports: list[dict], txn_sql: dict[str, str], coverage: list[dict],
                        statements: Callable[[str], list[tuple[str, str]]]) -> list[str]:
    """Suite transactions that rebuild the state although analysis.json's mutation_coverage maintains every
    (table, mutation) they write as supported_delta: one line each, naming the transaction and those writes. A
    transaction counts only when every report (N threads and one) shows it, so one noisy timing does not."""
    delta = {(str(c.get("table", "")).lower(), str(c.get("mutation", "")).lower())
             for c in coverage if c.get("behavior") == "supported_delta"}
    found: list[dict[str, str]] = []
    for report in reports:
        out: dict[str, str] = {}
        build_s = float(report.get("build_seconds") or 0.0)
        for cost in report.get("write_costs") or []:
            writes = sorted({(t, verb.lower()) for verb, t in statements(txn_sql.get(cost["txn"], ""))})
            if writes and all(w in delta for w in writes) and is_rebuild(_maintenance_s(cost), build_s, cost.get("action", "")):
                out[cost["txn"]] = (f"{cost['txn']} ({', '.join(f'{v} {t}' for t, v in writes)}): maintenance costs as much as "
                                    "a full build; analysis.json maintains these writes incrementally (supported_delta)")
        found.append(out)
    return [line for txn, line in found[0].items() if all(txn in other for other in found)] if found else []


def _maintenance_s(cost: dict) -> float:
    """The accelerator's own time for one write: what the engine reports, else the whole commit and first read."""
    return cost["maint_ms"] / 1000.0 if cost.get("maint_ms", -1) >= 0 else (cost["write_ms"] + cost["read_ms"]) / 1000.0


def _kind_line(kind: str, tables: tuple[str, ...], action: str, subset_ms: float, per_write_s: float, factor: float,
               rebuild: bool) -> str:
    on = f" on {','.join(tables)}" if tables else ""
    how = f" via {action}" if action else ""
    why = ": as costly as a rebuild; likely a full rebuild per write, make it incremental" if rebuild else ""
    return f"{kind}{on}{how}: {subset_ms:.0f} ms on the subset, projected ~{per_write_s:.1f} s per write at full scale (x{factor:.0f}){why}"


def project(reports: list[dict], *, profiles: dict[str, tuple[str, tuple[str, ...]]], dependent: list[str],
            scale: Callable[[Iterable[str]], float], probes_full: int, budget_s: float) -> Projection:
    """The full gate's projected write seconds from the suite reports of the quick tier's differential runs."""
    r_dep = scale(dependent)
    out = Projection(budget_s=budget_s)
    worst: dict[tuple[str, tuple[str, ...]], tuple[float, str]] = {}
    for report in reports:
        build_s = float(report.get("build_seconds") or 0.0)
        costs = [(*profiles.get(c["txn"], ("other", ())), c.get("action", ""), _maintenance_s(c))
                 for c in report.get("write_costs") or []]
        probe_ms = [float(ms) for ms in report.get("probe_costs_ms") or []]
        if probe_ms:  # the adversarial probes: every table the state depends on, scaled to the full gate's probe count
            costs += [("adversarial_probe", tuple(dependent), "", sum(probe_ms) / len(probe_ms) / 1000.0)] * probes_full
        for kind, tables, action, cost_s in costs:
            rebuild = is_rebuild(cost_s, build_s, action)
            r_touch = scale(tables) if tables else r_dep
            factor = max(r_dep if rebuild else 1.0, r_touch if kind.startswith("bulk") else math.sqrt(r_touch))
            out.total_s += cost_s * factor
            line = _kind_line(kind, tables, action, cost_s * 1000, cost_s * factor, factor, rebuild)
            if cost_s * factor > worst.get((kind, tables), (0.0, ""))[0]:
                worst[(kind, tables)] = (cost_s * factor, line)
    ranked = sorted(worst.values(), reverse=True)
    out.lines = [line for _, line in ranked]
    out.worst = out.lines[0] if out.lines else ""
    return out


def load_reports(gates_dir: Path) -> list[dict]:
    reports = []
    for path in report_paths(gates_dir):
        try:
            report = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        if "write_costs" in report:
            reports.append(report)
    return reports


if __name__ == "__main__":
    raise SystemExit(main())
