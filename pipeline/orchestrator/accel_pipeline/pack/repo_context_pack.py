"""Shared read-only repo context pack for concurrent workers.

Built once per pack (or on demand) and referenced from worker seeds so agents
do not re-explore registration surfaces / file layout every attempt.
"""

from __future__ import annotations

import hashlib
import json
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


PACK_REL = Path("pipeline/artifacts/shared/repo_context_pack.md")
PACK_META_REL = Path("pipeline/artifacts/shared/repo_context_pack.meta.json")


def pack_path(repo_root: Path) -> Path:
    return Path(repo_root) / PACK_REL


def pack_meta_path(repo_root: Path) -> Path:
    return Path(repo_root) / PACK_META_REL


def _list_aux_modules(repo_root: Path, benchmark: str = "tpch") -> list[str]:
    paths = sorted(repo_root.glob(f"accelerators/{benchmark}/q*/*/aux/module.cpp"))
    return [str(p.relative_to(repo_root)) for p in paths]


def build_repo_context_pack_markdown(repo_root: Path) -> str:
    root = Path(repo_root)
    tpch = _list_aux_modules(root, "tpch")
    lines = [
        "# Shared repo context pack (read-only)",
        "",
        f"Generated: {datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M:%SZ')}",
        "",
        "Workers should **not** re-bootstrap this map every attempt. Use it as the",
        "canonical layout / registration guide. Editable work stays in **your** `qN/aux/**`.",
        "",
        "## Ownership rules (enforced)",
        "",
        "- **Writable:** only your query directory `accelerators/<bench>/q{N}/**` (aux module,",
        "  registration, tests/, bench/, config/ cost model). `substrate/**` and `workloads/**`",
        "  are read-only; `backends/<engine>/**` belongs to the engine_backend run;",
        "  `accelerators/prims/**` changes only via `primitive_publish`.",
        "- **Registration lives in the query's own directory** (registrars); the engine backend",
        "  names no query. Shared files (backend, arg variants, cross-accel helpers, CMake) are",
        "  frozen during optimization. Do not edit them mid-opt.",
        "- **Cross-query writes are denied.** Read peers (search with `rg`) only.",
        "",
        "## Registration surfaces (discover via peer search; do not invent partial wiring)",
        "",
        "| Surface | Where | Failure if missing |",
        "|---------|-------|--------------------|",
        "| Registrars and their headers | `backends/<engine>/api/README.md` (`## Contract roles` table) | compile / link errors |",
        "| The query's registration | `accelerators/<bench>/q{N}/<engine>/register.cpp` (peer queries show the pattern) | `unsupported shared query` / function does not exist |",
        "| Shared test and bench helpers | `eval/tests/test_support/`, `eval/bench/` (read peers) | multi-conn / writer SQL fail |",
        "",
        "## TPC-H aux module index (read-only references)",
        "",
    ]
    if not tpch:
        lines.append("(none found)")
    else:
        for rel in tpch:
            # accelerators/<bench>/q12/<engine>/aux/module.cpp
            parts = Path(rel).parts
            q = next((p for p in parts if p.startswith("q") and p[1:].isdigit()), "?")
            lines.append(f"- `{rel}` ({q})")
    lines.extend(
        [
            "",
            "## Profiling / optimization loop (token-efficient)",
            "",
            "1. Measure with MCP `run_profile` / digests on the **current** owned tree.",
            "2. Patch only owned editable files (`module.hpp/cpp`, `finalize.cpp`, tests, cost model).",
            "3. Yield `CANDIDATE_READY` only after local build+smoke for **your** query.",
            "4. On `unsupported shared query` or foreign-file compile errors: **stop** —",
            "   orchestrator fail-fast repairs the tree; do not burn more turns.",
            "",
            "## Search examples",
            "",
            "```",
            "rg -n -C5 <Symbol> accelerators/<bench>/q<N>/<engine>/aux",
            "```",
            "",
        ]
    )
    return "\n".join(lines)


def ensure_repo_context_pack(
    repo_root: Path,
    *,
    force: bool = False,
) -> Path:
    """Write/refresh the shared pack; return path to the markdown file."""
    root = Path(repo_root)
    out = pack_path(root)
    meta = pack_meta_path(root)
    md = build_repo_context_pack_markdown(root)
    digest = hashlib.sha256(md.encode("utf-8")).hexdigest()[:16]
    if out.is_file() and meta.is_file() and not force:
        try:
            prev = json.loads(meta.read_text(encoding="utf-8"))
            if prev.get("sha256_16") == digest:
                return out
        except (json.JSONDecodeError, OSError):
            pass
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(md, encoding="utf-8")
    meta.write_text(
        json.dumps(
            {
                "sha256_16": digest,
                "path": str(PACK_REL),
                "bytes": len(md.encode("utf-8")),
                "generated_at": datetime.now(timezone.utc).isoformat(),
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    return out


def format_seed_pointer(repo_root: Path, *, max_preview_lines: int = 40) -> str:
    """Short seed block pointing at the shared pack (avoids re-inlining full map)."""
    path = ensure_repo_context_pack(repo_root)
    rel = PACK_REL.as_posix()
    preview = path.read_text(encoding="utf-8").splitlines()[:max_preview_lines]
    meta = {}
    mp = pack_meta_path(repo_root)
    if mp.is_file():
        try:
            meta = json.loads(mp.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            meta = {}
    digest = meta.get("sha256_16", "?")
    return "\n".join(
        [
            "## Shared repo context pack (do not re-explore layout)",
            "",
            f"- path: `{rel}` (sha={digest})",
            "- Full ownership / registration / aux index lives there. Prefer that file over",
            "  rediscovering CMake/catalog/helpers each attempt.",
            "",
            "### Preview",
            "```",
            *preview,
            "```",
            "",
        ]
    )
