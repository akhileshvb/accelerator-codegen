"""Write ownership: one glob per layer (see ``accel_pipeline.layout``).

* ``substrate/**`` is never writable by agents;
* ``backends/<engine>/**`` is owned by that engine's ``engine_backend`` run (incl.
  its catalog, ``backends/<engine>/api/catalog/``); accelerator runs never write it;
* the ``host_bootstrap`` stage writes only ``layout.harness_globs(engine)`` of its
  engine's backend (the workload harnesses), nothing else in it and no other backend;
* ``accelerators/<bench>/q<N>/**`` is owned by that query's run (kernels, aux
  module, registration, tests, bench, cost-model config, Verus kit);
  other queries' directories are never writable;
* ``accelerators/prims/**`` changes only through ``primitive_publish`` (agents
  stage candidates under ``pipeline/prims_staging/``);
* ``accelerators/prooflib/**`` changes only through ``prooflib_publish``;
* ``workloads/**`` is read-only to agents.

Everything else (``eval/**``, ``cmake/**``, ``accelerators/<bench>/common``, ...)
falls through to the shared-file lease and the stage's ``write_paths``; shared
registration paths are frozen during optimization stages so concurrent workers
cannot desync wiring.
"""

from __future__ import annotations

import fnmatch
import hashlib
import json
import os
import re
import subprocess
import time
from pathlib import Path
from typing import Any

from accel_pipeline import layout
from accel_pipeline.frozen_paths import FROZEN
from accel_pipeline.prims_library import PROVENANCE_REL, primary_root
from accel_pipeline.prims_library import DENY_MESSAGE as _PRIMS_DENY
from accel_pipeline.prims_library import is_staging_path
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

_FOREIGN_QUERY_RE = re.compile(r"(?:^|/)accelerators/[A-Za-z0-9_]+/q(\d+)(?:/|$)")

SUBSTRATE_DENY = (
    "substrate/** is never writable by agents (the shared, workload- and engine-agnostic "
    "substrate); record what you need in your report instead"
)
WORKLOADS_DENY = "workloads/** (query SQL and schemas) is read-only to agents"
PROOFLIB_DENY = (
    "accelerators/prooflib/** changes only through the prooflib_publish tool "
    "(edit the workspace copy, then publish)"
)


def is_optimization_stage(stage: str | None) -> bool:
    return (stage or "") in _OPTIMIZATION_STAGES


def owned_path_globs(query: int, *, benchmark: str = "tpch", engine: str = "") -> list[str]:
    """What one query x engine run owns: the query directory (one glob) + its artifact tree."""
    q = int(query)
    artifacts = f"pipeline/artifacts/{benchmark}/q{q}" + (f"/{engine}" if engine else "")
    return [layout.query_glob(benchmark, q), f"{artifacts}/**"]


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
    engine: str = "",
) -> bool:
    rel = _rel(repo_root, path)
    if rel is None:
        return False
    for pat in owned_path_globs(query, benchmark=benchmark, engine=engine):
        if fnmatch.fnmatch(rel, pat):
            return True
    # Also allow generic write_paths that are not query-scoped (handled by caller).
    return False


def _run_identity(pipeline_config: Any | None) -> tuple[str, str, str]:
    """(workload_kind, engine_name, benchmark_name) of the run, '' when unknown."""
    if pipeline_config is None:
        return "", "", ""
    return (
        str(getattr(pipeline_config, "workload_kind", "") or ""),
        str(getattr(pipeline_config, "engine_name", "") or ""),
        str(getattr(pipeline_config, "benchmark_name", "") or ""),
    )


def _host_bootstrap_verdict(rel: str, lay: layout.Layer, engine_name: str) -> tuple[bool, str]:
    if lay.kind == "backend" and lay.engine == engine_name and any(
        fnmatch.fnmatch(rel, g) for g in layout.harness_globs(engine_name)
    ):
        return True, "engine-harness"
    return False, (
        f"host_bootstrap write denied: {rel}. It writes only the {engine_name or '<engine>'} "
        f"workload harnesses ({', '.join(layout.harness_globs(engine_name or '<engine>'))}); "
        "the rest of the backend belongs to the engine_backend run, queries to their own runs"
    )


