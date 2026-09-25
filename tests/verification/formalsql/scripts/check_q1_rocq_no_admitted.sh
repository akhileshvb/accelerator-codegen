#!/usr/bin/env bash
# Fail if any Q1 Rocq source on the dependency path still contains an
# `Admitted`, an `admit` tactic, or an `Axiom` (i.e. an unproved obligation).
#
# Scope: the hand-written Q1 proof + catalog, the generated AuxSpec, and every
# generated Rocq file under q1/generated.  Keep this list in sync with the
# files compiled by compile_q1_proof.sh.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
FSQL="$ROOT/tests/verification/formalsql"

# Pattern for genuine unproved obligations.  Matches the vernac `Admitted.`,
# the `admit` / `give_up` tactics, and `Axiom`/`Admit ...` declarations.
PATTERN='Admitted|(^|[^[:alnum:]_])admit([^[:alnum:]_]|$)|(^|[^[:alnum:]_])give_up([^[:alnum:]_]|$)|(^|[[:space:]])Axiom([[:space:]]|$)'

files=()
add() { [[ -f "$1" ]] && files+=("$1"); }

add "$FSQL/lib/QuerySpineFacade.v"
add "$FSQL/lib/FilterAcceptanceFacade.v"
add "$FSQL/lib/ProjectExactFacade.v"
add "$FSQL/lib/OrderRowsFacade.v"
add "$FSQL/lib/GroupBagExactFacade.v"
add "$FSQL/lib/GroupsExactFacade.v"
add "$FSQL/lib/AggregateLeafExactFacade.v"
add "$FSQL/lib/TemporalPredAcceptanceFacade.v"
add "$FSQL/lib/NumericCallExactFacade.v"
add "$FSQL/lib/NumericScaledFits.v"
add "$FSQL/lib/NumericPackagingFacade.v"
add "$FSQL/lib/KeyedSortFacade.v"
add "$FSQL/lib/SortedGroupResultFacade.v"
add "$FSQL/lib/ListNoDupFacade.v"
add "$FSQL/lib/NumericFoldFacade.v"
add "$FSQL/q1/Q1FormalSQLProof.v"
add "$FSQL/q1/Q1GroupingCorrespondence.v"
add "$FSQL/q1/Q1AuxCorrectAssembly.v"
add "$FSQL/q1/Q1Obligations.v"
add "$FSQL/q1/Q1FormalSQLSpine.v"
add "$FSQL/q1/Q1NumericEmbed.v"
add "$FSQL/q1/Q1GroupsCorrespond.v"
add "$FSQL/q1/Q1TCBAuxCorrect.v"
add "$FSQL/q1/Q1Catalog.v"
add "$FSQL/generated/Q1AuxSpec.v"
# All generated Rocq files on the Q1 path.
while IFS= read -r -d '' f; do files+=("$f"); done \
  < <(find "$FSQL/q1/generated" -maxdepth 1 -name '*.v' -print0 2>/dev/null)

if [[ ${#files[@]} -eq 0 ]]; then
  echo "check_q1_rocq_no_admitted FAIL: no Q1 Rocq sources found" >&2
  exit 1
fi

echo "check_q1_rocq_no_admitted: scanning ${#files[@]} file(s)"
if grep -nE "$PATTERN" "${files[@]}"; then
  echo "check_q1_rocq_no_admitted FAIL: found Admitted/admit/Axiom above" >&2
  exit 1
fi

echo "check_q1_rocq_no_admitted OK: no Admitted/admit/Axiom on the Q1 path"
