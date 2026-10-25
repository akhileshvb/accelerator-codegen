"""Per-worker git worktrees so concurrent pack opts cannot crosstalk on sources.

Each pack worker checks out an isolated tree under ``pipeline/worktrees/<worker_id>``.
Peer edits stay inside that worktree; CMake builds no longer compile another
worker's mid-edit aux sources. Artifacts + CODEX_HOME stay on the primary repo
(via symlink / absolute env paths).
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

from accel_pipeline.prims_library import is_prims_path, refresh_prims
from accel_pipeline.write_ownership import owned_path_globs, query_id_from_rel


def primary_repo_root(repo_root: Path | None = None) -> Path:
    env = os.environ.get("ACCEL_PIPELINE_PRIMARY_REPO")
    if env:
        return Path(env).resolve()
    if repo_root is not None:
        return Path(repo_root).resolve()
    from accel_pipeline.paths import find_repo_root

    return find_repo_root().resolve()


def worktree_dir(primary: Path, worker_id: str) -> Path:
    return Path(primary) / "pipeline" / "worktrees" / worker_id


def _run_git(primary: Path, args: list[str], *, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", *args],
        cwd=str(primary),
        capture_output=True,
        text=True,
        check=check,
    )


def remove_worker_worktree(primary: Path, worker_id: str) -> None:
    dest = worktree_dir(primary, worker_id)
    _run_git(primary, ["worktree", "remove", "--force", str(dest)], check=False)
    if dest.exists():
        shutil.rmtree(dest, ignore_errors=True)
    # Drop leftover branch tip if present (best-effort).
    branch = f"pack/{worker_id}"
    _run_git(primary, ["branch", "-D", branch], check=False)


def _link_shared_pipeline_dirs(primary: Path, dest: Path) -> None:
    """Point worktree pipeline config/prompts/gates at the primary tree.

    Workload YAMLs and many pack configs are often untracked; ``git worktree add``
    only materializes HEAD, so workers would miss ``pipeline/config/workloads/*.yaml``.
    Symlinking keeps a single source of truth without copying.
    """
    for rel in (
        "pipeline/config",
        "pipeline/prompts",
        "pipeline/gates",
        "pipeline/schemas",
        "pipeline/templates",
    ):
        src = primary / rel
        if not src.exists():
            continue
        dst = dest / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        if dst.is_symlink() or dst.is_file():
            dst.unlink()
        elif dst.is_dir():
            shutil.rmtree(dst)
        dst.symlink_to(src.resolve())


def _link_shared_artifacts(primary: Path, dest: Path) -> None:
    """Point worktree artifacts at the primary tree so digests/checkpoints stay shared."""
    primary_art = primary / "pipeline" / "artifacts"
    primary_art.mkdir(parents=True, exist_ok=True)
    wt_pipe = dest / "pipeline"
    wt_pipe.mkdir(parents=True, exist_ok=True)
    art = wt_pipe / "artifacts"
    if art.is_symlink() or art.is_file():
        art.unlink()
    elif art.is_dir():
        shutil.rmtree(art)
    art.symlink_to(primary_art.resolve())


def _seed_generated_sources(primary: Path, dest: Path) -> None:
    """Copy primary working-tree sources the worktree needs to compile.

    ``git worktree add`` only materializes HEAD. Aux modules are often untracked,
    and shared headers (e.g. engine backend files) are frequently dirty on the
    primary checkout — both must be overlaid or every worker fails preflight.

    Uses ``shutil.copy`` (not ``copy2``) so destination mtimes are "now". Pack
    build dirs persist across worktree resets; preserving source mtimes made
    CMake skip rebuilds of critical units (e.g. aux TF registration) and left
    stale ``.o`` files that omitted ``accel_tpch_q*_aux`` table functions.
    """
    rels = (
        "accelerators",  # query trees, <bench>/common, prims (re-mirrored by refresh_prims)
        "backends",
        "substrate",
        "eval/harness",  # differential/catalog glue often dirty alongside catalog APIs
        "eval/tests",
        "eval/bench/tpch/single-threaded",
        "workloads",
        "cmake",
    )
    for rel in rels:
        src = primary / rel
        if not src.exists():
            continue
        dst = dest / rel
        if src.is_dir():
            if dst.exists():
                # Overlay copy: keep HEAD files, replace/add from primary working tree.
                for path in src.rglob("*"):
                    if not path.is_file():
                        continue
                    rel_file = path.relative_to(src)
                    target = dst / rel_file
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy(path, target)
            else:
                shutil.copytree(src, dst, copy_function=shutil.copy)
        else:
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(src, dst)


def _seed_pack_peers_from_baselines(
    primary: Path,
    dest: Path,
    pack_queries: list[int] | None = None,
) -> None:
    """Prefer each pack peer's best_valid snapshot over mid-edit primary sources.

    Concurrent packs previously left broken peer aux on the primary tree; seeding
    that verbatim makes every worktree fail foreign compiles. When a snapshot
    exists, restore it into the worktree after the working-tree overlay.
    """
    from accel_pipeline.infra_fail_fast import _restore_aux_from_snapshot

    queries = list(pack_queries) if pack_queries is not None else pack_queries_from_env()
    for q in queries:
        _restore_aux_from_snapshot(dest, int(q))


def ensure_worker_worktree(
    primary: Path,
    worker_id: str,
    *,
    reset: bool = True,
    pack_queries: list[int] | None = None,
) -> Path:
    """Create (or reset) an isolated git worktree for ``worker_id``.

    ``reset=True`` removes any prior worktree so the worker starts from the
    primary HEAD snapshot (no leftover peer edits), then seeds untracked
    generated sources from the primary working tree.
    """
    primary = Path(primary).resolve()
    dest = worktree_dir(primary, worker_id)
    dest.parent.mkdir(parents=True, exist_ok=True)
    branch = f"pack/{worker_id}"

    if reset or not (dest / "CMakeLists.txt").is_file():
        remove_worker_worktree(primary, worker_id)
        proc = _run_git(
            primary,
            ["worktree", "add", "-B", branch, str(dest), "HEAD"],
            check=False,
        )
        if proc.returncode != 0:
            raise RuntimeError(
                f"git worktree add failed for {worker_id}:\n"
                f"{(proc.stdout or '') + (proc.stderr or '')}"
            )
        _seed_generated_sources(primary, dest)
        _seed_pack_peers_from_baselines(primary, dest, pack_queries=pack_queries)
        print(
            f"[pipeline] worker worktree ready: {dest.relative_to(primary)}",
            file=sys.stderr,
            flush=True,
        )
    else:
        _seed_generated_sources(primary, dest)
        _seed_pack_peers_from_baselines(primary, dest, pack_queries=pack_queries)
    # Mirror the shared primitives library exactly (also reverts any direct edit).
    refresh_prims(primary, dest)
    _link_shared_pipeline_dirs(primary, dest)
    _link_shared_artifacts(primary, dest)
    return dest


def pack_queries_from_env() -> list[int]:
    raw = os.environ.get("ACCEL_PIPELINE_PACK_QUERIES") or ""
    out: list[int] = []
    for part in raw.split(","):
        part = part.strip()
        if part.isdigit():
            out.append(int(part))
    return out


def _iter_owned_files(repo_root: Path, query: int, *, benchmark: str = "tpch") -> list[Path]:
    import fnmatch

    files: list[Path] = []
    root = Path(repo_root)
    for pat in owned_path_globs(query, benchmark=benchmark):
        # Only sync concrete source/config/test paths — skip artifacts globs.
        if "artifacts" in pat:
            continue
        # Expand simple globs under repo.
        if "**" in pat:
            prefix = pat.split("**", 1)[0].rstrip("/")
            base = root / prefix if prefix else root
            if not base.is_dir():
                continue
            for path in base.rglob("*"):
                if not path.is_file():
                    continue
                rel = str(path.relative_to(root))
                if fnmatch.fnmatch(rel, pat):
                    files.append(path)
        else:
            # basename globs (none today: a query's files all live under its directory)
            parent = root / Path(pat).parent
            name_pat = Path(pat).name
            if not parent.is_dir():
                continue
            for path in parent.glob(name_pat):
                if path.is_file():
                    files.append(path)
    # Dedup
    return sorted({p.resolve() for p in files})


def sync_owned_to_primary(
    *,
    primary: Path,
    worktree: Path,
    query: int,
    benchmark: str = "tpch",
) -> list[str]:
    """Copy this query's owned sources from the worker worktree back to primary."""
    primary = Path(primary).resolve()
    worktree = Path(worktree).resolve()
    if primary == worktree:
        return []
    copied: list[str] = []
    for src in _iter_owned_files(worktree, query, benchmark=benchmark):
        rel = src.relative_to(worktree)
        # The shared primitives library reaches the primary only via primitive_publish.
        if is_prims_path(str(rel)):
            continue
        # Never sync another query id (defense in depth).
        other = query_id_from_rel(str(rel))
        if other is not None and other != int(query):
            continue
        dest = primary / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dest)
        copied.append(str(rel))
    return copied


def isolation_mode_from_env() -> str:
    return (os.environ.get("ACCEL_PIPELINE_ISOLATION_MODE") or "").strip() or "shared_tree"


def using_worktree_isolation() -> bool:
    mode = isolation_mode_from_env()
    if mode in {"worktree", "worktrees", "git_worktree"}:
        return True
    primary = os.environ.get("ACCEL_PIPELINE_PRIMARY_REPO")
    repo = os.environ.get("ACCEL_PIPELINE_REPO_ROOT")
    return bool(primary and repo and Path(primary).resolve() != Path(repo).resolve())
