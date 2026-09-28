#!/usr/bin/env bash
# Compile generic FormalSQLLogical package (RelNode IR + Eval).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
LOGOS="${LOGOS_ROOT:-$ROOT/third_party/Logos}"
FSQL="$ROOT/tests/verification/formalsql"
SWITCH="${ROCQ_OPAM_SWITCH:-$LOGOS/.opam-rocq}"

die() { echo "compile_logical_ir FAIL: $*" >&2; exit 1; }
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
  -Q "$FSQL/logical" FormalSQLLogical
  -Q "$FSQL/resultspec" FormalSQLResultSpec
  -Q "$FSQL/q1/generated" LogosGenerated
  -Q "$FSQL/generated" JetStreamFS
)

compile_one() {
  echo "rocq compile $1"
  opam exec --switch="$SWITCH" -- rocq compile "${FLAGS[@]}" "$1"
}

cd "$FSQL"
compile_one logical/SqlTypes.v
compile_one logical/Schema.v
compile_one logical/Expr.v
compile_one logical/Agg.v
compile_one logical/RelNode.v
compile_one logical/LogicalSpecSerialize.v
compile_one logical/Typing.v
compile_one logical/Eval.v
compile_one logical/proof/Morphisms.v
compile_one generated/Q1AuxSpec.v
# QueryResultSpec provides cutoff_Q1 / q1_base_date for RelNode DATA export.
compile_one resultspec/QueryResultSpec.v
cd "$FSQL/q1"
compile_one Q1Schema.v
compile_one Q1LogicalSpecTree.v
compile_one Q1LogicalSpecSerialize.v
compile_one Q1Adapt.v
# Observational RelNode ↔ flat bridge (Qed; used by NormalizeSound retarget)
# Requires Q1EvalMigration.vo from compile_q1_normalize_sound / compile_q1_proof path.
if [[ -f Q1EvalMigration.vo && -f Q1FormalSQLProof.vo ]]; then
  compile_one Q1RelNodeBridge.v
fi
echo "compile_logical_ir OK"