def layer_write_verdict(
    rel: str,
    *,
    query: int,
    benchmark: str = "tpch",
    workload_kind: str = "",
    engine_name: str = "",
    stage: str = "",
) -> tuple[bool, str] | None:
    """The layer rule for rel: (allowed, reason), or None when no layer rule applies."""
    lay = layout.classify(rel)
    engine_run = workload_kind == "engine_backend"
    if stage == "host_bootstrap" and lay.kind in {"backend", "query"}:
        return _host_bootstrap_verdict(rel, lay, engine_name)
    if lay.kind == "substrate":
        return False, f"direct write denied: {rel}. {SUBSTRATE_DENY}"
    if lay.kind == "workloads":
        return False, f"direct write denied: {rel}. {WORKLOADS_DENY}"
    if lay.kind == "prims":
        return False, f"direct write denied: {rel}. {_PRIMS_DENY}"
    if lay.kind == "prooflib":
        return False, f"direct write denied: {rel}. {PROOFLIB_DENY}"
    if lay.kind == "backend":
        if engine_run and engine_name and lay.engine == engine_name:
            return True, "engine-owned"
        if engine_run:
            return False, (
                f"write denied: {rel} belongs to the {lay.engine} backend; this engine_backend "
                f"run owns backends/{engine_name or '<engine>'}/** only"
            )
        return False, (
            f"write denied: {rel} is in backends/{lay.engine}/**, owned by the {lay.engine} "
            "engine_backend run. Accelerator runs register from their own query directory; "
            "record a needed generic backend addition instead of editing the backend"
        )
    if lay.kind == "query":
        if engine_run:
            return False, (
                f"write denied: {rel} is a query directory; engine backends name no query "
                f"(this run owns backends/{engine_name or '<engine>'}/**)"
            )
        if lay.query != int(query) or lay.bench != benchmark:
            return False, (
                f"cross-query write denied: {rel} belongs to {lay.bench} Q{lay.query}, "
                f"worker owns {benchmark} Q{query}"
            )
        return True, "owned"
    return None


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

    kind, engine, run_bench = _run_identity(pipeline_config)
    if run_bench:
        benchmark = run_bench

    # One rule per layer (substrate / backends / query dirs / prims / prooflib / workloads).
    verdict = layer_write_verdict(
        rel, query=query, benchmark=benchmark, workload_kind=kind, engine_name=engine, stage=stage
    )
    if verdict is not None:
        return verdict
    if is_staging_path(rel):
        return True, "prims-staging"

    if is_owned_path(repo_root, path, query=query, benchmark=benchmark, engine=engine):
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

    # Fall back to configured write_paths (layer-protected paths were decided above).
    for pat in write_paths or []:
        if fnmatch.fnmatch(rel, pat):
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
_FOREIGN_PATH_IN_CMD = re.compile(r"accelerators/[A-Za-z0-9_]+/q(\d+)(?:/|\b)", re.IGNORECASE)
# Explicit file mutation (interpreters that merely read a protected file, e.g.
# sql2verus --query workloads/<bench>/q<N>.sql, stay allowed).
_SHELL_MUTATE = re.compile(
    r"(?:^|[\s;|&])(?:tee\b|sed\s+(?:-[a-zA-Z]*i|--in-place)|perl\s+-[a-zA-Z]*i|cp\s|mv\s|rm\s|"
    r"truncate\b|dd\s|touch\s|install\s|ln\s|chmod\s|apply_patch\b|git\s+(?:checkout|restore|apply|rm|mv)\b)"
    r"|(?<![0-9&])>(?!&)",
)
# Layers no agent shell command may mutate (the publish tools are the only way in).
_PROTECTED_PATH_IN_CMD = re.compile(
    r"(?:^|[\s'\"=:(])(?:\./)?(substrate/|workloads/|accelerators/prims/|accelerators/prooflib/)"
)


def _looks_readonly(stripped: str) -> bool:
    if _SHELL_READONLY_PREFIX.match(stripped) and not _SHELL_WRITE_HINTS.search(stripped):
        if re.search(r"(?:^|[;|&])\s*(?:tee|cp|mv)\b", stripped):
            return False
        return not re.search(r"(>|>>)", stripped)
    return False


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
    if _looks_readonly(stripped):
        return None

    other = sorted(foreign_ids)[0]
    return (
        f"shell access that may mutate foreign query Q{other} denied "
        f"(worker owns Q{query}). Edit only owned aux sources; peer compile "
        f"errors are another worker's problem under worktree isolation. cmd={cmd[:200]}"
    )


def shell_command_layer_write(cmd: str) -> str | None:
    """Deny commands that may mutate substrate/, workloads/, prims or prooflib."""
    hits = sorted({m.group(1) for m in _PROTECTED_PATH_IN_CMD.finditer(cmd)})
    if not hits or not _SHELL_MUTATE.search(cmd):
        return None
    return (
        f"shell access that may mutate a protected layer ({', '.join(h + '**' for h in hits)}) "
        "denied: substrate/ and workloads/ are read-only; prims and prooflib change only "
        f"through primitive_publish / prooflib_publish. cmd={cmd[:200]}"
    )


# ---------------------------------------------------------------- post-stage check

