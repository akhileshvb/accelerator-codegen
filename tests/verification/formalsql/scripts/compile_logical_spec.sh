#!/usr/bin/env bash
# Compile LogicalSpec + compositional normalize + Eval_Spec bridges (Phase 1–3 scaffold).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
LOGOS="${LOGOS_ROOT:-$ROOT/third_party/Logos}"
FSQL="$ROOT/tests/verification/formalsql"
SWITCH="${ROCQ_OPAM_SWITCH:-$LOGOS/.opam-rocq}"

die() { echo "compile_logical_spec FAIL: $*" >&2; exit 1; }
command -v opam >/dev/null || die "opam not found"

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
  echo "rocq compile $1"
  opam exec --switch="$SWITCH" -- rocq compile "${FLAGS[@]}" "$1"
}

cd "$FSQL"
compile_one lib/QuerySpineFacade.v
compile_one generated/Q1AuxSpec.v
compile_one resultspec/QueryResultSpec.v
compile_one resultspec/LogicalSpec.v
compile_one resultspec/LogicalSpecSerialize.v
compile_one resultspec/NormalizeQuery.v
compile_one resultspec/NormalizeCompositional.v
compile_one resultspec/EvalSpec.v
compile_one resultspec/EvalLogicalSpec.v
compile_one resultspec/NormalizeSoundGeneric.v
echo "compile_logical_spec OK"
