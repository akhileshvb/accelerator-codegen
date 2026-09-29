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
  -Q "$FSQL/logical" FormalSQLLogical
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
compile_one resultspec/LogicalSpec.v
compile_one resultspec/LogicalSpecSerialize.v
compile_one resultspec/NormalizeQuery.v
compile_one resultspec/NormalizeCompositional.v
compile_one resultspec/EvalSpec.v
compile_one resultspec/EvalLogicalSpec.v
compile_one resultspec/NormalizeSoundGeneric.v
# RelNode IR (semantic authority for public Eval_Spec)
compile_one logical/SqlTypes.v
compile_one logical/Schema.v
compile_one logical/Expr.v
compile_one logical/Agg.v
compile_one logical/RelNode.v
compile_one logical/Typing.v
compile_one logical/Eval.v
compile_one lib/PossibleOutcomeDeterminism.v
cd "$FSQL/q1"
compile_one Q1ResultSpecNormalize.v
compile_one Q1LogicalNormalize.v
compile_one Q1EvalMigration.v
compile_one Q1Schema.v
compile_one Q1LogicalSpecTree.v
compile_one Q1Adapt.v
compile_one Q1RelNodeBridge.v
compile_one Q1NormalizeSound.v
# Forall-outcomes strengthening (every FormalSQL outcome of q-hat is the same).
compile_one Q1AllOutcomes.v
PATTERN='Admitted|(^|[^[:alnum:]_])admit([^[:alnum:]_]|$)|(^|[^[:alnum:]_])give_up([^[:alnum:]_]|$)|(^|[[:space:]])Axiom([[:space:]]|$)'
if grep -nE "$PATTERN" "$FSQL/lib/PossibleOutcomeDeterminism.v" "$FSQL/q1/Q1AllOutcomes.v"; then
  die "unproved obligation marker found"
fi

echo "compile_q1_normalize_sound OK (NORMALIZE_SOUND_OK)"