# Not the agents' to write by this policy but handled elsewhere (artifacts, frozen paths are
# restored by frozen_paths, the codex home and run dirs are the pipeline's own).
_UNCHECKED_PREFIXES = (
    "pipeline/artifacts/", "pipeline/.codex/", "pipeline/verus_runs/", "pipeline/worktrees/",
    "pipeline/.locks/", PROVENANCE_REL,
    *(f if "." in f else f + "/" for f in FROZEN if f != layout.SUBSTRATE),
)
_HASH_LIMIT = 16 << 20


def _signature(path: Path) -> str:
    try:
        if path.is_symlink():
            return "link:" + os.readlink(path)
        if not path.exists():
            return "absent"
        if path.stat().st_size > _HASH_LIMIT:
            return f"big:{path.stat().st_size}:{path.stat().st_mtime_ns}"
        return hashlib.sha1(path.read_bytes()).hexdigest()
    except OSError:
        return "unreadable"


def tree_snapshot(repo_root: Path) -> dict[str, str] | None:
    """{changed path: content signature} from git status; None when root is no git work tree."""
    proc = subprocess.run(["git", "-C", str(repo_root), "status", "--porcelain", "-z", "--untracked-files=all"],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        return None
    out: dict[str, str] = {}
    entries = iter(proc.stdout.split("\0"))
    for entry in entries:
        if len(entry) < 4:
            continue
        paths = [entry[3:]]
        if entry[0] in "RC" or entry[1] in "RC":
            paths.append(next(entries, ""))  # the rename's origin
        for rel in filter(None, paths):
            out[rel] = _signature(Path(repo_root) / rel)
    return out


def _published_since(started_at: float, *roots: Path) -> set[str]:
    """The prims files primitive_publish recorded at or after started_at."""
    files: set[str] = set()
    for root in roots:
        try:
            lines = (root / PROVENANCE_REL).read_text(encoding="utf-8").splitlines()
        except OSError:
            continue
        for line in lines:
            try:
                rec = json.loads(line)
            except ValueError:
                continue
            if float(rec.get("ts") or 0) >= started_at:
                files.update(str(f) for f in rec.get("files") or [])
    return files


def shared_tree_run(repo_root: Path) -> bool:
    """A concurrent pack worker on the primary checkout: other workers write the same tree."""
    return bool(os.environ.get("ACCEL_PIPELINE_PACK_QUERIES")) and primary_root(repo_root) == Path(repo_root).resolve()


class StageWriteCheck:
    """Snapshot the tree before an agent stage; ``violations()`` after it lists what the stage changed
    against the write policy: (path, rule it broke).  Exempt: what the publish tools wrote during the
    stage, artifact and pipeline-owned dirs, and (shared-tree packs, where attribution is impossible)
    other queries' directories."""

    def __init__(self, repo_root: Path, *, query: int, stage: str, pipeline_config: Any | None = None,
                 benchmark: str = "tpch") -> None:
        self.repo_root = Path(repo_root)
        self.query, self.stage, self.cfg, self.benchmark = int(query), stage, pipeline_config, benchmark
        self.started_at = time.time()
        self.before = tree_snapshot(self.repo_root)

    def violations(self) -> list[tuple[str, str]]:
        after = tree_snapshot(self.repo_root) if self.before is not None else None
        if after is None:
            return []
        changed = sorted(r for r, sig in after.items() if self.before.get(r) != sig)
        prims = _published_since(self.started_at, primary_root(self.repo_root), self.repo_root)
        shared = shared_tree_run(self.repo_root)
        write_paths = list(getattr(self.cfg, "write_paths", None) or [])
        bench = str(getattr(self.cfg, "benchmark_name", "") or self.benchmark)
        out: list[tuple[str, str]] = []
        for rel in changed:
            if rel.startswith(_UNCHECKED_PREFIXES) or os.path.basename(rel).startswith(".sandbox_probe_"):
                continue
            lay = layout.classify(rel)
            if rel in prims:
                continue
            if shared and lay.kind == "query" and (lay.query != self.query or lay.bench != bench):
                continue
            ok, why = write_allowed(self.repo_root, rel, query=self.query, stage=self.stage,
                                    write_paths=write_paths, pipeline_config=self.cfg, benchmark=self.benchmark)
            if not ok:
                out.append((rel, why))
        return out


def violation_message(stage: str, violations: list[tuple[str, str]]) -> str:
    lines = "\n".join(f"  - {rel}: {why}" for rel, why in violations[:40])
    more = f"\n  ... and {len(violations) - 40} more" if len(violations) > 40 else ""
    return (f"stage {stage} wrote paths the write policy forbids; undo them (restore the files with git, "
            f"delete new ones) and keep to the files you own:\n{lines}{more}")
