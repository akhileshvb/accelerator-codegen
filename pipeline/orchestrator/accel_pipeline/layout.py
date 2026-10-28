"""The repository's layers, who may write each, and the check of an agent stage's writes.

=============================  ==========================================================
``substrate/**``               never writable by agents (workload- and engine-agnostic)
``backends/<engine>/**``       owned by that engine's ``engine_backend`` run (state/,
                               engine/, api/ incl. api/catalog/, tests/, bench/); the
                               ``host_bootstrap`` stage writes only its workload harnesses
                               (``harness_globs``), nothing else in it and no other backend
``accelerators/<bench>/q<N>/**``  owned by that query's run (kernels, aux, registration,
                               tests, bench, cost-model config, verus_kit); other queries'
                               directories are never writable, and its ``tests/workload/``
                               (the test suite) is written only by the test_suite stage
``accelerators/prims/**``      changed only through ``primitive_publish`` (agents stage
                               candidates under ``pipeline/prims_staging/``)
``accelerators/prooflib/**``   changed only through ``prooflib_publish``
``workloads/**``               read-only to agents (query SQL, schemas)
=============================  ==========================================================

Everything else (``accelerators/<bench>/{common,verus_world}``, ``eval/**``, ``cmake/**``,
``CMakeLists.txt``, ``pipeline/**``) falls through to the shared-path lease and the stage's configured
``write_paths``; shared registration paths are frozen during optimization stages so concurrent workers
cannot desync wiring. The pipeline's own files (gates, prompts, contracts, the orchestrator, the
substrate) are restored after every agent stage; a change that matches the tree's committed version is kept,
which is how a fix to the pipeline reaches a running tree (commit it there). ``StageWriteCheck`` lists what a
stage wrote against all of this.
"""

from __future__ import annotations

import fnmatch
import hashlib
import json
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Sequence


# ---- the layers -------------------------------------------------------------------------------

SUBSTRATE = "substrate"
BACKENDS = "backends"
ACCELERATORS = "accelerators"
PRIMS = f"{ACCELERATORS}/prims"
PROOFLIB = f"{ACCELERATORS}/prooflib"
WORKLOADS = "workloads"
# Directories directly under accelerators/ that are not benchmarks.
_NON_BENCH_ACCEL_DIRS = frozenset({"prims", "prooflib"})
_QUERY_DIR_RE = re.compile(r"^accelerators/([A-Za-z0-9_]+)/q(\d+)(?:/|$)")
_BACKEND_RE = re.compile(r"^backends/([A-Za-z0-9_]+)(?:/|$)")


def backend_glob(engine: str) -> str:
    return f"{BACKENDS}/{engine}/**"


def harness_globs(engine: str) -> list[str]:
    """The workload harnesses of one backend: the only part of it host_bootstrap writes."""
    return [
        f"{BACKENDS}/{engine}/tests/harness/**",
        f"{BACKENDS}/{engine}/bench/**",
        f"{BACKENDS}/{engine}/tests/harness.cmake",
    ]


def query_dir(bench: str, query: int | str) -> str:
    return f"{ACCELERATORS}/{bench}/q{query}"


# ---- who owns a failure ------------------------------------------------------------------------

QUERY, HARNESS, BACKEND, DATA, PIPELINE, INFRA = OWNERS = ("query", "harness", "backend", "data", "pipeline", "infra")
UNKNOWN_TARGET = re.compile(r"unknown target '([A-Za-z0-9_]+)'|No rule to make target '([A-Za-z0-9_]+)'")
_NAMED_PATH = re.compile(r"(?:backends|accelerators)/[^\s:()'\"]+")


def path_owner(rel: str, query: str) -> str:
    """Who owns a file a failure names: the query (its directory `query`), a peer query's shared tree (infra), the
    workload harnesses (host_bootstrap), the rest of a backend; "" for any other file."""
    layer = classify(rel)
    if layer.kind == "query":
        return QUERY if _under(_norm(rel), query) else INFRA
    if layer.kind == "backend":
        return HARNESS if any(fnmatch.fnmatch(_norm(rel), g) for g in harness_globs(layer.engine)) else BACKEND
    return ""


