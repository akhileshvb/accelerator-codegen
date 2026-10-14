#!/usr/bin/env python3
"""Check the include direction between the repository's layers.

    substrate/   includes nothing from backends/ or accelerators/
    backends/    includes nothing from accelerators/ (prims included)

Accelerators may include anything below them; eval/ is not checked.
substrate/ or backends/ including eval/ (the cross-query evaluation harness) is
reported as a warning (an error with --strict).

Includes are resolved the way the build does: relative to the including file,
then against the include roots (the repo root and accelerators/).  The build
also exposes each engine's catalog as "catalog/..." (CMake links
backends/<engine>/api/catalog/ into the build tree), so "catalog/..." counts
as a backend include.  An include that resolves to no file in the tree is
classified by its leading path segment (backends/, substrate/, prims/,
prooflib/, <bench>/ of accelerators/); anything else (system, DuckDB, the
DBMS SDKs) is ignored.

usage: scripts/check_layer_includes.py [--json] [--strict] [--root DIR] [--file PATH ...]
Exit 1 if any include points the wrong way.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SUFFIXES = {".cpp", ".cc", ".c", ".hpp", ".h", ".hh", ".inc", ".ipp"}
INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]')

# layer -> layers it must not include
FORBIDDEN = {
    "substrate": ("backends", "accelerators"),
    "backends": ("accelerators",),
}
# layer -> layers it should not include (warnings)
DISCOURAGED = {
    "substrate": ("eval",),
    "backends": ("eval",),
}


def layer_of_rel(rel: str) -> str | None:
    top = rel.split("/", 1)[0]
    if top in ("substrate", "backends", "accelerators", "eval", "workloads"):
        return top
    return None


def _accel_tops(root: Path) -> set[str]:
    acc = root / "accelerators"
    return {p.name for p in acc.iterdir() if p.is_dir()} if acc.is_dir() else set()


def resolve(root: Path, including: Path, target: str, accel_tops: set[str]) -> tuple[str | None, str]:
    """(layer, how) of an include target; layer None when it is outside the tree."""
    for base, how in ((including.parent, "relative"), (root, "repo root"), (root / "accelerators", "accelerators/")):
        cand = (base / target)
        if cand.is_file():
            try:
                rel = str(cand.resolve().relative_to(root.resolve()))
            except ValueError:
                return None, how
            return layer_of_rel(rel), f"{how}: {rel}"
    first = target.split("/", 1)[0]
    if first == "catalog":
        return "backends", "catalog/ (backends/<engine>/api/catalog/)"
    if first in ("substrate", "backends", "accelerators"):
        return first, "by prefix (missing file)"
    if first in accel_tops:
        return "accelerators", "accelerators/ by prefix (missing file)"
    return None, "external"


def scan_file(root: Path, path: Path, accel_tops: set[str], layer: str | None = None) -> list[dict]:
    try:
        rel = str(path.resolve().relative_to(root.resolve()))
    except ValueError:
        rel = str(path)
    layer = layer or layer_of_rel(rel)
    banned = FORBIDDEN.get(layer or "", ())
    warn = DISCOURAGED.get(layer or "", ())
    if not banned and not warn:
        return []
    hits = []
    for no, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        m = INCLUDE.match(line)
        if not m:
            continue
        target_layer, how = resolve(root, path, m.group(2), accel_tops)
        if target_layer in banned or target_layer in warn:
            hits.append({"file": rel, "line": no, "layer": layer, "include": m.group(2),
                         "target_layer": target_layer, "resolved": how,
                         "severity": "error" if target_layer in banned else "warning"})
    return hits


def scan(root: Path = ROOT) -> tuple[list[dict], int]:
    accel_tops = _accel_tops(root)
    hits: list[dict] = []
    n = 0
    for top in FORBIDDEN:
        base = root / top
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.is_file() and path.suffix in SUFFIXES:
                n += 1
                hits += scan_file(root, path, accel_tops, top)
    return hits, n


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--strict", action="store_true", help="warnings (includes of eval/) fail too")
    ap.add_argument("--root", type=Path, default=ROOT)
    ap.add_argument("--file", action="append", type=Path,
                    help="check only these files (layer from their path under --root)")
    args = ap.parse_args()
    root = args.root.resolve()
    if args.file:
        tops = _accel_tops(root)
        hits = [h for p in args.file for h in scan_file(root, p.resolve(), tops)]
        n = len(args.file)
    else:
        hits, n = scan(root)
    errors = [h for h in hits if h["severity"] == "error" or args.strict]
    warnings = [h for h in hits if h not in errors]
    if args.json:
        print(json.dumps({"status": "fail" if errors else "pass", "files_scanned": n, "hits": errors,
                          "warnings": warnings}, indent=2))
    else:
        for h in hits:
            print(f"{h['file']}:{h['line']}: {h['severity']}: {h['layer']} includes {h['target_layer']}: "
                  f"\"{h['include']}\" ({h['resolved']})")
        print(f"{len(errors)} wrong-way include(s), {len(warnings)} warning(s) in {n} files" if errors
              else f"include direction ok ({n} files in substrate/ and backends/; {len(warnings)} warning(s))")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
