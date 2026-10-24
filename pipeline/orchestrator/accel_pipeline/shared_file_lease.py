"""Cross-query shared-file lease for a shared git tree.

Workers may run concurrently with separate build dirs. Edits (and builds that
compile) shared registration/CMake sources must hold this lease so two agents
cannot interleave patches on the same files.
"""

from __future__ import annotations

import atexit
import fnmatch
import json
import os
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from accel_pipeline.host_lock import FairLock, LockTimeout

DEFAULT_SHARED_PATH_GLOBS = (
    "CMakeLists.txt",
    "cmake/**",
    "cmake/*.cmake",
    # The engine backends (generic only: queries register from their own
    # directories, so these change only in engine_backend runs; builds that
    # compile them take the lease).
    "backends/**",
    # Cross-query evaluation harnesses shared by every query.
    "eval/tests/test_support/**",
    "eval/bench/**/*_arg_variants.*",
    # Shared primitives library: changed only by primitive_publish; builds that
    # read it take the lease so a publish cannot land mid-compile.
    "accelerators/prims/**",
    # Helpers shared by one benchmark's queries (not owned by any one query run).
    "accelerators/*/common/**",
)

_LOCK_DIRNAME = ".locks"
_LOCK_NAME = "shared_tree"
DEFAULT_WAIT_SEC = 300.0
_META_FILENAME = "shared_tree_lease.json"

# Process-local sticky holder (auto-acquired on shared writes).
_sticky_holder: str | None = None
_sticky_repo: Path | None = None
_sticky_lease = None  # SharedFileLease | None; keep fd alive


def locks_dir(repo_root: Path) -> Path:
    path = Path(repo_root) / "pipeline" / _LOCK_DIRNAME
    path.mkdir(parents=True, exist_ok=True)
    return path


def meta_path(repo_root: Path) -> Path:
    return locks_dir(repo_root) / _META_FILENAME


def shared_path_globs(pipeline_config: Any | None = None) -> list[str]:
    if pipeline_config is not None:
        custom = getattr(pipeline_config, "shared_paths", None)
        if isinstance(custom, list) and custom:
            return [str(x) for x in custom]
    return list(DEFAULT_SHARED_PATH_GLOBS)


def rel_path(repo_root: Path, path: Path | str) -> str:
    p = Path(path)
    if not p.is_absolute():
        p = repo_root / p
    return str(p.resolve().relative_to(Path(repo_root).resolve()))


def is_shared_path(
    repo_root: Path,
    path: Path | str,
    *,
    globs: list[str] | None = None,
) -> bool:
    try:
        rel = rel_path(repo_root, path)
    except ValueError:
        return False
    patterns = globs if globs is not None else list(DEFAULT_SHARED_PATH_GLOBS)
    return any(fnmatch.fnmatch(rel, pat) for pat in patterns)


def worker_id_from_env(default: str = "local") -> str:
    for key in ("ACCEL_PIPELINE_WORKER_ID", "ACCEL_PIPELINE_QUERY"):
        raw = os.environ.get(key)
        if raw is not None and str(raw).strip():
            return str(raw).strip()
    return default


@dataclass
class LeaseInfo:
    holder: str
    acquired_at: float
    pid: int
    reason: str = ""
    paths: list[str] | None = None

    def to_dict(self) -> dict[str, Any]:
        return {
            "holder": self.holder,
            "acquired_at": self.acquired_at,
            "pid": self.pid,
            "reason": self.reason,
            "paths": list(self.paths or []),
        }

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> LeaseInfo:
        return cls(
            holder=str(data.get("holder") or ""),
            acquired_at=float(data.get("acquired_at") or 0.0),
            pid=int(data.get("pid") or 0),
            reason=str(data.get("reason") or ""),
            paths=[str(x) for x in (data.get("paths") or [])],
        )


