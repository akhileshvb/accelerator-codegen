#!/usr/bin/env python3
"""Materialize sf1_instances SQL from tpch_placeholders + C++ manifests.

Keeps SQL aligned with accelerators/tpch/q*/tests/q*_manifest.hpp (accelerator bind args)
while using the real placeholder templates (--placeholders).

Usage:
  python3 scripts/materialize_tpch_sf1_from_manifests.py --placeholders <dir>
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFESTS = ROOT / "accelerators" / "tpch"
DEFAULT_OUT = ROOT / "workloads" / "tpch" / "instances"


def _parse_entry_args(arg_blob: str) -> list:
    tokens: list = []
    i = 0
    s = arg_blob.strip()
    while i < len(s):
        while i < len(s) and s[i] in " \t\n\r,":
            i += 1
        if i >= len(s):
            break
        if s[i] == '"':
            j = i + 1
            out: list[str] = []
            while j < len(s):
                if s[j] == "\\" and j + 1 < len(s):
                    out.append(s[j + 1])
                    j += 2
                    continue
                if s[j] == '"':
                    break
                out.append(s[j])
                j += 1
            tokens.append("".join(out))
            i = j + 1
            continue
        if s[i] == "{":
            depth = 1
            j = i + 1
            while j < len(s) and depth:
                if s[j] == "{":
                    depth += 1
                elif s[j] == "}":
                    depth -= 1
                j += 1
            if depth != 0:
                raise ValueError(f"unbalanced '{{' in {s[i:i+60]!r}")
            inner = s[i + 1 : j - 1]
            if '"' in inner:
                tokens.append(re.findall(r'"([^"]*)"', inner))
            else:
                tokens.append([int(x.strip()) for x in inner.split(",") if x.strip()])
            i = j
            continue
        m = re.match(r"([+-]?(?:\d+\.\d+|\d+)(?:[eE][+-]?\d+)?)", s[i:])
        if not m:
            raise ValueError(f"cannot parse args at {s[i:i+40]!r}")
        num = m.group(1)
        tokens.append(float(num) if "." in num or "e" in num.lower() else int(num))
        i += len(num)
    return tokens


def parse_manifest(path: Path) -> list[tuple[int, list]]:
    text = path.read_text(encoding="utf-8")
    entries: list[tuple[int, list]] = []
    for m in re.finditer(r'\{\s*"seed_(\d+)_q\d+"\s*,\s*(?:ACCEL_TPCH_INSTANCES_DIR\s*)?"[^"]+"\s*,', text):
        seed = int(m.group(1))
        start = m.start()
        depth = 0
        i = start
        while i < len(text):
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
                if depth == 0:
                    i += 1
                    break
            i += 1
        after = text[start:i][m.end() - start : -1]
        entries.append((seed, _parse_entry_args(after)))
    return entries


def q(s: str) -> str:
    return f"'{s}'"


def date_lit(s: str) -> str:
    return f"DATE '{s}'"


def subs_for_query(query: int, args: list) -> dict[int, str]:
    """Map $N → replacement text matching QueryGen literal style."""
    if query == 1:
        return {1: str(int(args[0]))}
    if query == 2:
        return {1: str(int(args[0])), 2: q(str(args[1])), 3: q(str(args[2]))}
    if query == 3:
        return {1: q(str(args[0])), 2: date_lit(str(args[1]))}
    if query == 4:
        return {1: date_lit(str(args[0]))}
    if query == 5:
        return {1: q(str(args[0])), 2: date_lit(str(args[1]))}
    if query == 6:
        # Manifest stores discount as string like "0.03"
        return {1: date_lit(str(args[0])), 2: str(args[1]), 3: str(args[2])}
    if query == 7:
        return {1: q(str(args[0])), 2: q(str(args[1]))}
    if query == 8:
        return {1: q(str(args[0])), 2: q(str(args[1])), 3: q(str(args[2]))}
    if query == 9:
        # Manifest stores color stem; placeholder becomes '%color%'
        color = str(args[0]).strip("%")
        return {1: q(f"%{color}%")}
    if query == 10:
        return {1: date_lit(str(args[0]))}
    if query == 11:
        return {1: q(str(args[0])), 2: str(args[1])}
    if query == 12:
        return {1: q(str(args[0])), 2: q(str(args[1])), 3: date_lit(str(args[2]))}
    if query == 13:
        pat = str(args[0])
        return {1: q(pat) if not pat.startswith("'") else pat}
    if query == 14:
        return {1: date_lit(str(args[0]))}
    if query == 15:
        return {1: date_lit(str(args[0]))}
    if query == 16:
        brand, typ, sizes = str(args[0]), str(args[1]), list(args[2])
        out = {1: q(brand), 2: q(typ)}
        for i, sz in enumerate(sizes, start=3):
            out[i] = str(int(sz))
        return out
    if query == 17:
        return {1: q(str(args[0])), 2: q(str(args[1]))}
    if query == 18:
        return {1: str(int(args[0]))}
    if query == 19:
        return {
            1: q(str(args[0])),
            2: str(int(args[1])),
            3: q(str(args[2])),
            4: str(int(args[3])),
            5: q(str(args[4])),
            6: str(int(args[5])),
        }
    if query == 20:
        return {1: date_lit(str(args[0])), 2: q(str(args[1])), 3: q(str(args[2]))}
    if query == 21:
        return {1: q(str(args[0]))}
    if query == 22:
        codes = [str(x) for x in args[0]]
        return {i + 1: codes[i] for i in range(7)}
    raise ValueError(f"unsupported query {query}")


def apply_subs(template: str, subs: dict[int, str]) -> str:
    # Replace higher $N first so $10 is not clobbered by $1.
    out = template
    for n in sorted(subs.keys(), reverse=True):
        out = out.replace(f"${n}", subs[n])
    if re.search(r"\$\d+", out):
        left = sorted(set(re.findall(r"\$\d+", out)))
        raise ValueError(f"unreplaced placeholders: {left}")
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--placeholders", type=Path, required=True, help="directory of the TPC-H placeholder templates")
    ap.add_argument("--manifests", type=Path, default=DEFAULT_MANIFESTS)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = ap.parse_args()

    written = 0
    errors: list[str] = []
    for path in sorted(args.manifests.glob("q*/tests/q*_manifest.hpp")):
        if "nonzero" in path.name:
            continue
        m = re.search(r"q(\d+)_manifest", path.name)
        if not m:
            continue
        query = int(m.group(1))
        tmpl_path = args.placeholders / f"{query:02d}.sql"
        if not tmpl_path.is_file():
            errors.append(f"Q{query}: missing placeholder {tmpl_path}")
            continue
        template = tmpl_path.read_text(encoding="utf-8")
        try:
            entries = parse_manifest(path)
        except Exception as exc:  # noqa: BLE001
            errors.append(f"Q{query}: parse failed: {exc}")
            continue
        for seed, entry_args in entries:
            try:
                sql = apply_subs(template, subs_for_query(query, entry_args))
            except Exception as exc:  # noqa: BLE001
                errors.append(f"Q{query} seed_{seed}: {exc}")
                continue
            dest = args.out / f"seed_{seed}" / f"{query:02d}.sql"
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_text(sql, encoding="utf-8")
            written += 1

    print(f"wrote {written} sql files under {args.out}")
    if errors:
        print("ERRORS:", file=sys.stderr)
        for e in errors:
            print(f"  {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
