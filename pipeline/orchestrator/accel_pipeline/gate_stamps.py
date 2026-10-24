"""Which gates already passed on the code as it is now.

run_gate records the code stamp (build_stamp.code_stamp) of every passing gate; a caller that
wants to skip a gate asks passed_at_current_stamp. The stamp covers the query's directory and the
shared files changed since the commit, so an edit anywhere the gate could see invalidates it.
"""

from __future__ import annotations

import fcntl
import json
from pathlib import Path
from typing import Any

from accel_pipeline.build_stamp import code_stamp
from accel_pipeline.config import PipelineConfig
from accel_pipeline.layout import query_dir
from accel_pipeline.paths import gates_dir_for_query, query_artifacts_dir

STAMPS_FILE = "gate_stamps.json"


def current_stamp(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int) -> str:
    """The code stamp of this query's tree now."""
    bench = str(workload.get("benchmark_name") or "")
    return code_stamp(pipeline_config.repo_root, query_dir=query_dir(bench, query) if bench else "")


def _stamps_path(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int) -> Path:
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    return gates_dir_for_query(art) / STAMPS_FILE


def _read(path: Path) -> dict[str, str]:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def record_pass(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int, gate: str, stamp: str) -> None:
    path = _stamps_path(pipeline_config, workload, query)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.with_suffix(".lock").open("a") as lock:  # parallel gates record concurrently
        fcntl.flock(lock, fcntl.LOCK_EX)
        path.write_text(json.dumps({**_read(path), gate: stamp}, indent=2), encoding="utf-8")


def passed_at_current_stamp(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int, gate: str) -> bool:
    """True when `gate` last passed on exactly the code the tree has now."""
    recorded = _read(_stamps_path(pipeline_config, workload, query)).get(gate)
    return bool(recorded) and recorded == current_stamp(pipeline_config, workload, query)
