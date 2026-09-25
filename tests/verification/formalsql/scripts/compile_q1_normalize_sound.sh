#!/usr/bin/env bash
# NORMALIZE_SOUND_OK — compile NormalizeSound_Q1 atop existing AuxCorrect .vo.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
LOGOS="${LOGOS_ROOT:-$ROOT/third_party/Logos}"
FSQL="$ROOT/tests/verification/formalsql"
SWITCH="${ROCQ_OPAM_SWITCH:-$LOGOS/.opam-rocq}"

die() { echo "compile_q1_normalize_sound FAIL: $*" >&2; exit 1; }

command -v opam >/dev/null || die "opam not found"
[[ -f "$FSQL/q1/Q1TCBAuxCorrect.vo" ]] || die "missing Q1TCBAuxCorrect.vo — run compile_q1_proof.sh first"

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
  -Q "$FSQL/q1/generated" LogosGenerated
  -Q "$FSQL/generated" JetStreamFS
)

compile_one() {
  local f="$1"
  echo "rocq compile $f"
  opam exec --switch="$SWITCH" -- rocq compile "${FLAGS[@]}" "$f"
}

cd "$FSQL"
compile_one resultspec/QueryResultSpec.v
compile_one resultspec/NormalizeQuery.v
compile_one resultspec/EvalSpec.v
cd "$FSQL/q1"
compile_one Q1ResultSpecNormalize.v
compile_one Q1NormalizeSound.v

echo "compile_q1_normalize_sound OK (NORMALIZE_SOUND_OK)"
