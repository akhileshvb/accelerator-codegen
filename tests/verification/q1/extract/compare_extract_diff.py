#!/usr/bin/env python3
"""Compare Dafny-extract dump vs C++ dump.

Integer lines must match exactly. Lines starting with 'AVG ' compare a
Dafny rational n/d to a C++ IEEE double (not bit-identity).
"""
from __future__ import annotations

import math
import sys


def load(path: str) -> tuple[list[str], list[str]]:
    ints: list[str] = []
    avgs: list[str] = []
    with open(path, encoding="utf-8") as f:
        for raw in f:
            line = raw.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            if line.startswith("AVG "):
                avgs.append(line)
            else:
                ints.append(line)
    return ints, avgs


def parse_avg(line: str) -> tuple[str, float]:
    parts = line.split(" ", 2)
    if len(parts) < 3:
        raise ValueError(f"bad AVG line: {line!r}")
    tag, val = parts[1], parts[2]
    if "/" in val:
        n, d = val.split("/", 1)
        return tag, float(n) / float(d)
    return tag, float(val)


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: compare_extract_diff.py dafny.out cpp.out", file=sys.stderr)
        return 2
    dafny_ints, dafny_avgs = load(sys.argv[1])
    cpp_ints, cpp_avgs = load(sys.argv[2])
    ok = True
    if dafny_ints != cpp_ints:
        ok = False
        print("integer-line mismatch", file=sys.stderr)
        n = max(len(dafny_ints), len(cpp_ints))
        for i in range(n):
            a = dafny_ints[i] if i < len(dafny_ints) else "<missing>"
            b = cpp_ints[i] if i < len(cpp_ints) else "<missing>"
            if a != b:
                print(f"  [{i}] dafny: {a}", file=sys.stderr)
                print(f"  [{i}] cpp:   {b}", file=sys.stderr)
    if len(dafny_avgs) != len(cpp_avgs):
        ok = False
        print(
            f"AVG count mismatch dafny={len(dafny_avgs)} cpp={len(cpp_avgs)}",
            file=sys.stderr,
        )
    for i, (da, ca) in enumerate(zip(dafny_avgs, cpp_avgs)):
        try:
            t0, v0 = parse_avg(da)
            t1, v1 = parse_avg(ca)
        except ValueError as exc:
            ok = False
            print(exc, file=sys.stderr)
            continue
        if t0 != t1:
            ok = False
            print(f"AVG tag mismatch {t0!r} vs {t1!r}", file=sys.stderr)
            continue
        if not math.isclose(v0, v1, rel_tol=1e-12, abs_tol=1e-9):
            ok = False
            print(
                f"AVG {t0} mismatch dafny={v0} cpp={v1} ({da!r} vs {ca!r})",
                file=sys.stderr,
            )
    if not ok:
        return 1
    print(
        f"PASS extract-diff ({len(dafny_ints)} integer lines, "
        f"{len(dafny_avgs)} avg lines)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
