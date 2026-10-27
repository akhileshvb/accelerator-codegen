"""Per-query worker isolation for concurrent packs.

Each concurrent pack worker gets:
- its own CMake build directory (``build-release-{worker_id}``)
- its own Codex home (MCP env sidecar cannot collide)
- a stable ``ACCEL_PIPELINE_WORKER_ID`` used by the shared-file lease
- optionally its own git worktree (``pipeline/worktrees/{worker_id}``) so
  peer aux edits cannot break shared-library compiles
"""

from __future__ import annotations

import os
import re
import shutil
from dataclasses import replace
from pathlib import Path
from typing import Any

from accel_pipeline.codex import ensure_codex_config
from accel_pipeline.config import PipelineConfig


def worker_id_for_query(query: int, workload_name: str = "", engine: str = "") -> str:
    """One worker per query x engine: its build dir, CODEX_HOME and worktree are keyed by this id."""
    base = re.sub(r"[^a-zA-Z0-9_.-]+", "_", workload_name.strip()) if workload_name else ""
    return "_".join(part for part in (base or f"q{query}", engine) if part)


def default_build_dir_for_worker(repo_root: Path, worker_id: str) -> Path:
    # Prefer primary repo so worktree checkouts do not duplicate multi-GB builds.
    primary = os.environ.get("ACCEL_PIPELINE_PRIMARY_REPO")
    root = Path(primary) if primary else Path(repo_root)
    return root / f"build-release-{worker_id}"


def default_codex_home_for_worker(repo_root: Path, worker_id: str) -> Path:
    primary = os.environ.get("ACCEL_PIPELINE_PRIMARY_REPO")
    root = Path(primary) if primary else Path(repo_root)
    return root / "pipeline" / ".codex" / "workers" / worker_id


def _rewrite_build_dir_patterns(patterns: list[str], build_dir_name: str) -> list[str]:
    """Allow both legacy ``build-release`` and this worker's build dir name."""
    out: list[str] = []
    seen: set[str] = set()
    for pat in patterns:
        variants = [pat]
        if "build-release" in pat and build_dir_name != "build-release":
            variants.append(pat.replace("build-release", build_dir_name))
        for variant in variants:
            if variant not in seen:
                seen.add(variant)
                out.append(variant)
    generic = [
        rf"^cmake -S \. -B {re.escape(build_dir_name)}( .*)?$",
        rf"^cmake --build {re.escape(build_dir_name)}( .*)?$",
        rf"^ctest --test-dir {re.escape(build_dir_name)}( .*)?$",
        rf"^(\./)?{re.escape(build_dir_name)}/(test|benchmark|calibrate)_[A-Za-z0-9_]+( .*)?$",
    ]
    for g in generic:
        if g not in seen:
            out.append(g)
            seen.add(g)
    return out


def prepare_codex_home(
    *,
    repo_root: Path,
    worker_id: str,
    base_codex_home: Path | None = None,
) -> Path:
    """Create a per-worker CODEX_HOME with config.toml copied/symlinked from base."""
    base = base_codex_home or (Path(repo_root) / "pipeline" / ".codex")
    dest = default_codex_home_for_worker(repo_root, worker_id)
    dest.mkdir(parents=True, exist_ok=True)
    src_cfg = ensure_codex_config(base / "config.toml")
    dst_cfg = dest / "config.toml"
    if not dst_cfg.exists():
        try:
            dst_cfg.symlink_to(src_cfg.resolve())
        except OSError:
            shutil.copy2(src_cfg, dst_cfg)
    return dest


