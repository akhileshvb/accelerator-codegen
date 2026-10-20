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
import time
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterator

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
_LOCK_FILENAME = "shared_tree.lock"
_META_FILENAME = "shared_tree_lease.json"

# Process-local sticky holder (auto-acquired on shared writes).
_sticky_holder: str | None = None
_sticky_repo: Path | None = None
_sticky_lease = None  # SharedFileLease | None; keep fd alive


def locks_dir(repo_root: Path) -> Path:
    path = Path(repo_root) / "pipeline" / _LOCK_DIRNAME
    path.mkdir(parents=True, exist_ok=True)
    return path


def lock_path(repo_root: Path) -> Path:
    return locks_dir(repo_root) / _LOCK_FILENAME


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
    """fcntl flock around shared tree edits/builds."""

    def __init__(self, repo_root: Path) -> None:
        self.repo_root = Path(repo_root).resolve()
        self._fp = None

    def _open_lock(self):
        import fcntl

        path = lock_path(self.repo_root)
        path.parent.mkdir(parents=True, exist_ok=True)
        fp = path.open("a+", encoding="utf-8")
        return fp, fcntl

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
        info = self.read_meta()
        return info.holder if info else None

    def acquire(
        self,
        holder: str,
        *,
        timeout_sec: float = 600.0,
        poll_sec: float = 0.25,
        reason: str = "",
        paths: list[str] | None = None,
    ) -> LeaseInfo:
        import fcntl

        deadline = time.monotonic() + max(0.0, float(timeout_sec))
        last_holder = None
        while True:
            fp, _ = self._open_lock()
            try:
                fcntl.flock(fp.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                fp.close()
                meta = self.read_meta()
                last_holder = meta.holder if meta else "?"
                if time.monotonic() >= deadline:
                    raise TimeoutError(
                        f"shared-file lease timed out after {timeout_sec}s "
                        f"(held by {last_holder!r})"
                    )
                time.sleep(poll_sec)
                continue

            # Non-blocking lock acquired — keep fp open for the lease lifetime.
            self._fp = fp
            info = LeaseInfo(
                holder=holder,
                acquired_at=time.time(),
                pid=os.getpid(),
                reason=reason,
                paths=list(paths or []),
            )
            self._write_meta(info)
            return info

    def release(self, holder: str | None = None) -> bool:
        import fcntl

        meta = self.read_meta()
        if holder is not None and meta is not None and meta.holder != holder:
            return False
        self._clear_meta()
        if self._fp is not None:
            try:
                fcntl.flock(self._fp.fileno(), fcntl.LOCK_UN)
            except OSError:
                pass
            try:
                self._fp.close()
            except OSError:
                pass
            self._fp = None
        return True

    def ensure_held(
        self,
        holder: str,
        *,
        timeout_sec: float = 600.0,
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


@contextmanager
def hold_shared_lease(
    repo_root: Path,
    holder: str,
    *,
    timeout_sec: float = 600.0,
    reason: str = "",
    paths: list[str] | None = None,
) -> Iterator[LeaseInfo]:
    lease = SharedFileLease(repo_root)
    info = lease.acquire(
        holder, timeout_sec=timeout_sec, reason=reason, paths=paths
    )
    try:
        yield info
    finally:
        lease.release(holder)


def sticky_holder() -> str | None:
    return _sticky_holder


def sticky_lease_obj() -> SharedFileLease | None:
    return _sticky_lease


def ensure_sticky_lease_for_write(
    repo_root: Path,
    *,
    holder: str | None = None,
    path: str | Path | None = None,
    timeout_sec: float = 600.0,
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