class SharedFileLease:
    """flock around shared tree edits/builds; waiters are served in arrival order, with a bound."""

    def __init__(self, repo_root: Path) -> None:
        self.repo_root = Path(repo_root).resolve()
        self._fp = None
        self._lock = FairLock(locks_dir(self.repo_root), _LOCK_NAME)

    def read_meta(self) -> LeaseInfo | None:
        path = meta_path(self.repo_root)
        if not path.is_file():
            return None
        try:
            return LeaseInfo.from_dict(json.loads(path.read_text(encoding="utf-8")))
        except (OSError, json.JSONDecodeError, TypeError, ValueError):
            return None

    def _write_meta(self, info: LeaseInfo) -> None:
        path = meta_path(self.repo_root)
        path.write_text(json.dumps(info.to_dict(), indent=2) + "\n", encoding="utf-8")

    def _clear_meta(self) -> None:
        path = meta_path(self.repo_root)
        if path.is_file():
            path.unlink(missing_ok=True)

    def holder(self) -> str | None:
        """The current holder; None when free (a crashed holder's leftover metadata does not count)."""
        info = self.read_meta()
        return info.holder if info and self._lock.is_held() else None

    def _busy_message(self, waited: float) -> str:
        info = self.read_meta()
        who = (
            f"held by {info.holder!r} for {time.time() - info.acquired_at:.0f}s ({info.reason or 'no reason'})"
            if info else "held by another worker"
        )
        return (
            f"shared-file lease not acquired within {waited:.0f}s: {who}, {self._lock.queue_depth()} waiting; "
            "work that does not edit shared files, then retry"
        )

    def acquire(
        self,
        holder: str,
        *,
        timeout_sec: float = DEFAULT_WAIT_SEC,
        poll_sec: float = 0.1,
        reason: str = "",
        paths: list[str] | None = None,
    ) -> LeaseInfo:
        def on_wait(waited: float, ahead: int) -> None:
            print(f"[pipeline] {holder}: waiting {waited:.0f}s for the shared-file lease ({ahead} ahead)", file=sys.stderr, flush=True)

        try:
            self._fp = self._lock.acquire(timeout_sec=max(0.0, float(timeout_sec)), poll_sec=poll_sec, on_wait=on_wait)
        except LockTimeout:
            raise TimeoutError(self._busy_message(timeout_sec)) from None
        info = LeaseInfo(holder=holder, acquired_at=time.time(), pid=os.getpid(), reason=reason, paths=list(paths or []))
        self._write_meta(info)
        return info

    def release(self, holder: str | None = None) -> bool:
        meta = self.read_meta()
        if holder is not None and meta is not None and meta.holder != holder:
            return False
        self._clear_meta()
        if self._fp is not None:
            self._fp.close()
            self._fp = None
        return True

    def ensure_held(
        self,
        holder: str,
        *,
        timeout_sec: float = DEFAULT_WAIT_SEC,
        reason: str = "",
        paths: list[str] | None = None,
    ) -> LeaseInfo:
        meta = self.read_meta()
        if meta is not None and meta.holder == holder and self._fp is not None:
            return meta
        if meta is not None and meta.holder == holder and self._fp is None:
            # Another object in-process may hold the flock via OS; try re-acquire NB.
            return self.acquire(
                holder, timeout_sec=timeout_sec, reason=reason or "rebind", paths=paths
            )
        return self.acquire(
            holder, timeout_sec=timeout_sec, reason=reason, paths=paths
        )


def sticky_holder() -> str | None:
    return _sticky_holder


def sticky_lease_obj() -> SharedFileLease | None:
    return _sticky_lease


def ensure_sticky_lease_for_write(
    repo_root: Path,
    *,
    holder: str | None = None,
    path: str | Path | None = None,
    timeout_sec: float = DEFAULT_WAIT_SEC,
    globs: list[str] | None = None,
) -> LeaseInfo | None:
    """If ``path`` is shared, acquire (or keep) a process-sticky lease.

    Returns lease info when a lease is required/held; ``None`` if path is not shared.
    """
    global _sticky_holder, _sticky_repo, _sticky_lease
    if path is not None and not is_shared_path(repo_root, path, globs=globs):
        return None
    who = holder or worker_id_from_env()
    root = Path(repo_root).resolve()
    if (
        _sticky_holder == who
        and _sticky_repo == root
        and _sticky_lease is not None
        and _sticky_lease._fp is not None
    ):
        meta = _sticky_lease.read_meta()
        if meta and meta.holder == who:
            return meta
    lease = SharedFileLease(root)
    info = lease.ensure_held(
        who,
        timeout_sec=timeout_sec,
        reason=f"shared write {path}" if path else "shared write",
        paths=[str(path)] if path is not None else None,
    )
    _sticky_holder = who
    _sticky_repo = root
    _sticky_lease = lease
    return info


def release_sticky_lease(repo_root: Path | None = None, holder: str | None = None) -> bool:
    global _sticky_holder, _sticky_repo, _sticky_lease
    who = holder or _sticky_holder or worker_id_from_env()
    root = Path(repo_root or _sticky_repo or Path.cwd()).resolve()
    lease = _sticky_lease if _sticky_lease is not None else SharedFileLease(root)
    ok = lease.release(who)
    if _sticky_holder == who:
        _sticky_holder = None
        _sticky_repo = None
        _sticky_lease = None
    return ok


def _atexit_release() -> None:
    if _sticky_holder and _sticky_lease is not None:
        try:
            _sticky_lease.release(_sticky_holder)
        except Exception:
            pass


atexit.register(_atexit_release)


def command_touches_shared_build(cmd: str) -> bool:
    """Heuristic: cmake configure/build of the shared library likely compiles shared TUs."""
    lowered = cmd.lower()
    if "cmake" not in lowered:
        return False
    if "cmake -s" in lowered or "cmake --build" in lowered:
        return True
    return False
