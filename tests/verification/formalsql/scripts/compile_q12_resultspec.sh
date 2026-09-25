#!/usr/bin/env bash
# Compile Q12 ResultSpec normalize gate (join TFGOP peel → S_Q12_catalog).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
LOGOS="${LOGOS_ROOT:-$ROOT/third_party/Logos}"
FSQL="$ROOT/tests/verification/formalsql"
SWITCH="${ROCQ_OPAM_SWITCH:-$LOGOS/.opam-rocq}"

die() { echo "compile_q12_resultspec FAIL: $*" >&2; exit 1; }

command -v opam >/dev/null || die "opam not found"
[[ -f "$FSQL/q12/generated/Queries.v" ]] || die "missing q12 Queries.v — run scripts/logos_lower_catalog.sh q12"

if [[ -d "$SWITCH/_opam" || -L "$SWITCH/_opam" ]]; then
  _prefix="$SWITCH/_opam"
else
  _prefix="$(opam var prefix --switch="$SWITCH")"
fi
export ROCQLIB="${ROCQLIB:-$_prefix/lib/coq}"
export COQLIB="${COQLIB:-$ROCQLIB}"
export OCAMLFIND_CONF="${OCAMLFIND_CONF:-$_prefix/lib/findlib.conf}"

FLAGS=(
  -Q "$LOGOS/vendor/FormalSQL/src" SQLFS
  -Q "$LOGOS/theories" Logos
  -Q "$FSQL/lib" FormalSQLLib
  -Q "$FSQL/resultspec" FormalSQLResultSpec
  -Q "$FSQL/q12/generated" LogosGenerated
)

compile_one() {
  local f="$1"
  echo "rocq compile $f"
  opam exec --switch="$SWITCH" -- rocq compile "${FLAGS[@]}" "$f"
}

cd "$FSQL"
compile_one resultspec/QueryResultSpecQ12.v
compile_one resultspec/NormalizeQueryQ12.v
cd "$FSQL/q12"
compile_one generated/Schema.v
compile_one generated/Queries.v
compile_one Q12Catalog.v
compile_one Q12ResultSpecNormalize.v

echo "compile_q12_resultspec OK (RESULTSPEC_NORMALIZE_OK)"
