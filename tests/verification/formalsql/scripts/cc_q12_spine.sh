#!/usr/bin/env bash
# Fast iteration: compile only Q12JoinSpine.v (deps already .vo).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
LOGOS="${LOGOS_ROOT:-$ROOT/third_party/Logos}"
FSQL="$ROOT/tests/verification/formalsql"
SWITCH="${ROCQ_OPAM_SWITCH:-$LOGOS/.opam-rocq}"
if [[ -d "$SWITCH/_opam" || -L "$SWITCH/_opam" ]]; then _prefix="$SWITCH/_opam"; else _prefix="$(opam var prefix --switch="$SWITCH")"; fi
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
cd "$FSQL/q12"
opam exec --switch="$SWITCH" -- rocq compile "${FLAGS[@]}" "${1:-Q12JoinSpine.v}"
echo "CC OK: ${1:-Q12JoinSpine.v}"
