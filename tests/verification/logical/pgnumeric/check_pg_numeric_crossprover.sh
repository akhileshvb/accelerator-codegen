#!/usr/bin/env bash
# Cross-prover check of the PostgreSQL avg(numeric) closed form:
#   Rocq  formalsql/lib/NumericAggExact.v  pg_avg_scale / pg_avg_coeff   (vm_compute)
#   Dafny logical/dafny/PgNumeric.dfy       DivScale / AvgOfScaledSum     (compiled to Python)
# on a deterministic grid of (sum, s, n): signs, zero, ties, tiny/huge
# magnitudes, scales 0..6 and base-10000 weight boundaries. Any difference
# fails. (Rocq side is proved equal to FormalSQL's numeric_div_at_scales by
# NumericAggExact.pg_avg_exact; the C++ helper is checked against both and
# against PostgreSQL by the Q1 extract-diff and live tests.)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
LOGOS="$ROOT/third_party/Logos"; FSQL="$ROOT/tests/verification/formalsql"
SWITCH="${ROCQ_OPAM_SWITCH:-$LOGOS/.opam-rocq}"
DAFNY="${DAFNY_BIN:-$ROOT/tools/dafny/dafny}"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
_prefix="$SWITCH/_opam"; export ROCQLIB="$_prefix/lib/coq" COQLIB="$_prefix/lib/coq" OCAMLFIND_CONF="$_prefix/lib/findlib.conf"

python3 - "$WORK/cases.txt" <<'PY'
import sys, random
rng = random.Random(20260929)
cases = set()
for s in range(0, 7):
    for n in (1, 2, 3, 7, 9, 10, 99, 100, 9999, 10000, 10001, 123456, 1000000007):
        for sum_ in (0, 1, -1, 5, -5, 9999, 10000, 10001, 12345, -12345, 99999999, 10**15, -(10**15)+7):
            cases.add((sum_, s, n))
while len(cases) < 1500:
    s = rng.randrange(0, 7)
    n = rng.choice([rng.randrange(1, 50), rng.randrange(1, 10**6), rng.randrange(1, 10**12)])
    sum_ = rng.choice([rng.randrange(-10**6, 10**6), rng.randrange(-10**18, 10**18), n * rng.randrange(-50, 50) // 2])
    cases.add((sum_, s, n))
with open(sys.argv[1], "w") as f:
    for c in sorted(cases):
        f.write("%d %d %d\n" % c)
PY

# Rocq: vm_compute the closed form on every case.
{
  echo 'From Stdlib Require Import ZArith List. Import ListNotations. Open Scope Z_scope.'
  echo 'From FormalSQLLib Require Import NumericAggExact.'
  echo 'Definition cases : list (Z * Z * Z) := ['
  awk 'NR>1{printf ";\n"} {printf "(%s, %s, %s)", ($1<0?"("$1")":$1), $2, $3}' "$WORK/cases.txt"
  echo '].'
  echo 'Definition out := map (fun c : Z * Z * Z => let (p, n) := c in let (sum, s) := p in (pg_avg_coeff sum s n, pg_avg_scale sum s n)) cases.'
  echo 'Eval vm_compute in out.'
} > "$WORK/PgCheck.v"
cd "$FSQL"
opam exec --switch="$SWITCH" -- rocq compile -Q "$LOGOS/vendor/FormalSQL/src" SQLFS -Q "$LOGOS/theories" Logos \
  -Q "$FSQL/lib" FormalSQLLib "$WORK/PgCheck.v" > "$WORK/rocq.out" 2>&1 || { cat "$WORK/rocq.out"; exit 1; }

# Dafny: compile PgNumeric to Python.
"$DAFNY" build --target py --no-verify --output "$WORK/pgnum" "$ROOT/tests/verification/logical/dafny/PgNumeric.dfy" > /dev/null

python3 - "$WORK" <<'PY'
import re, sys
work = sys.argv[1]
sys.path.insert(0, work + "/pgnum-py")
import PgNumeric as P
D = P.default__
cases = [tuple(map(int, l.split())) for l in open(work + "/cases.txt")]
text = open(work + "/rocq.out").read()
pairs = re.findall(r"\(\s*(-?\d+)\s*,\s*(-?\d+)\s*\)", text.replace("\n", " "))
if len(pairs) != len(cases):
    sys.exit("check_pg_numeric_crossprover FAIL: parsed %d Rocq results for %d cases" % (len(pairs), len(cases)))
bad = 0
for (sum_, s, n), (rc, rs) in zip(cases, pairs):
    d = D.AvgOfScaledSum(sum_, s, n)
    if (int(d.coeff), int(d.scale)) != (int(rc), int(rs)):
        bad += 1
        if bad <= 5:
            print("MISMATCH sum=%d s=%d n=%d rocq=(%s,%s) dafny=(%d,%d)" % (sum_, s, n, rc, rs, d.coeff, d.scale))
if bad:
    sys.exit("check_pg_numeric_crossprover FAIL: %d/%d mismatches" % (bad, len(cases)))
print("PG_NUMERIC_CROSSPROVER_OK (%d cases: Rocq pg_avg_* == Dafny PgNumeric)" % len(cases))
PY
