"""MCP tools of a pack worker: the shared-file lease around edits to the files every worker shares."""

from __future__ import annotations

import json
from typing import Callable

from accel_pipeline.config import PipelineConfig
from accel_pipeline.pack.shared_file_lease import (
    ensure_sticky_lease_for_write,
    release_sticky_lease,
    worker_id_from_env,
)


def callables(pipeline_config: PipelineConfig, query: int) -> dict[str, Callable[..., str]]:
    holder = pipeline_config.worker_id or worker_id_from_env(f"q{query}")

    def acquire_shared_lease(reason: str = "", timeout_sec: float = 300.0) -> str:
        """Hold shared-file lease before editing catalog/CMake/shared registration files."""
        info = ensure_sticky_lease_for_write(
            pipeline_config.repo_root,
            holder=holder,
            path=None,
            timeout_sec=min(float(timeout_sec), pipeline_config.concurrency.shared_lease_wait_sec),
        )
        return json.dumps(
            {"acquired": True, "holder": holder, "reason": reason, "lease": info.to_dict() if info else {}}, indent=2
        )

    def release_shared_lease() -> str:
        """Release sticky shared-file lease after shared edits + rebuild."""
        return json.dumps({"released": release_sticky_lease(pipeline_config.repo_root, holder=holder), "holder": holder})

    return {"acquire_shared_lease": acquire_shared_lease, "release_shared_lease": release_shared_lease}


def register(tool, call) -> None:
    @tool(read_only=False)
    def acquire_shared_lease(reason: str = "", timeout_sec: float = 300.0) -> str:
        """Hold shared-file lease before editing catalog/CMake/shared registration files."""
        return call("acquire_shared_lease", reason, timeout_sec)

    @tool(read_only=False)
    def release_shared_lease() -> str:
        """Release sticky shared-file lease after shared edits + rebuild."""
        return call("release_shared_lease")
