"""Per-query write ownership for shared-tree concurrent packs.

Owned paths (this query's aux module / tests / cost model) may be written freely.
Shared registration paths require the sticky shared-file lease and are blocked
during optimization stages so concurrent workers cannot desync catalog wiring.
Cross-query aux sources are never writable.
"""

from __future__ import annotations

import fnmatch
import re
from pathlib import Path
from typing import Any

from accel_pipeline.shared_file_lease import is_shared_path, shared_path_globs

_OPTIMIZATION_STAGES = frozenset(
    {
        "optimization",
        "optimization_worker",
        "optimization_unified",
        "optimization_unified_hypothesis",
        "optimization_unified_patch",
        "optimization_fix",
    }
)

_FOREIGN_QUERY_RE = re.compile(
    r"(?:^|/)(?:src/generated/(?:tpch|sec_edgar)/q|tests/single-threaded/test_tpch_q|"
    r"tests/single-threaded/test_sec_edgar_q|benchmarks/(?:tpch|sec_edgar)/single-threaded/"
    r"(?:benchmark_q|calibrate_q)|config/q)(\d+)"
)


def is_optimization_stage(stage: str | None) -> bool:
    return (stage or "") in _OPTIMIZATION_STAGES


def owned_path_globs(query: int, *, benchmark: str = "tpch") -> list[str]:
    q = int(query)
    if benchmark == "sec_edgar":
        return [
            f"src/generated/sec_edgar/q{q}/**",
            f"tests/single-threaded/test_sec_edgar_q{q}*.cpp",
            f"benchmarks/sec_edgar/single-threaded/benchmark_q{q}*.cpp",
            f"config/q{q}_aux_cost_model.json",
            f"tests/test_support/q{q}_aux_helpers.hpp",
            f"pipeline/artifacts/**/q{q}/**",
            f"pipeline/artifacts/sec_edgar/q{q}/**",
        ]
    return [
        f"src/generated/tpch/q{q}/**",
        f"tests/single-threaded/test_tpch_q{q}*.cpp",
        f"benchmarks/tpch/single-threaded/benchmark_q{q}*.cpp",
        f"benchmarks/tpch/single-threaded/calibrate_q{q}*.cpp",
        f"config/q{q}_aux_cost_model.json",
        f"tests/test_support/q{q}_aux_helpers.hpp",
        f"pipeline/artifacts/tpch/q{q}/**",
        f"pipeline/artifacts/**/q{q}/**",
    ]


def _rel(repo_root: Path, path: Path | str) -> str | None:
    p = Path(path)
    if not p.is_absolute():
        p = repo_root / p
    try:
        return str(p.resolve().relative_to(Path(repo_root).resolve()))
    except ValueError:
        return None


def query_id_from_rel(rel: str) -> int | None:
    m = _FOREIGN_QUERY_RE.search(rel.replace("\\", "/"))
    if not m:
        return None
    return int(m.group(1))


def is_owned_path(
    repo_root: Path,
    path: Path | str,
    *,
    query: int,
    benchmark: str = "tpch",
) -> bool:
    rel = _rel(repo_root, path)
    if rel is None:
        return False
    for pat in owned_path_globs(query, benchmark=benchmark):
        if fnmatch.fnmatch(rel, pat):
            return True
    # Also allow generic write_paths that are not query-scoped (handled by caller).
    return False


def is_cross_query_path(
    repo_root: Path,
    path: Path | str,
    *,
    query: int,
) -> bool:
    """True when path belongs to a different TPC-H/SEC query id."""
    rel = _rel(repo_root, path)
    if rel is None:
        return False
    other = query_id_from_rel(rel)
    return other is not None and other != int(query)


