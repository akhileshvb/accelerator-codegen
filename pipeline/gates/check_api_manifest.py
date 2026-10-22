#!/usr/bin/env python3
"""Check a backend's api/README.md manifest against the headers of its api/.

usage: check_api_manifest.py [repo_root] [engine ...]   (default: every backends/*/backend.cmake)

The format is fixed by pipeline/prompts/engine_backend/host_contract.md and the roles by
api_contract.md: a table under `## Contract roles` with one row per role (role | headers |
symbols); empty headers and symbols declare a role n/a. Every listed header exists, every
.hpp under api/ is listed, and every symbol occurs in one of its row's headers (role
`introspection`: in any file under api/, since SQL function names live where they are defined).
Prints JSON; exit 1 on any problem.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROLES = ("registration", "runtime", "scan", "sql_fallback", "cost_model", "maintenance",
         "introspection", "thread_knobs", "value_semantics")
ANYWHERE_ROLES = {"introspection"}
SOURCE_SUFFIXES = {".hpp", ".cpp", ".h", ".sql"}


def parse_roles(text: str) -> dict[str, tuple[list[str], list[str]]] | None:
    lines = text.splitlines()
    try:
        start = next(i for i, line in enumerate(lines) if line.strip() == "## Contract roles")
    except StopIteration:
        return None
    rows: dict[str, tuple[list[str], list[str]]] = {}
    in_table = False
    for line in lines[start + 1:]:
        if not line.strip().startswith("|"):
            if in_table:
                break
            continue
        in_table = True
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) != 3 or cells[0] in ("role",) or set(cells[0]) <= {"-", " "}:
            continue
        ticks = lambda cell: re.findall(r"`([^`]+)`", cell)
        rows[cells[0]] = (ticks(cells[1]), ticks(cells[2]))
    return rows


def check_engine(root: Path, engine: str) -> list[str]:
    api = root / "backends" / engine / "api"
    manifest = api / "README.md"
    if not manifest.is_file():
        return [f"{engine}: missing {manifest.relative_to(root)}"]
    rows = parse_roles(manifest.read_text(encoding="utf-8"))
    if rows is None:
        return [f"{engine}: api/README.md has no '## Contract roles' table"]
    problems = []
    for role in ROLES:
        if role not in rows:
            problems.append(f"{engine}: role '{role}' has no row")
    for role in rows:
        if role not in ROLES:
            problems.append(f"{engine}: unknown role '{role}'")
    listed: set[str] = set()
    api_text = {p: p.read_text(encoding="utf-8", errors="replace")
                for p in api.rglob("*") if p.is_file() and p.suffix in SOURCE_SUFFIXES}
    for role, (headers, symbols) in rows.items():
        if bool(headers) != bool(symbols) and not (role in ANYWHERE_ROLES and symbols):
            problems.append(f"{engine}: role '{role}' lists headers without symbols or the reverse")
        texts = []
        for header in headers:
            rel = header[4:] if header.startswith("api/") else header
            path = api / rel
            if not path.is_file():
                problems.append(f"{engine}: role '{role}': header {header} does not exist")
                continue
            listed.add(rel)
            texts.append(api_text.get(path, ""))
        pool = list(api_text.values()) if role in ANYWHERE_ROLES else texts
        for symbol in symbols:
            if not any(re.search(rf"(?<![\w]){re.escape(symbol)}(?![\w])", t) for t in pool):
                problems.append(f"{engine}: role '{role}': symbol {symbol} not found")
    for path in sorted(api.rglob("*.hpp")):
        rel = str(path.relative_to(api))
        if rel not in listed:
            problems.append(f"{engine}: api/{rel} is in no role's headers")
    return problems


def main() -> int:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    engines = sys.argv[2:] or sorted(p.parent.name for p in (root / "backends").glob("*/backend.cmake"))
    problems = [p for e in engines for p in check_engine(root, e)]
    print(json.dumps({"gate": "api_manifest", "engines": engines,
                      "status": "fail" if problems else "pass", "problems": problems}, indent=2))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
