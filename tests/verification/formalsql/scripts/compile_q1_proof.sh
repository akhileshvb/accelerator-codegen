#!/usr/bin/env bash
# Compile Q1 FormalSQL AuxSpec + Embed + FormalSQLAuxCorrect/NoError against Logos pin.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
LOGOS="${LOGOS_ROOT:-$ROOT/third_party/Logos}"
FSQL="$ROOT/tests/verification/formalsql"
SWITCH="${ROCQ_OPAM_SWITCH:-$LOGOS/.opam-rocq}"

die() { echo "compile_q1_proof FAIL: $*" >&2; exit 1; }

command -v opam >/dev/null || die "opam not found"
[[ -d "$FSQL/q1/generated" ]] || die "missing Logos-generated Rocq under formalsql/q1/generated (run logos_lower_catalog.sh q1)"
[[ -f "$FSQL/q1/generated/Queries.v" ]] || die "missing Queries.v"
[[ -f "$FSQL/generated/Q1AuxSpec.v" ]] || die "missing Q1AuxSpec.v"

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
compile_one lib/FilterAcceptanceFacade.v
compile_one lib/ProjectExactFacade.v
compile_one lib/OrderRowsFacade.v
compile_one lib/GroupBagExactFacade.v
compile_one lib/GroupsExactFacade.v
compile_one lib/AggregateLeafExactFacade.v
compile_one lib/TemporalPredAcceptanceFacade.v
compile_one lib/NumericScaledFits.v
compile_one lib/NumericPackagingFacade.v
compile_one lib/NumericCallExactFacade.v
compile_one lib/KeyedSortFacade.v
compile_one lib/SortedGroupResultFacade.v
compile_one lib/ListNoDupFacade.v
compile_one lib/NumericFoldFacade.v
compile_one generated/Q1AuxSpec.v
cd "$FSQL/q1"
compile_one generated/Schema.v
compile_one generated/Queries.v
compile_one generated/Witness.v
compile_one Q1Catalog.v
compile_one Q1FormalSQLProof.v
compile_one Q1GroupingCorrespondence.v
compile_one Q1AuxCorrectAssembly.v
compile_one Q1Obligations.v
compile_one Q1FormalSQLSpine.v
compile_one Q1NumericEmbed.v
compile_one Q1GroupsCorrespond.v
compile_one Q1TCBAuxCorrect.v

echo "compile_q1_proof OK"