def build_error_owner(log: str, query: str, harness_targets: Sequence[str] = ()) -> str:
    """Who a failed build is the fault of, by its first error that names a harness target host_bootstrap should have
    made or a file in a layer with an owner (path_owner); the query's by default."""
    for line in log.splitlines():
        if any(t in harness_targets for m in UNKNOWN_TARGET.finditer(line) for t in m.groups() if t):
            return HARNESS
        named = _NAMED_PATH.search(line)
        if named and re.search(r"error|undefined reference", line, re.I) and (owner := path_owner(named.group(), query)):
            return owner
    return QUERY


def query_glob(bench: str, query: int | str) -> str:
    return f"{ACCELERATORS}/{bench}/q{query}/**"


def workload_query_sql(bench: str, query: int | str) -> str:
    return f"{WORKLOADS}/{bench}/q{query}.sql"


def workload_schema_sql(bench: str) -> str:
    return f"{WORKLOADS}/{bench}/schema.sql"


def workload_schema_constraints(bench: str) -> str:
    return f"{WORKLOADS}/{bench}/schema_constraints.sql"


def suite_dir(bench: str, query: int | str) -> str:
    """The query's test suite: written by the test_suite stage, then frozen (accel_pipeline.data)."""
    return f"{ACCELERATORS}/{bench}/q{query}/tests/workload"


_SUITE_RE = re.compile(r"^accelerators/[A-Za-z0-9_]+/q\d+/tests/workload(?:/|$)")


def is_suite_path(rel: str) -> bool:
    """Inside a query's test suite (frozen after the test_suite stage)."""
    return bool(_SUITE_RE.match(rel.replace("\\", "/")))


@dataclass(frozen=True)
class Layer:
    """Which layer a repo-relative path belongs to."""

    kind: str  # substrate | backend | prims | prooflib | query | accel_shared | workloads | other
    engine: str = ""
    bench: str = ""
    query: int | None = None


def _norm(rel: str) -> str:
    rel = str(rel).replace("\\", "/")
    while rel.startswith("./"):
        rel = rel[2:]
    return rel.lstrip("/")


def _under(rel: str, top: str) -> bool:
    return rel == top or rel.startswith(top + "/")


def classify(rel: str) -> Layer:
    rel = _norm(rel)
    if _under(rel, SUBSTRATE):
        return Layer("substrate")
    if _under(rel, WORKLOADS):
        return Layer("workloads")
    if _under(rel, PRIMS):
        return Layer("prims")
    if _under(rel, PROOFLIB):
        return Layer("prooflib")
    m = _BACKEND_RE.match(rel)
    if m:
        return Layer("backend", engine=m.group(1))
    m = _QUERY_DIR_RE.match(rel)
    if m:
        return Layer("query", bench=m.group(1), query=int(m.group(2)))
    if _under(rel, ACCELERATORS):
        parts = rel.split("/")
        bench = parts[1] if len(parts) > 1 and parts[1] not in _NON_BENCH_ACCEL_DIRS else ""
        return Layer("accel_shared", bench=bench)
    return Layer("other")


# ---- who may write what -----------------------------------------------------------------------

# Paths that need the shared-file lease under concurrent pack workers.
DEFAULT_SHARED_PATH_GLOBS = (
    "backends/*/api/**",  # engine backends: generic only, queries register from their own directories
    "backends/*/engine/**",
    "backends/*/state/**",
    "eval/tests/test_support/**",
    "eval/bench/**/*_arg_variants.*",
    "accelerators/prims/**",  # changed only through primitive_publish
    "accelerators/*/common/**",  # helpers shared by one benchmark's queries
)


SUBSTRATE_DENY = (
    "substrate/** is never writable by agents (the shared, workload- and engine-agnostic "
    "substrate); record what you need in your report instead"
)


WORKLOADS_DENY = "workloads/** (query SQL and schemas) is read-only to agents"


PROOFLIB_DENY = (
    "accelerators/prooflib/** changes only through the prooflib_publish tool "
    "(edit the workspace copy, then publish)"
)

STAGING_REL = "pipeline/prims_staging"  # where agents stage the candidates primitive_publish takes
PROVENANCE_REL = "pipeline/prims_provenance.jsonl"  # what primitive_publish published, when
PRIMS_DENY = (
    "the primitives library is shared by every worker and is changed only through the "
    "primitive_publish tool: stage the candidate file(s) under "
    f"{STAGING_REL}/<repo-relative path> (new header, full updated README.md, full updated "
    "test_prims.cpp) and call primitive_publish(files, rationale). Additions only, generic only."
)


