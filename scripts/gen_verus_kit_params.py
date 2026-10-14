#!/usr/bin/env python3
"""Parameter tuples of the TPC-H Verus kit test workloads
(accelerators/tpch/q<N>/tests/verus_workload/params.sql), from one source.

Every TPC-H substitution parameter is a runtime parameter of the kit function
accel_tpch_qN_vgen(...) (its arity is the highest $n of the kit's query text,
in its glue.cpp).  A params.sql has the shape the Verus pipeline's tests stage
freezes (pipeline/orchestrator/accel_pipeline/verus/actions.py:add_random_params):

  1. curated tuples      the tests stage's adversarial tuples (kept as they are);
  2. edge tuples         (this script) edge cases of a parameter that replaced a
                         literal after the tests were written (see FORMER_LITERAL);
  3. RANDOM_PARAMS_MARK  then SETS random sets drawn from the TPC-H domain
                         (spec 2.4 / qgen, eval/compare/gen_tpch_params.py, the
                         generator the pipeline itself calls), with a fixed seed.

Sections 2 and 3 are regenerated on every run (reproducible: same seed, same
file).  Curated tuples written before a literal became a parameter have one value
too few: they get the former literal inserted at its position (that is the
question they asked), once; the file then stays as it is.  Any other arity
mismatch is an error.

    python3 scripts/gen_verus_kit_params.py            # rewrite all 22
    python3 scripts/gen_verus_kit_params.py --check    # exit 1 if any file is stale or invalid
    python3 scripts/gen_verus_kit_params.py --queries 2,9 [--sets 8] [--seed 20261014]

It also writes workloads/tpch/schema_constraints.sql (the runner's ACCEL_VERUS_SCHEMA_SQL):
the NOT NULL columns of workloads/tpch/schema.sql as ALTER statements, by the rule of the
pipeline's schema_constraints() (actions.py), which writes the same file per run.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GENERATOR = ROOT / "eval/compare/gen_tpch_params.py"
# exactly the pipeline's marker (actions.py), so its add_random_params sees the block as present
RANDOM_PARAMS_MARK = "-- random parameter sets drawn from the domain when the tests were frozen"
EDGE_MARK = "-- edge cases of a parameter that was a literal when the tests were written (scripts/gen_verus_kit_params.py)"
SEED = 20261014
SETS = 8  # as the pipeline's gates.frozen_param_sets

# Parameters that were literals in the query when its tests were written:
#   q -> (0-based position in the kit's argument list, the former literal, edge values of the new parameter)
# The edge values are combined with the first curated tuple's other values.
FORMER_LITERAL: dict[int, tuple[int, str, list[str]]] = {
    # p_type LIKE $2 ('%' || TYPE3); the kernel applies any ASCII pattern at read time
    2: (1, "'%BRASS'", ["'%TIN'", "'%'", "''", "'%brass'", "'%BRASS%'", "'%_RASS'", "'STANDARD%'",
                        "'ECONOMY ANODIZED BRASS'"]),
    # p_name LIKE $1 ('%' || color || '%'); the kernel tracks the 92 TPC-H colors, others decline
    9: (0, "'%green%'", ["'%red%'", "'%almond%'", "'%yellow%'", "'%'", "'%GREEN%'", "'green'", "'%zzzz%'"]),
    # HAVING ... > SUM(...) * $2 (0.0001 / SF); the kit takes the fraction at scale <= 9, else declines
    11: (1, "0.0001", ["0.00001", "0.001", "0.01", "0.0", "1.0", "0.000000001", "0.0000000001"]),
    # o_comment NOT LIKE $1 ('%' || word1 || '%' || word2 || '%'); the kernel tracks the 16 TPC-H patterns
    13: (0, "'%unusual%accounts%'", ["'%special%requests%'", "'%pending%deposits%'", "'%express%packages%'",
                                     "'%accounts%unusual%'", "'%unusual%'", "'%'"]),
    # p_type NOT LIKE $2 (TYPE1 || ' ' || TYPE2 || '%')
    16: (1, "'SMALL BRUSHED%'", ["'MEDIUM POLISHED%'", "'PROMO%'", "'%'", "''", "'small brushed%'",
                                 "'%BRUSHED%'", "'STANDARD PLATED TIN'"]),
    # p_name LIKE $2 (color || '%'); a 3-byte literal prefix takes the kernel's indexed path, else a full scan
    20: (1, "'gainsboro%'", ["'forest%'", "'a%'", "'%'", "''", "'gainsboro'", "'%gainsboro%'", "'GAINSBORO%'",
                             "'ga_nsboro%'"]),
}


SCHEMA_SQL = ROOT / "workloads/tpch/schema.sql"
SCHEMA_CONSTRAINTS = ROOT / "workloads/tpch/schema_constraints.sql"
_NOT_NULL = re.compile(r"^\s*(\w+)\s+.*\bNOT\s+NULL\b", re.I)


def schema_constraints() -> str:
    """The declared schema's NOT NULL columns as ALTER statements (the pipeline's rule, actions.py)."""
    stmts, table = [], None
    for line in SCHEMA_SQL.read_text(encoding="utf-8").splitlines():
        m = re.match(r"\s*CREATE\s+TABLE\s+(\w+)", line, re.I)
        if m:
            table = m.group(1)
            continue
        if table and line.strip().startswith(")"):
            table = None
            continue
        c = _NOT_NULL.match(line)
        if table and c and c.group(1).upper() not in ("PRIMARY", "CONSTRAINT", "UNIQUE", "FOREIGN", "CHECK"):
            stmts.append(f"ALTER TABLE {table} ALTER COLUMN {c.group(1)} SET NOT NULL;")
    return ("-- NOT NULL columns of workloads/tpch/schema.sql, applied by verus_kit_runner (ACCEL_VERUS_SCHEMA_SQL)\n"
            "-- before the tests' setup.sql. Generated: scripts/gen_verus_kit_params.py\n" + "\n".join(stmts) + "\n")


def split_tuple(t: str) -> list[str]:
    """Top-level comma-separated literals of a tuple (quotes and parentheses respected)."""
    out, depth, cur, i = [], 0, "", 0
    while i < len(t):
        c = t[i]
        if c == "'":
            j = i + 1
            while j < len(t):
                if t[j] == "'" and t[j + 1:j + 2] == "'":
                    j += 2
                    continue
                if t[j] == "'":
                    break
                j += 1
            cur += t[i:j + 1]
            i = j + 1
            continue
        if c in "([":
            depth += 1
        elif c in ")]":
            depth -= 1
        if c == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += c
        i += 1
    if cur.strip():
        out.append(cur.strip())
    return out


def is_tuple(line: str) -> bool:
    s = line.strip()
    return bool(s) and not s.startswith("--")


def kit_arity(q: int) -> int:
    """The highest $n of the kit's query text (its glue's kQuerySql)."""
    glue = (ROOT / f"accelerators/tpch/q{q}/verus_kit/tpch_q{q}_vgen/glue.cpp").read_text(encoding="utf-8")
    m = re.search(r"kQuerySql(?:\[\])?\s*=([^;]*);", glue)
    if not m:
        raise SystemExit(f"q{q}: no kQuerySql in glue.cpp")
    return max((int(n) for n in re.findall(r"\$(\d+)", m.group(1))), default=0)


def domain_sets(sets: int, seed: int) -> dict[int, list[str]]:
    """The Verus argument lists of `sets` random draws per query (as the pipeline calls the generator)."""
    proc = subprocess.run([sys.executable, "-I", str(GENERATOR), str(sets), str(seed)], capture_output=True,
                          text=True, check=True)
    qs = json.loads(proc.stdout)["queries"]
    return {int(q): [s["verus"] for s in v["sets"]] for q, v in qs.items()}


def render(q: int, text: str, drawn: list[str], sets: int, seed: int) -> tuple[str, list[str]]:
    """The new params.sql text, and the problems found."""
    n = kit_arity(q)
    problems: list[str] = []
    lines = text.splitlines()
    for mark in (EDGE_MARK, RANDOM_PARAMS_MARK):  # sections 2 and 3 are regenerated
        if mark in lines:
            lines = lines[:lines.index(mark)]
    while lines and not lines[-1].strip():
        lines.pop()
    former = FORMER_LITERAL.get(q)
    curated = []
    for i, line in enumerate(lines):
        if not is_tuple(line):
            continue
        vals = split_tuple(line.strip())
        if former and len(vals) == n - 1:  # written when $k was a literal: ask the same question
            pos, lit, _ = former
            vals.insert(pos, lit)
            lines[i] = ", ".join(vals)
        if len(vals) != n:
            problems.append(f"q{q}: curated tuple {line.strip()!r} has {len(vals)} values, the kit takes {n}")
        curated.append(vals)
    if former and n == 1 and not curated:  # parameter-free tests: the former literal was the only question
        lines.append(former[1])
        curated.append([former[1]])
    out = list(lines)
    if former:
        pos, lit, edges = former
        base = curated[0] if curated else [lit]
        out.append(EDGE_MARK)
        out.append(f"-- ${pos + 1} was the literal {lit}; its edge values, the other parameters as in the first tuple")
        for e in edges:
            vals = list(base)
            vals[pos] = e
            out.append(", ".join(vals))
    out.append(RANDOM_PARAMS_MARK)
    out.append(f"-- {len(drawn)} sets, seed {seed} (eval/compare/gen_tpch_params.py {sets} {seed}; "
               "regenerate: scripts/gen_verus_kit_params.py)")
    for t in drawn:
        if len(split_tuple(t)) != n:
            problems.append(f"q{q}: domain tuple {t!r} has {len(split_tuple(t))} values, the kit takes {n}")
        out.append(t)
    return "\n".join(out) + "\n", problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="only check: exit 1 if a file is stale or invalid")
    ap.add_argument("--queries", default=",".join(map(str, range(1, 23))))
    ap.add_argument("--sets", type=int, default=SETS)
    ap.add_argument("--seed", type=int, default=SEED)
    a = ap.parse_args()
    drawn = domain_sets(a.sets, a.seed)
    bad = 0
    sc = schema_constraints()
    old_sc = SCHEMA_CONSTRAINTS.read_text(encoding="utf-8") if SCHEMA_CONSTRAINTS.is_file() else ""
    if sc != old_sc:
        if a.check:
            print(f"{SCHEMA_CONSTRAINTS.relative_to(ROOT)} is stale (run scripts/gen_verus_kit_params.py)")
            bad += 1
        else:
            SCHEMA_CONSTRAINTS.write_text(sc, encoding="utf-8")
            print(f"{SCHEMA_CONSTRAINTS.relative_to(ROOT)} (updated)")
    for q in (int(x) for x in a.queries.split(",") if x):
        path = ROOT / f"accelerators/tpch/q{q}/tests/verus_workload/params.sql"
        old = path.read_text(encoding="utf-8") if path.is_file() else ""
        new, problems = render(q, old, drawn.get(q, []), a.sets, a.seed)
        for p in problems:
            print("ERROR", p, file=sys.stderr)
        bad += bool(problems)
        n_tuples = sum(is_tuple(l) for l in new.splitlines())
        if a.check:
            if new != old:
                print(f"q{q}: {path.relative_to(ROOT)} is stale (run scripts/gen_verus_kit_params.py)")
                bad += 1
            continue
        if new != old and not problems:
            path.write_text(new, encoding="utf-8")
        print(f"q{q}: arity {kit_arity(q)}, {n_tuples} tuples{' (updated)' if new != old and not problems else ''}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
