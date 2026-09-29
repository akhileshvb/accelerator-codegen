#!/usr/bin/env bash
# Q21 Stage-2 gate: build the shared LogicalSpec proof library and the full
# Q21 FormalSQL → RelNode proof chain (through NormalizeSound_Q21 and the
# all-outcomes strengthening Q21AllOutcomes), and fail
# on any Admitted / admit / give_up / Axiom in those sources.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
LOGOS="${LOGOS_ROOT:-$ROOT/third_party/Logos}"
FSQL="$ROOT/tests/verification/formalsql"
SWITCH="${ROCQ_OPAM_SWITCH:-$LOGOS/.opam-rocq}"

die() { echo "compile_q21_stage2 FAIL: $*" >&2; exit 1; }
command -v opam >/dev/null || die "opam not found"
if [[ -d "$SWITCH/_opam" || -L "$SWITCH/_opam" ]]; then
  _prefix="$SWITCH/_opam"
else
  _prefix="$(opam var prefix --switch="$SWITCH")"
fi
export ROCQLIB="${ROCQLIB:-$_prefix/lib/coq}"
export COQLIB="${COQLIB:-$ROCQLIB}"
export OCAMLFIND_CONF="${OCAMLFIND_CONF:-$_prefix/lib/findlib.conf}"

# q21/ is bound first so its generated/ subdir is then rebound to LogosGenerated.
FLAGS=(
  -Q "$FSQL/q21" ""
  -Q "$LOGOS/vendor/FormalSQL/src" SQLFS
  -Q "$LOGOS/theories" Logos
  -Q "$FSQL/lib" FormalSQLLib
  -Q "$FSQL/logical" FormalSQLLogical
  -Q "$FSQL/resultspec" FormalSQLResultSpec
  -Q "$FSQL/q21/generated" LogosGenerated
  -Q "$FSQL/generated" JetStreamFS
)

PROOF_LIB=(ValueCorrespond SlotMap ScanCorrespond Morphisms)
FSQL_LIB=(PossibleOutcomeDeterminism)
Q21_CHAIN=(Q21Schema Q21LogicalSpecTree Q21Qhat Q21LogicalNormalize
  Q21LogicalSpecSerialize Q21FormalSQLProof Q21NormalizeSound
  Q21OuterOrdinarySpine Q21ExistsL2 Q21NotExistsL3 Q21WhereFusion
  Q21GroupCount Q21NormalizeSoundFinal Q21AllOutcomes)

PATTERN='Admitted|(^|[^[:alnum:]_])admit([^[:alnum:]_]|$)|(^|[^[:alnum:]_])give_up([^[:alnum:]_]|$)|(^|[[:space:]])Axiom([[:space:]]|$)'
files=()
for f in "${PROOF_LIB[@]}"; do files+=("$FSQL/logical/proof/$f.v"); done
for f in "${FSQL_LIB[@]}"; do files+=("$FSQL/lib/$f.v"); done
for f in "${Q21_CHAIN[@]}"; do files+=("$FSQL/q21/$f.v"); done
if grep -nE "$PATTERN" "${files[@]}"; then
  die "unproved obligation marker found"
fi

compile_one() {
  echo "rocq compile $1"
  opam exec --switch="$SWITCH" -- rocq compile "${FLAGS[@]}" "$1" 2>&1 \
    | grep -v -E 'remapped to|previously bound|overriding-logical-loadpath|^Warning:$|q21/generated$' \
    | grep -E 'Error|^File' || true
  [[ -f "${1%.v}.vo" && "${1%.v}.vo" -nt "$1" ]] || die "compile failed: $1"
}

cd "$FSQL"
for f in "${PROOF_LIB[@]}"; do compile_one "logical/proof/$f.v"; done
for f in "${FSQL_LIB[@]}"; do compile_one "lib/$f.v"; done
# Logos-generated schema / query terms (tracked sources; build them here so
# the chain never depends on stale or missing .vo files).
cd "$FSQL/q21/generated"
for f in Schema Queries; do compile_one "$f.v"; done
cd "$FSQL/q21"
for f in "${Q21_CHAIN[@]}"; do compile_one "$f.v"; done
echo "compile_q21_stage2 OK"
