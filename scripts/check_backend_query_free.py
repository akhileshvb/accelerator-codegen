#!/usr/bin/env python3
"""Check that the engine backends and the substrate name no query.

Queries register themselves from accelerators/<bench>/q<N>/ (static
registrars against the backends' registries), so a backend file that mentions
a specific query (TpchQ6..., sec_edgar_q2, "tpch/q21/...", accel_tpch_q1_aux)
is per-query code in the wrong place.  Comments are ignored.

A second check keeps the shared primitives library
(accelerators/prims/) workload-agnostic: no identifier
derived from the benchmark schemas (table and column names parsed from the
schema SQL files below, plus tpch / sec_edgar / edgar) may appear there, comments
and README included.  A small allowlist (GENERIC_WORDS) exempts schema names that
are ordinary English/C++ words.

usage: scripts/check_backend_query_free.py [--engine duckdb|postgres|mysql] [--json]
                                           [--no-prims-vocab | --prims-only] [--prims-file PATH ...]
Exit 1 if any backend file names a query or a prims file uses workload vocabulary.
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
    "backends/duckdb/api/**/*",
    "substrate/verus/kit/verus_kit.hpp",
    "substrate/aux/core/**/*",
    "substrate/aux/state/**/*",
    "substrate/aux/maintenance/**/*",
    "substrate/verus/kit/**/*",
    "backends/duckdb/api/**/*",
]
ENGINE = {
    "duckdb": [
        "include/accelerator_codegen/runtime/duckdb/**/*",
        "include/accelerator_codegen/stateful/duckdb/**/*",
        "src/runtime/duckdb/**/*",
        "src/stateful/duckdb/**/*",
        "backends/duckdb/api/catalog/**/*",
    ],
    "postgres": [
        "backends/postgres/engine/**/*",
        "include/accelerator_codegen/stateful/postgres/**/*",
        "src/runtime/postgres/**/*",
        "src/stateful/postgres/**/*",
        "src/generated/postgres/**/*",
        "backends/postgres/api/catalog/**/*",
    ],
    "mysql": [
        "backends/mysql/engine/**/*",
        "include/accelerator_codegen/stateful/mysql/**/*",
        "backends/mysql/engine/**/*",
        "src/stateful/mysql/**/*",
        "backends/mysql/api/**/*",
        "backends/mysql/api/catalog/**/*",
    ],
}
SUFFIXES = {".cpp", ".hpp", ".h", ".cc", ".sql"}

QUERY = re.compile(
    r"\b(?:Tpch|SecEdgar)Q\d+"            # TpchQ6Module, SecEdgarQ2Layout
    r"|\b(?:tpch|sec_edgar)_q\d+"          # tpch_q21_verus, accel_tpch_q1_aux
    r"|\b(?:tpch|sec_edgar)/q\d+/"         # "tpch/q21/verus/module.hpp"
    r"|\bq\d+v_|\bq\d+vs_"                 # a Verus kernel's C ABI (q21v_..., q21vs_...)
)
# ---- workload vocabulary for the primitives library -------------------------
PRIMS_DIR = "accelerators/prims"
SCHEMA_FILES = [
    "scripts/postgres_tpch_schema.sql",
    "scripts/mysql_tpch_schema.sql",
    "workloads/tpch/schema.sql",
    "workloads/sec_edgar/schema.sql",
]
# Always denied (substring, case-insensitive), in addition to schema identifiers.
BANNED_SUBSTRINGS = ("tpch", "sec_edgar", "edgar")
# Schema identifiers that are ordinary words in generic data-structure code.
# They are not workload-specific on their own; every other schema name
# (lineitem, l_orderkey, adsh, cik, ...) stays denied.
GENERIC_WORDS = frozenset(
    {
        "name", "comment", "date", "key", "value", "count", "type", "size",
        "sub", "num", "pre", "tag", "part", "region", "nation", "orders",
        "customer", "supplier", "address", "phone", "version", "form", "period",
        "status", "label", "line", "order", "price", "mode", "flag", "quantity",
        "discount", "tax", "brand", "container", "number", "text", "unit",
        "instance", "report", "filed", "custom", "abstract", "rank", "uom",
        "accepted", "doc", "footnote",
    }
)
_CREATE_TABLE = re.compile(r"CREATE\s+TABLE\s+(?:IF\s+NOT\s+EXISTS\s+)?([A-Za-z_]\w*)\s*\(", re.I)
_NOT_COLUMN = {"primary", "foreign", "unique", "constraint", "check", "key", "index", "fulltext"}


def _split_top_level(body: str) -> list[str]:
    parts, depth, cur = [], 0, []
    for ch in body:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    parts.append("".join(cur))
    return parts


def parse_schema_names(sql: str) -> set[str]:
    """Table and column names declared by the CREATE TABLE statements in `sql`."""
    sql = re.sub(r"--[^\n]*", "", sql)
    names: set[str] = set()
    for m in _CREATE_TABLE.finditer(sql):
        names.add(m.group(1).lower())
        depth, i = 1, m.end()
        while i < len(sql) and depth:
            depth += {"(": 1, ")": -1}.get(sql[i], 0)
            i += 1
        for col in _split_top_level(sql[m.end(): i - 1]):
            toks = col.split()
            if toks:
                word = toks[0].strip('`"').lower()
                if word not in _NOT_COLUMN and re.fullmatch(r"[a-z_]\w*", word):
                    names.add(word)
    return names


def workload_vocabulary(root: Path = ROOT) -> set[str]:
    vocab: set[str] = set()
    for rel in SCHEMA_FILES:
        path = root / rel
        if path.is_file():
            vocab |= parse_schema_names(path.read_text(encoding="utf-8", errors="replace"))
    return vocab - GENERIC_WORDS


_WORD = re.compile(r"[A-Za-z_]\w*")


def scan_vocab_text(text: str, vocab: set[str]) -> list[tuple[int, str, str]]:
    """(line, match, text) for every workload word in `text` (comments included)."""
    hits = []
    for no, line in enumerate(text.splitlines(), 1):
        low = line.lower()
        found = [b for b in BANNED_SUBSTRINGS if b in low]
        found += [w for w in _WORD.findall(low) if w in vocab]
        if found:
            hits.append((no, found[0], line.strip()[:160]))
    return hits


def scan_prims(files: list[Path] | None = None, root: Path = ROOT) -> list[dict]:
    vocab = workload_vocabulary(root)
    if files is None:
        base = root / PRIMS_DIR
        files = sorted(p for p in base.rglob("*") if p.is_file()) if base.is_dir() else []
    hits = []
    for path in files:
        text = path.read_text(encoding="utf-8", errors="replace")
        try:
            label = str(path.relative_to(root))
        except ValueError:
            label = str(path)
        for no, match, line in scan_vocab_text(text, vocab):
            hits.append({"file": label, "line": no, "match": match, "text": line})
    return hits


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
    ap.add_argument("--no-prims-vocab", action="store_true", help="skip the prims/ workload-vocabulary check")
    ap.add_argument("--prims-only", action="store_true", help="only the prims/ workload-vocabulary check")
    ap.add_argument("--prims-file", action="append", type=Path,
                    help="check only these files for workload vocabulary (candidate prims files)")
    args = ap.parse_args()
    if args.prims_file:
        vhits = scan_prims([p.resolve() for p in args.prims_file])
        for h in vhits:
            print(f"{h['file']}:{h['line']}: workload word '{h['match']}': {h['text']}")
        print(f"{len(vhits)} workload word(s) in prims files" if vhits else "prims files are workload-agnostic")
        return 1 if vhits else 0
    if args.prims_only:
        vhits = scan_prims()
        for h in vhits:
            print(f"{h['file']}:{h['line']}: workload word '{h['match']}' in prims/: {h['text']}")
        print(f"{len(vhits)} workload word(s) in prims/" if vhits else "prims/ is workload-agnostic")
        return 1 if vhits else 0
    engines = args.engine or sorted(ENGINE)
    hits = scan(engines)
    vhits = [] if args.no_prims_vocab else scan_prims()
    if args.json:
        print(json.dumps({"status": "fail" if hits or vhits else "pass", "engines": engines, "hits": hits[:200],
                          "prims_vocab_hits": vhits[:200]}, indent=2))
    else:
        for h in hits:
            print(f"{h['file']}:{h['line']}: {h['match']}: {h['text']}")
        for h in vhits:
            print(f"{h['file']}:{h['line']}: workload word '{h['match']}' in prims/: {h['text']}")
        print(f"{len(hits)} query reference(s) in {', '.join(engines)} backend files" if hits
              else f"backends name no query ({', '.join(engines)})")
        if not args.no_prims_vocab:
            print(f"{len(vhits)} workload word(s) in prims/" if vhits else "prims/ is workload-agnostic")
    return 1 if hits or vhits else 0


if __name__ == "__main__":
    sys.exit(main())