def write_allowed(
    repo_root: Path,
    path: Path | str,
    *,
    query: int,
    stage: str = "",
    write_paths: list[str] | None = None,
    pipeline_config: Any | None = None,
    benchmark: str = "tpch",
) -> tuple[bool, str]:
    """Return (allowed, reason). Shared writes are denied in optimization stages."""
    rel = _rel(repo_root, path)
    if rel is None:
        return False, "path outside repo"

    # Architecture audit / per-layer critique: read-only on adapters; artifacts only.
    if stage == "architecture_audit" or stage == "layer_critique" or stage.startswith(
        "critique_"
    ):
        if fnmatch.fnmatch(rel, "pipeline/artifacts/**"):
            return True, "critique-artifacts"
        return (
            False,
            f"{stage} may only write under pipeline/artifacts/** (got {rel})",
        )

    if is_cross_query_path(repo_root, path, query=query):
        other = query_id_from_rel(rel)
        return False, f"cross-query write denied: {rel} belongs to Q{other}, worker owns Q{query}"

    if is_owned_path(repo_root, path, query=query, benchmark=benchmark):
        return True, "owned"

    globs = shared_path_globs(pipeline_config)
    if is_shared_path(repo_root, path, globs=globs):
        if is_optimization_stage(stage):
            return (
                False,
                f"shared registration write denied during {stage}: {rel} "
                "(registration is frozen for concurrent optimization; edit owned aux files only)",
            )
        for pat in write_paths or []:
            if fnmatch.fnmatch(rel, pat):
                return True, "shared"
        return False, f"write not allowed (shared, not in write_paths): {rel}"

    # Fall back to configured write_paths (impl may create new benches etc.) but
    # still block anything that resolves to a foreign query id (already checked).
    for pat in write_paths or []:
        if fnmatch.fnmatch(rel, pat):
            # Broad globs like src/generated/tpch/q* must not open other queries.
            if "q*" in pat or "q?" in pat:
                other = query_id_from_rel(rel)
                if other is not None and other != int(query):
                    return False, f"cross-query write denied via glob {pat}: {rel}"
                if other == int(query):
                    return True, "write_paths-owned"
                # Shared-ish path matched by write_paths during impl
                if is_optimization_stage(stage) and is_shared_path(repo_root, path, globs=globs):
                    return False, f"shared write denied during {stage}: {rel}"
            return True, "write_paths"

    return False, f"write not allowed: {rel}"


_SHELL_WRITE_HINTS = re.compile(
    r"(?:^|[\s;|&])(?:tee\b|sed\s+-i|cp\s|mv\s|rm\s|truncate\b|dd\s|>|>>|"
    r"python3?\b|perl\b|ruby\b|apply_patch\b)",
    re.IGNORECASE,
)
_SHELL_READONLY_PREFIX = re.compile(
    r"^(?:(?:cd\s+\S+\s*(?:&&|;)\s*)*)?(?:"
    r"rg\b|grep\b|egrep\b|fgrep\b|sed\s+-n\b|nl\b|head\b|tail\b|cat\b|"
    r"less\b|bat\b|wc\b|stat\b|ls\b|find\b|git\s+(?:status|diff|log|show|blame)\b|"
    r"nm\b|objdump\b|readelf\b"
    r")\b",
    re.IGNORECASE,
)
_FOREIGN_PATH_IN_CMD = re.compile(
    r"(?:src/generated/(?:tpch|sec_edgar)/q|tests/single-threaded/test_(?:tpch|sec_edgar)_q|"
    r"benchmarks/(?:tpch|sec_edgar)/single-threaded/(?:benchmark_q|calibrate_q)|config/q)(\d+)",
    re.IGNORECASE,
)


def shell_command_cross_query_write(cmd: str, *, query: int) -> str | None:
    """If command likely writes (or mutates) a foreign query path, return an error."""
    foreign_ids = {int(m.group(1)) for m in _FOREIGN_PATH_IN_CMD.finditer(cmd)}
    foreign_ids.discard(int(query))
    if not foreign_ids:
        return None

    stripped = cmd.strip()
    # Pure read-only inspection of peer sources is allowed (reference shopping).
    # Anything else that mentions a foreign query path is denied — including
    # python/sed-i/cp that peers used to "repair" another query's compile break.
    if _SHELL_READONLY_PREFIX.match(stripped) and not _SHELL_WRITE_HINTS.search(stripped):
        # Still deny read-only commands that redirect into foreign paths.
        if re.search(r"(?:^|[;|&])\s*(?:tee|cp|mv)\b", stripped):
            pass
        elif not re.search(r"(>|>>)", stripped):
            return None

    other = sorted(foreign_ids)[0]
    return (
        f"shell access that may mutate foreign query Q{other} denied "
        f"(worker owns Q{query}). Edit only owned aux sources; peer compile "
        f"errors are another worker's problem under worktree isolation. cmd={cmd[:200]}"
    )
