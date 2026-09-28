#!/usr/bin/env bash
# Compile Q1 QueryResultSpec normalize gate (Phase 1 PoC).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
LOGOS="${LOGOS_ROOT:-$ROOT/third_party/Logos}"
FSQL="$ROOT/tests/verification/formalsql"
SWITCH="${ROCQ_OPAM_SWITCH:-$LOGOS/.opam-rocq}"

die() { echo "compile_q1_resultspec FAIL: $*" >&2; exit 1; }

command -v opam >/dev/null || die "opam not found"
[[ -f "$FSQL/q1/generated/Queries.v" ]] || die "missing Queries.v"

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
compile_one lib/QuerySpineFacade.v
compile_one generated/Q1AuxSpec.v
compile_one resultspec/QueryResultSpec.v
compile_one resultspec/LogicalSpec.v
compile_one resultspec/NormalizeQuery.v
compile_one resultspec/NormalizeCompositional.v
compile_one resultspec/EvalSpec.v
compile_one resultspec/EvalLogicalSpec.v
compile_one resultspec/NormalizeSoundGeneric.v
# NormalizeSound_Q1: see scripts/compile_q1_normalize_sound.sh (NORMALIZE_SOUND_OK).
cd "$FSQL/q1"
compile_one generated/Schema.v
compile_one generated/Queries.v
compile_one generated/Witness.v
compile_one Q1Catalog.v
compile_one Q1FormalSQLProof.v
compile_one Q1ResultSpecNormalize.v
compile_one Q1LogicalNormalize.v

echo "compile_q1_resultspec OK (RESULTSPEC_NORMALIZE_OK; NORMALIZE_SOUND_OK via compile_q1_normalize_sound.sh)"
