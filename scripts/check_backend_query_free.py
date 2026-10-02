#!/usr/bin/env python3
"""Check that the engine backends and the substrate name no query.

Queries register themselves from src/generated/<bench>/q<N>/ (static
registrars against the backends' registries), so a backend file that mentions
a specific query (TpchQ6..., sec_edgar_q2, "tpch/q21/...", accel_tpch_q1_aux)
is per-query code in the wrong place.  Comments are ignored.

usage: scripts/check_backend_query_free.py [--engine duckdb|postgres|mysql] [--json]
Exit 1 if any backend file names a query.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

COMMON = [
    "include/accelerator_codegen/stateful/**/*",
    "include/accelerator_codegen/runtime/catalog/**/*",
    "include/accelerator_codegen/verus/verus_kit.hpp",
    "src/stateful/core/**/*",
    "src/stateful/state/**/*",
    "src/stateful/maintenance/**/*",
    "src/verus_kit/**/*",
    "src/runtime/catalog/**/*",
]
ENGINE = {
    "duckdb": [
        "include/accelerator_codegen/runtime/duckdb/**/*",
        "include/accelerator_codegen/stateful/duckdb/**/*",
        "src/runtime/duckdb/**/*",
        "src/stateful/duckdb/**/*",
        "src/generated/catalog/duckdb/**/*",
    ],
    "postgres": [
        "include/accelerator_codegen/runtime/postgres/**/*",
        "include/accelerator_codegen/stateful/postgres/**/*",
        "src/runtime/postgres/**/*",
        "src/stateful/postgres/**/*",
        "src/generated/postgres/**/*",
        "src/generated/catalog/postgres/**/*",
    ],
    "mysql": [
        "include/accelerator_codegen/runtime/mysql/**/*",
        "include/accelerator_codegen/stateful/mysql/**/*",
        "src/runtime/mysql/**/*",
        "src/stateful/mysql/**/*",
        "src/generated/mysql/**/*",
        "src/generated/catalog/mysql/**/*",
    ],
}
SUFFIXES = {".cpp", ".hpp", ".h", ".cc", ".sql"}

QUERY = re.compile(
    r"\b(?:Tpch|SecEdgar)Q\d+"            # TpchQ6Module, SecEdgarQ2Layout
    r"|\b(?:tpch|sec_edgar)_q\d+"          # tpch_q21_verus, accel_tpch_q1_aux
    r"|\b(?:tpch|sec_edgar)/q\d+/"         # "tpch/q21/verus/module.hpp"
    r"|\bq\d+v_|\bq\d+vs_"                 # a Verus kernel's C ABI (q21v_..., q21vs_...)
)
COMMENT_LINE = re.compile(r"^\s*(?://|/\*|\*|--|#(?!include|if|ifdef|ifndef|define|endif|else|elif))")


def strip_comment(line: str, sql: bool) -> str:
    if COMMENT_LINE.match(line):
        return ""
    cut = line.find("--" if sql else "//")
    return line if cut < 0 else line[:cut]


def scan(engines: list[str]) -> list[dict]:
    patterns = list(COMMON)
    for engine in engines:
        patterns += ENGINE[engine]
    seen: set[Path] = set()
    hits: list[dict] = []
    for pat in patterns:
        for path in sorted(ROOT.glob(pat)):
            if path in seen or not path.is_file() or path.suffix not in SUFFIXES:
                continue
            seen.add(path)
            sql = path.suffix == ".sql"
            for no, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
                m = QUERY.search(strip_comment(line, sql))
                if m:
                    hits.append({"file": str(path.relative_to(ROOT)), "line": no, "match": m.group(0),
                                 "text": line.strip()[:160]})
    return hits


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--engine", action="append", choices=sorted(ENGINE))
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()
    engines = args.engine or sorted(ENGINE)
    hits = scan(engines)
    if args.json:
        print(json.dumps({"status": "fail" if hits else "pass", "engines": engines, "hits": hits[:200]}, indent=2))
    else:
        for h in hits:
            print(f"{h['file']}:{h['line']}: {h['match']}: {h['text']}")
        print(f"{len(hits)} query reference(s) in {', '.join(engines)} backend files" if hits
              else f"backends name no query ({', '.join(engines)})")
    return 1 if hits else 0


if __name__ == "__main__":
    sys.exit(main())
