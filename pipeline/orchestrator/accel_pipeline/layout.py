"""The repository's layers and the one write glob each layer gets.

Layers (see docs/codebase-layout-proposal.md, phase 3):

=============================  ==========================================================
``substrate/**``               never writable by agents (workload- and engine-agnostic)
``backends/<engine>/**``       owned by that engine's ``engine_backend`` run (state/,
                               engine/, api/ incl. api/catalog/, tests/, bench/)
``accelerators/<bench>/q<N>/**``  owned by that query's run (kernels, aux, registration,
                               tests, bench, cost-model config, verus_kit)
``accelerators/prims/**``      changed only through ``primitive_publish``
``accelerators/prooflib/**``   changed only through ``prooflib_publish``
``workloads/**``               read-only to agents (query SQL, schemas)
=============================  ==========================================================

Everything else (``accelerators/<bench>/{common,verus_world}``, ``eval/**``,
``cmake/**``, ``CMakeLists.txt``, ``pipeline/**``) falls through to the shared-path
lease and the stage's configured ``write_paths``.
"""

from __future__ import annotations

import re
from dataclasses import dataclass

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


def query_glob(bench: str, query: int | str) -> str:
    return f"{ACCELERATORS}/{bench}/q{query}/**"


def workload_query_sql(bench: str, query: int | str) -> str:
    return f"{WORKLOADS}/{bench}/q{query}.sql"


def workload_schema_sql(bench: str) -> str:
    return f"{WORKLOADS}/{bench}/schema.sql"


def workload_schema_constraints(bench: str) -> str:
    return f"{WORKLOADS}/{bench}/schema_constraints.sql"


def suite_dir(bench: str, query: int | str) -> str:
    """The query's test suite: written by the test_suite stage, then frozen (accel_pipeline.test_suite)."""
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
