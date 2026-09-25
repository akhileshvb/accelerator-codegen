#!/usr/bin/env bash
# Compile Q21 FormalSQL AuxSpec + Embed + FormalSQLAuxCorrect/NoError against Logos pin.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
LOGOS="${LOGOS_ROOT:-$ROOT/third_party/Logos}"
FSQL="$ROOT/tests/verification/formalsql"
SWITCH="${ROCQ_OPAM_SWITCH:-$LOGOS/.opam-rocq}"

die() { echo "compile_q21_proof FAIL: $*" >&2; exit 1; }

command -v opam >/dev/null || die "opam not found"
[[ -d "$FSQL/q21/generated" ]] || die "missing Logos-generated Rocq under formalsql/q21/generated (run logos_lower_catalog.sh q21)"
[[ -f "$FSQL/q21/generated/Queries.v" ]] || die "missing Queries.v"
[[ -f "$FSQL/generated/Q21AuxSpec.v" ]] || die "missing Q21AuxSpec.v"
[[ -f "$FSQL/q21/Q21Catalog.v" ]] || die "missing Q21Catalog.v (run logos_lower_catalog.sh q21)"

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
  -Q "$FSQL/q21/generated" LogosGenerated
  -Q "$FSQL/generated" JetStreamFS
)

compile_one() {
  local f="$1"
  echo "rocq compile $f"
  opam exec --switch="$SWITCH" -- rocq compile "${FLAGS[@]}" "$f"
}

cd "$FSQL"
compile_one lib/QueryJoinSpineFacade.v
compile_one generated/Q21AuxSpec.v
cd "$FSQL/q21"
compile_one generated/Schema.v
compile_one generated/Queries.v
compile_one generated/Witness.v
compile_one Q21Catalog.v
compile_one Q21FormalSQLProof.v
compile_one Q21Obligations.v
compile_one Q21FormalSQLSpine.v

echo "compile_q21_proof OK"
