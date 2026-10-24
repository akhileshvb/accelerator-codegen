"""Run a batch of gates fast: cheap likely-to-fail gates first, independent gates in parallel.

    run_gates_parallel(pipeline_config, ["aux_unit", "aux_correctness", ...], query, workload)

returns one GateResult per name, in the order given. The caller decides what a result means
(require_gate_pass); a gate that was not run because an earlier one failed has status "cancelled".
"""

from __future__ import annotations

import os
import threading
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from typing import Any, Literal, Sequence

from accel_pipeline.config import PipelineConfig
from accel_pipeline.gate_stamps import passed_at_current_stamp
from accel_pipeline.gates import GATES_ALLOW_SKIP, GateResult, run_gate
from accel_pipeline.jsonio import read_json
from accel_pipeline.paths import gates_dir_for_query, query_artifacts_dir


@dataclass(frozen=True)
class GateGroup:
    """Gates that run together. "serial": one after the other; "parallel": at once, each on its own
    engine instance (the instance loads its dataset once, then stays warm)."""

    names: tuple[str, ...]
    mode: Literal["serial", "parallel"]


# Order matters: failures surface in the cheap gates before the minutes-long ones start.
DEFAULT_GROUPS: tuple[GateGroup, ...] = (
    GateGroup(("aux_unit", "aux_correctness", "prims_tests"), "serial"),
    GateGroup(("aux_multi_connection", "aux_cross_accel", "aux_cost_model_calibrate"), "parallel"),
    GateGroup(("aux_bench_sweep", "aux_tsan", "module_tsan", "slot_tsan"), "serial"),
)


def _ok(result: GateResult) -> bool:
    return result.status == "pass" or (result.status == "skip" and result.name in GATES_ALLOW_SKIP)


def _cancelled(name: str, culprit: str) -> GateResult:
    return GateResult(name=name, status="cancelled", data={"status": "cancelled", "reason": f"not run: {culprit} failed"})


def _reused(pipeline_config: PipelineConfig, name: str, query: int, workload: dict[str, Any]) -> GateResult:
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    path = gates_dir_for_query(art) / f"{name}.json"
    data = {**read_json(path), "status": "pass", "reused": True}
    return GateResult(name=name, status="pass", data=data, path=path if path.is_file() else None)


def _instance_env(workload: dict[str, Any], worker_id: str, name: str) -> dict[str, str]:
    """A distinct engine instance (and state dir, when the caller pinned one) for a gate that runs beside others."""
    engine = str(workload.get("engine_name") or "engine")
    env = {"ACCEL_ENGINE_INSTANCE": f"{worker_id or 'w0'}-{engine}-{name}"}
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
    reuse: a gate that already passed on the code as it is now (gate_stamps) is not run again;
      its result carries reused=True.
    """
    grouped = {name for group in groups for name in group.names}
    wanted = list(dict.fromkeys(names))
    plan: list[tuple[str, list[str]]] = [("serial", [n for n in wanted if n not in grouped])]
    plan += [(group.mode, [n for n in group.names if n in wanted]) for group in groups]
    limit = jobs if jobs is not None else pipeline_config.quick_tier.gate_jobs
    cancel = threading.Event()
    failed: list[str] = []

    def run_one(name: str, *, isolated: bool) -> GateResult:
        if cancel.is_set():
            return _cancelled(name, failed[0])
        if reuse and passed_at_current_stamp(pipeline_config, workload, query, name):
            return _reused(pipeline_config, name, query, workload)
        result = run_gate(
            pipeline_config, name, query, workload,
            change_kind=change_kind, iteration=iteration, gate_index=wanted.index(name) + 1, gate_count=len(wanted),
            env=_instance_env(workload, pipeline_config.worker_id, name) if isolated else None,
            cancel=cancel,
        )
        if fail_fast and not _ok(result) and result.status != "cancelled" and not cancel.is_set():
            failed.append(name)
            cancel.set()
        return result

    results: dict[str, GateResult] = {}
    for mode, batch in plan:
        if mode == "parallel" and len(batch) > 1:
            with ThreadPoolExecutor(max_workers=limit or len(batch)) as pool:
                futures = {name: pool.submit(run_one, name, isolated=True) for name in batch}
            results.update({name: future.result() for name, future in futures.items()})
        else:
            for name in batch:
                results[name] = run_one(name, isolated=False)
    return [results[name] for name in wanted]