def is_staging_path(rel: str) -> bool:
    return rel.replace("\\", "/").lstrip("./").startswith(STAGING_REL + "/")


def primary_root(repo_root: Path) -> Path:
    """The checkout the shared libraries (prims, prooflib) live in: a pack worker's worktree names it."""
    env = os.environ.get("ACCEL_PIPELINE_PRIMARY_REPO")
    return Path(env).resolve() if env else Path(repo_root).resolve()


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


def is_optimization_stage(stage: str | None) -> bool:
    return (stage or "") in _OPTIMIZATION_STAGES


def owned_path_globs(query: int, *, benchmark: str = "tpch", engine: str = "") -> list[str]:
    """What one query x engine run owns: the query directory (one glob) + its artifact tree."""
    q = int(query)
    artifacts = f"pipeline/artifacts/{benchmark}/q{q}" + (f"/{engine}" if engine else "")
    return [query_glob(benchmark, q), f"{artifacts}/**"]


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


def _host_bootstrap_verdict(rel: str, lay: Layer, engine_name: str) -> tuple[bool, str]:
    if lay.kind == "backend" and lay.engine == engine_name and any(
        fnmatch.fnmatch(rel, g) for g in harness_globs(engine_name)
    ):
        return True, "engine-harness"
    return False, (
        f"host_bootstrap write denied: {rel}. It writes only the {engine_name or '<engine>'} "
        f"workload harnesses ({', '.join(harness_globs(engine_name or '<engine>'))}); "
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
    lay = classify(rel)
    engine_run = workload_kind == "engine_backend"
    if stage == "host_bootstrap" and lay.kind in {"backend", "query"}:
        return _host_bootstrap_verdict(rel, lay, engine_name)
    if lay.kind == "substrate":
        return False, f"direct write denied: {rel}. {SUBSTRATE_DENY}"
    if lay.kind == "workloads":
        return False, f"direct write denied: {rel}. {WORKLOADS_DENY}"
    if lay.kind == "prims":
        return False, f"direct write denied: {rel}. {PRIMS_DENY}"
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
        if is_suite_path(rel) != (stage == "test_suite"):
            return False, (
                f"write denied: {rel}. The test suite of a query is written by the test_suite stage and frozen "
                "after it; to change a test, write a test_suite_change_request in the artifacts instead"
                if is_suite_path(rel) else
                f"write denied: {rel}. The test_suite stage writes only the query's tests/workload directory"
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


# ---- the pipeline's own files -----------------------------------------------------------------

FROZEN = (
    "substrate",
    "cmake",
    "CMakeLists.txt",
    "pipeline/codex",
    "pipeline/config",
    "pipeline/gates",
    "pipeline/orchestrator",
    "pipeline/prompts",
    "pipeline/schemas",
    "pipeline/templates",
)


_SKIP_PARTS = {"__pycache__", ".venv", ".pytest_cache"}


def _files(repo_root: Path) -> list[Path]:
    out: list[Path] = []
    for rel in FROZEN:
        p = repo_root / rel
        if p.is_file():
            out.append(p)
        elif p.is_dir():
            out.extend(
                f for f in p.rglob("*")
                if f.is_file() and not _SKIP_PARTS.intersection(f.relative_to(repo_root).parts)
            )
    return out


def frozen_edited(repo_root: Path) -> list[str]:
    """Frozen paths that differ from the tree's committed version right now (empty when git cannot say)."""
    present = [rel for rel in FROZEN if (repo_root / rel).exists()]
    proc = subprocess.run(["git", "-C", str(repo_root), "status", "--porcelain", "--untracked-files=all", "--", *present],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        return []
    paths = (line[3:] for line in proc.stdout.splitlines())
    return [p for p in paths if not _SKIP_PARTS.intersection(Path(p).parts)]


def frozen_snapshot(repo_root: Path) -> dict[Path, bytes]:
    return {f: f.read_bytes() for f in _files(repo_root)}


def _committed(f: Path) -> bytes | None:
    """The file's content at HEAD of the repository that holds it (a worker worktree links some
    pipeline directories to the primary tree, whose commits are the ones that count)."""
    real = f.resolve()
    top = subprocess.run(["git", "-C", str(real.parent), "rev-parse", "--show-toplevel"],
                         capture_output=True, text=True)
    if top.returncode != 0:
        return None
    rel = real.relative_to(Path(top.stdout.strip())).as_posix()
    proc = subprocess.run(["git", "-C", top.stdout.strip(), "show", f"HEAD:{rel}"], capture_output=True)
    return proc.stdout if proc.returncode == 0 else None


def frozen_restore(repo_root: Path, before: dict[Path, bytes]) -> list[str]:
    """Undo every change to the frozen paths since `before` that is not the committed
    version; return the paths that were reverted."""
    changed: list[str] = []
    now = set(_files(repo_root))
    for f in sorted(now - before.keys()):
        if _committed(f) == f.read_bytes():
            continue
        f.unlink()
        changed.append(f"added {f.relative_to(repo_root)}")
    for f, data in before.items():
        if f in now and f.read_bytes() == data:
            continue
        if f in now and _committed(f) == f.read_bytes():
            continue
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_bytes(data)
        changed.append(f"{'edited' if f in now else 'deleted'} {f.relative_to(repo_root)}")
    return changed


# ---- the stage write check --------------------------------------------------------------------

_HASH_LIMIT = 16 << 20


def file_signature(path: Path) -> str:
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
            out[rel] = file_signature(Path(repo_root) / rel)
    return out


# Not the agents' to write by this policy but handled elsewhere (artifacts; frozen paths are restored by
# frozen_restore; the codex home and run dirs are the pipeline's own).
_UNCHECKED_PREFIXES = (
    "pipeline/artifacts/", "pipeline/.codex/", "pipeline/verus_runs/", "pipeline/worktrees/",
    "pipeline/.locks/", PROVENANCE_REL,
    *(f if "." in f else f + "/" for f in FROZEN if f != SUBSTRATE),
)


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
        # what revert() restores: the content of every file already changed when the stage began
        self.saved = {rel: (self.repo_root / rel).read_bytes() for rel, sig in (self.before or {}).items()
                      if sig not in ("absent", "unreadable") and not sig.startswith(("link:", "big:"))}

    def _runtime_prefixes(self) -> tuple[str, ...]:
        """Engine state and build dirs inside the tree: written by builds and gates, not by the agent."""
        dirs = (os.environ.get("ACCEL_ENGINE_STATE_DIR"), getattr(self.cfg, "build_dir", None))
        out = []
        for d in dirs:
            if not d:
                continue
            path = Path(d) if Path(d).is_absolute() else self.repo_root / d
            try:
                out.append(str(path.resolve().relative_to(self.repo_root.resolve())).rstrip("/") + "/")
            except ValueError:
                continue
        return tuple(out)

    def violations(self) -> list[tuple[str, str]]:
        after = tree_snapshot(self.repo_root) if self.before is not None else None
        if after is None:
            return []
        changed = sorted(r for r, sig in after.items() if self.before.get(r) != sig)
        prims = _published_since(self.started_at, primary_root(self.repo_root), self.repo_root)
        shared = shared_tree_run(self.repo_root)
        write_paths = list(getattr(self.cfg, "write_paths", None) or [])
        bench = str(getattr(self.cfg, "benchmark_name", "") or self.benchmark)
        runtime = self._runtime_prefixes()
        out: list[tuple[str, str]] = []
        for rel in changed:
            if rel.startswith(_UNCHECKED_PREFIXES + runtime) or os.path.basename(rel).startswith(".sandbox_probe_"):
                continue
            lay = classify(rel)
            if rel in prims:
                continue
            if shared and lay.kind == "query" and (lay.query != self.query or lay.bench != bench):
                continue
            ok, why = write_allowed(self.repo_root, rel, query=self.query, stage=self.stage,
                                    write_paths=write_paths, pipeline_config=self.cfg, benchmark=self.benchmark)
            if not ok:
                out.append((rel, why))
        return out


    def revert(self, violations: list[tuple[str, str]]) -> None:
        """Put each violating path back as it was when the stage began."""
        for rel, _ in violations:
            path = self.repo_root / rel
            if rel in self.saved:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(self.saved[rel])
            elif (self.before or {}).get(rel) == "absent" or subprocess.run(
                    ["git", "-C", str(self.repo_root), "checkout", "-q", "HEAD", "--", rel], capture_output=True).returncode:
                path.unlink(missing_ok=True)  # new in this stage (or deleted before it): it goes


def violation_message(stage: str, violations: list[tuple[str, str]], *, reverted: bool = False) -> str:
    lines = "\n".join(f"  - {rel}: {why}" for rel, why in violations[:40])
    more = f"\n  ... and {len(violations) - 40} more" if len(violations) > 40 else ""
    undo = ("they were put back as they were when the stage began; do the work within the files you own"
            if reverted else "undo them (restore the files with git, delete new ones) and keep to the files you own")
    return f"stage {stage} wrote paths the write policy forbids; {undo}:\n{lines}{more}"


# ---- the code stamp ----------------------------------------------------------------------------
# One digest of the source state a build or a gate result depends on: the checked-out commit and the content of
# every uncommitted file, except what cannot change this query's build (pipeline code, scripts, docs, build output
# and the other queries' directories). Gate scripts use it to skip a build that already succeeded
# (`python3 -m accel_pipeline.layout stamp <repo_root> [--query-dir DIR] [EXTRA ...]`); the repair and gate-plan code
# uses it to reuse a gate result while the stamp still matches.

IRRELEVANT_TOP_DIRS = frozenset({"pipeline", "scripts", "docs"})
_BUILD_OR_VCS = re.compile(r"^(\.git|build([-._/]|$))")  # build dirs and their lock files, at the repo root
_QUERY_DIR = re.compile(r"^accelerators/[A-Za-z0-9_]+/q\d+/")


def git(repo_root: Path, *args: str) -> bytes:
    return subprocess.run(["git", *args], cwd=repo_root, capture_output=True, check=True).stdout


def is_relevant(path: str, query_dir: str) -> bool:
    """Whether `path` can change this query's build or behaviour: everything but pipeline code, scripts,
    docs, build output and the other queries' directories (accelerators, prims, backends, substrate,
    cmake and the shared accelerators/<bench>/common are all relevant)."""
    if path.split("/", 1)[0] in IRRELEVANT_TOP_DIRS or _BUILD_OR_VCS.match(path):
        return False
    if _QUERY_DIR.match(path) and "/scratch/" in path:  # an agent's throwaway probes: no gate judges them
        return False
    return not (query_dir and _QUERY_DIR.match(path) and not path.startswith(query_dir.rstrip("/") + "/"))


def code_stamp(repo_root: Path, *, query_dir: str = "", extra: Sequence[str] = ()) -> str:
    """Digest of HEAD plus the relevant uncommitted files for `query_dir` (repo-relative), mixed with `extra`.
    "" when the tree is not a git checkout: no stamp matches anything."""
    try:
        digest = hashlib.sha256(git(repo_root, "rev-parse", "HEAD"))
    except (OSError, subprocess.CalledProcessError):
        return ""
    for path, signature in sorted((tree_snapshot(repo_root) or {}).items()):
        if is_relevant(path, query_dir):
            digest.update(f"\0{path}\0{signature}".encode())
    for item in extra:
        digest.update(f"\0{item}".encode())
    return digest.hexdigest()


def main(argv: list[str]) -> int:
    """The `stamp` and `build-owner` commands the gate scripts run."""
    if argv[:1] == ["build-owner"] and len(argv) >= 2:  # <query dir> [harness target ...], the build log on stdin
        print(build_error_owner(sys.stdin.read(), argv[1], argv[2:]))
        return 0
    if argv[:1] != ["stamp"] or len(argv) < 2:
        print("usage: python3 -m accel_pipeline.layout stamp <repo_root> [--query-dir DIR] [EXTRA ...]\n"
              "       python3 -m accel_pipeline.layout build-owner <query dir> [harness target ...] < build log", file=sys.stderr)
        return 2
    args, query_dir = argv[1:], ""
    if "--query-dir" in args:
        at = args.index("--query-dir")
        query_dir = args[at + 1]
        del args[at : at + 2]
    print(code_stamp(Path(args[0]), query_dir=query_dir, extra=args[1:]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
