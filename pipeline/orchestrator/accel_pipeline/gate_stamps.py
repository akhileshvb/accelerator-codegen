"""Which gates already passed on the code as it is now.

run_gate records the code stamp (build_stamp.code_stamp) of every passing gate; a caller that
wants to skip a gate asks reusable_gate_result. The stamp covers the query's directory and every
shared file changed since the commit, so an edit anywhere the gate could see invalidates it.

Timing gates (TIMING_GATES) measure acceptance numbers: they are reused only from a run of the
same stage's acceptance tier, never from another stage and never from the iteration tier.
"""

from __future__ import annotations

import fcntl
import json
from pathlib import Path
from typing import TYPE_CHECKING, Any

from accel_pipeline.build_stamp import code_stamp
from accel_pipeline.config import PipelineConfig
from accel_pipeline.jsonio import read_json
from accel_pipeline.layout import query_dir
from accel_pipeline.paths import gates_dir_for_query, query_artifacts_dir

if TYPE_CHECKING:
    from accel_pipeline.gates import GateResult

STAMPS_FILE = "gate_stamps.json"
ITERATION_TIER = "optimization"  # the change_kind whose gate runs use the cheaper iteration bench settings
TIMING_GATES = frozenset({
    "aux_bench_sweep", "aux_cost_model_calibrate", "aux_speedup_verification", "baseline", "fairness", "profile", "sf10",
})


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
    entry = {"stamp": stamp, "change_kind": change_kind, "result": str(result or "")}
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
    if gate in TIMING_GATES:
        return bool(change_kind) and change_kind != ITERATION_TIER and entry.get("change_kind") == change_kind
    return True


def reusable_gate_result(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int, gate: str,
                         *, change_kind: str = "") -> GateResult | None:
    """The passing result of `gate` on exactly the current code (a foreground run, else a background
    run), or None: run the gate."""
    from accel_pipeline.background_gates import reusable_background_result
    from accel_pipeline.gates import GateResult

    if passed_at_current_stamp(pipeline_config, workload, query, gate, change_kind=change_kind):
        entry = _read(_stamps_path(pipeline_config, workload, query))[gate]
        path = Path(entry["result"]) if entry.get("result") else None
        data = read_json(path) if path else {}
        if data.get("status") == "pass":
            return GateResult(name=gate, status="pass", data={**data, "reused": True}, path=path)
    if gate in TIMING_GATES:
        return None
    return reusable_background_result(pipeline_config, workload, query, gate)
