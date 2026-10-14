"""Detect shared-tree infrastructure failures and repair without burning agent turns."""

from __future__ import annotations

import re
import shutil
from pathlib import Path
from typing import Any

from accel_pipeline.paths import query_artifacts_dir
from accel_pipeline.shared_file_lease import release_sticky_lease, worker_id_from_env

_UNSUPPORTED = re.compile(r"unsupported shared query(?:\s+binding)?", re.IGNORECASE)
_REBUILD_REVERT = re.compile(r"rebuild after (?:revert|restore) failed", re.IGNORECASE)
_FOREIGN_COMPILE = re.compile(
    r"(?:error|fatal error):.*?accelerators/(?:tpch|sec_edgar)/q(\d+)/",
    re.IGNORECASE | re.DOTALL,
)
_FOREIGN_COMPILE_PATH = re.compile(
    r"accelerators/(?:tpch|sec_edgar)/q(\d+)/[^\s:]+",
    re.IGNORECASE,
)


def classify_infra_failure(text: str, *, query: int) -> str | None:
    """Return a short infra reason if ``text`` is a shared-tree / catalog failure."""
    if not text:
        return None
    if _UNSUPPORTED.search(text):
        return "unsupported_shared_query"
    if _REBUILD_REVERT.search(text):
        return "rebuild_after_revert_failed"
    # Prefer the path on an actual error/fatal/undefined line — not every TU
    # mentioned while cmake compiles peers in the same shared library.
    foreign_from_errors: set[int] = set()
    for line in text.splitlines():
        low = line.lower()
        if not (
            "error:" in low
            or "fatal error:" in low
            or "undefined reference" in low
        ):
            continue
        for m in _FOREIGN_COMPILE_PATH.finditer(line):
            foreign_from_errors.add(int(m.group(1)))
    foreign_from_errors.discard(int(query))
    if foreign_from_errors:
        return f"foreign_file_compile:q{sorted(foreign_from_errors)[0]}"
    m = _FOREIGN_COMPILE.search(text)
    if m and int(m.group(1)) != int(query):
        return f"foreign_file_compile:q{m.group(1)}"
    return None


def release_shared_lease(repo_root: Path, *, query: int, worker_id: str | None = None) -> bool:
    holder = worker_id or worker_id_from_env(f"q{query}")
    return bool(release_sticky_lease(repo_root, holder=holder))


def _restore_aux_from_snapshot(repo_root: Path, query: int, workload: dict[str, Any] | None = None) -> list[str]:
    art = query_artifacts_dir(repo_root, query, workload=workload)
    # The snapshot mirrors repo-relative paths; everything a query owns (aux module,
    # tests, cost model under config/) lives in its directory accelerators/<bench>/q<N>/.
    snap_root = art / "best_valid_baseline_snapshot"
    bench = str((workload or {}).get("benchmark_name") or "tpch")
    rel_q = Path("accelerators") / bench / f"q{query}"
    snap = snap_root / rel_q
    restored: list[str] = []
    if not snap.is_dir():
        return restored
    for src in sorted(snap.rglob("*")):
        if src.is_file():
            dest = repo_root / src.relative_to(snap_root)
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dest)
            restored.append(str(dest.relative_to(repo_root)))
    return restored


def repair_foreign_query_sources(
    repo_root: Path,
    *,
    owner_query: int,
    pack_queries: list[int] | None = None,
    only_queries: list[int] | None = None,
) -> dict[str, Any]:
    """Restore peer aux sources from best_valid snapshots.

    Prefer ``only_queries`` (e.g. the single foreign id from a compile error).
    Blasting every pack query wipes concurrent workers' in-progress edits.
    """
    if only_queries is not None:
        targets = list(only_queries)
    else:
        targets = list(pack_queries or [])
    if not targets and only_queries is None:
        # Conservative default: do not scan the whole artifacts tree under pack
        # concurrency — caller should pass an explicit foreign id.
        targets = []
    restored: dict[str, list[str]] = {}
    for q in sorted(set(targets)):
        if int(q) == int(owner_query):
            continue
        files = _restore_aux_from_snapshot(repo_root, int(q))
        if files:
            restored[f"q{q}"] = files
    return {"owner_query": owner_query, "restored": restored}


def handle_infra_failure(
    pipeline_config: Any,
    workload: dict[str, Any],
    query: int,
    *,
    text: str,
    pack_queries: list[int] | None = None,
) -> dict[str, Any] | None:
    """If ``text`` is an infra failure, repair tree + release lease and return info."""
    reason = classify_infra_failure(text, query=query)
    if not reason:
        return None
    repo_root = Path(pipeline_config.repo_root)
    worker_id = getattr(pipeline_config, "worker_id", None)
    released = release_shared_lease(repo_root, query=query, worker_id=worker_id)

    only: list[int] | None = None
    m = re.match(r"foreign_file_compile:q(\d+)$", reason)
    if m:
        only = [int(m.group(1))]
    elif reason == "unsupported_shared_query":
        # Catalog desync — do not mass-restore peers; release lease + own restore only.
        only = []
    elif pack_queries:
        only = list(pack_queries)

    repair = repair_foreign_query_sources(
        repo_root, owner_query=query, pack_queries=pack_queries, only_queries=only
    )
    # Restore this query from its own baseline so a bad mid-edit does not linger.
    own = _restore_aux_from_snapshot(repo_root, query, workload=workload)
    if own:
        repair.setdefault("restored", {})[f"q{query}_own"] = own
    return {
        "infra_reason": reason,
        "lease_released": released,
        "repair": repair,
        "fail_fast": True,
    }