def apply_worker_isolation(
    pipeline_config: PipelineConfig,
    *,
    query: int,
    workload_name: str = "",
    worker_id: str | None = None,
    build_dir: Path | str | None = None,
    codex_home: Path | str | None = None,
    set_environ: bool = True,
) -> PipelineConfig:
    """Return a PipelineConfig scoped to a concurrent pack worker."""
    wid = worker_id or worker_id_for_query(query, workload_name, pipeline_config.engine_name)
    # Prefer explicit worktree / pack repo root when set.
    env_repo = os.environ.get("ACCEL_PIPELINE_REPO_ROOT")
    repo = Path(env_repo).resolve() if env_repo else Path(pipeline_config.repo_root).resolve()
    primary = Path(
        os.environ.get("ACCEL_PIPELINE_PRIMARY_REPO") or pipeline_config.repo_root
    ).resolve()

    env_build = os.environ.get("ACCEL_PIPELINE_BUILD_DIR")
    if build_dir is not None:
        bdir = Path(build_dir)
        if not bdir.is_absolute():
            bdir = primary / bdir
    elif env_build:
        bdir = Path(env_build)
        if not bdir.is_absolute():
            bdir = primary / bdir
    else:
        bdir = default_build_dir_for_worker(primary, wid)

    env_codex = os.environ.get("ACCEL_PIPELINE_CODEX_HOME")
    if codex_home is not None:
        chome = Path(codex_home)
    elif env_codex:
        chome = Path(env_codex)
    else:
        chome = prepare_codex_home(repo_root=primary, worker_id=wid)
    if not chome.is_absolute():
        chome = primary / chome

    cfg = replace(
        pipeline_config,
        repo_root=repo,
        build_dir=bdir,
        allowed_commands=_rewrite_build_dir_patterns(
            list(pipeline_config.allowed_commands or []), bdir.name
        ),
        worker_id=wid,
        codex_home=chome,
    )

    if set_environ:
        os.environ["ACCEL_PIPELINE_WORKER_ID"] = wid
        os.environ["ACCEL_PIPELINE_BUILD_DIR"] = str(bdir)
        os.environ["ACCEL_PIPELINE_CODEX_HOME"] = str(chome)
        os.environ["ACCEL_PIPELINE_QUERY"] = str(query)
        os.environ["ACCEL_PIPELINE_REPO_ROOT"] = str(repo)
        os.environ.setdefault("ACCEL_PIPELINE_PRIMARY_REPO", str(primary))
        if workload_name:
            os.environ["ACCEL_PIPELINE_WORKLOAD"] = workload_name

    return cfg


def isolation_summary(pipeline_config: PipelineConfig) -> dict[str, Any]:
    return {
        "worker_id": pipeline_config.worker_id or None,
        "build_dir": str(pipeline_config.build_dir),
        "codex_home": str(pipeline_config.codex_home or ""),
        "repo_root": str(pipeline_config.repo_root),
        "primary_repo": os.environ.get("ACCEL_PIPELINE_PRIMARY_REPO") or "",
        "isolation_mode": os.environ.get("ACCEL_PIPELINE_ISOLATION_MODE") or "",
    }


def ensure_private_dataset_db(
    pipeline_config: PipelineConfig,
    dataset_db: str | Path,
    *,
    query: int,
) -> Path:
    """Return a worker-private DuckDB path for concurrent pack benchmarks.

    DuckDB takes an exclusive file lock on the database. Pack workers that all
    point ``ACCEL_TPCH_DB`` at the shared SF1 file fail with conflicting lock;
    copy into the worker build dir instead.
    """
    src = Path(dataset_db)
    build_dir = Path(pipeline_config.build_dir)
    worker_id = pipeline_config.worker_id or ""
    needs_copy = bool(worker_id) or (
        build_dir.name.startswith("build-release-") and build_dir.name != "build-release"
    )
    if not needs_copy:
        return src
    if not src.is_file():
        raise FileNotFoundError(f"dataset db missing: {src}")
    build_dir.mkdir(parents=True, exist_ok=True)
    dest = build_dir / f"worker_q{query}_{src.name}"
    refresh = (
        not dest.is_file()
        or dest.stat().st_size != src.stat().st_size
        or dest.stat().st_mtime < src.stat().st_mtime
    )
    if refresh:
        tmp = dest.with_suffix(dest.suffix + ".tmp")
        shutil.copy2(src, tmp)
        tmp.replace(dest)
    return dest
