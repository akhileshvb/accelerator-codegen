"""Gates that run in the background while the agent keeps working.

request_gate(background=True) starts one (start_background_gate); gate_status reads it
(background_status). At stage exit the orchestrator asks reusable_background_result before
running a gate: a passing background run is reused only if the code stamp is the same at its
start, at its end and now, so a result is never reused for code it did not see.

State: <artifacts>/gates/bg/<gate>.json; the gate's own files go under <artifacts>/gates/bg/<gate>/,
so a background run never overwrites the canonical gate result. Each background gate runs on its
own engine instance; builds share the build dir under the build lock.
"""

from __future__ import annotations

import argparse
import contextlib
import json
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

from accel_pipeline.config import PipelineConfig
from accel_pipeline.gate_stamps import check_version, current_stamp
from accel_pipeline.gates import GateResult
from accel_pipeline.paths import gates_dir_for_query, query_artifacts_dir

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
    """Start `gate` in a detached process and return its state; a gate already running is left alone.
    Only quick_tier.background_gates may run this way."""
    if gate not in pipeline_config.quick_tier.background_gates:
        return {"gate": gate, "status": "refused",
                "reason": f"only {list(pipeline_config.quick_tier.background_gates)} can run in the background"}
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
        [sys.executable, "-m", "accel_pipeline.background_gates", "--repo-root", str(pipeline_config.repo_root),
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
    from accel_pipeline.config import apply_workload_overrides, load_pipeline_config, load_workload
    from accel_pipeline.gates import run_gate

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


if __name__ == "__main__":
    raise SystemExit(main())
