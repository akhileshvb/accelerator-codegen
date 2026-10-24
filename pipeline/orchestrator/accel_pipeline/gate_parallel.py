"""Run a batch of gates fast: cheap likely-to-fail gates first, independent gates in parallel.

    run_gates_parallel(pipeline_config, ["aux_unit", "aux_correctness", ...], query, workload)

returns one GateResult per name, in the order given. The caller decides what a result means
(require_gate_pass); a gate that was not run because an earlier one failed has status "cancelled".
"""

from __future__ import annotations

import os
import sys
import threading
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from typing import Any, Literal, Sequence

from accel_pipeline.config import PipelineConfig
from accel_pipeline.core_budget import jobs_per_gate
from accel_pipeline.engine_host import instance_tag
from accel_pipeline.gate_stamps import reusable_gate_result
from accel_pipeline.gates import GATES_ALLOW_SKIP, GateResult, run_gate
from accel_pipeline.pipeline_log import log_gate_batch_start


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
    reuse: a gate that already passed on the code as it is now (gate_stamps, background gates) is not
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
