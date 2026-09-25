#!/usr/bin/env bash
# Fail if any Q21 Rocq source on the dependency path still contains an
# `Admitted`, an `admit` tactic, or an `Axiom` (i.e. an unproved obligation).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
FSQL="$ROOT/tests/verification/formalsql"

PATTERN='Admitted|(^|[^[:alnum:]_])admit([^[:alnum:]_]|$)|(^|[^[:alnum:]_])give_up([^[:alnum:]_]|$)|(^|[[:space:]])Axiom([[:space:]]|$)'

files=()
add() { [[ -f "$1" ]] && files+=("$1"); }

add "$FSQL/lib/QueryJoinSpineFacade.v"
add "$FSQL/q21/Q21FormalSQLProof.v"
add "$FSQL/q21/Q21Obligations.v"
add "$FSQL/q21/Q21FormalSQLSpine.v"
add "$FSQL/q21/Q21Catalog.v"
add "$FSQL/generated/Q21AuxSpec.v"
while IFS= read -r -d '' f; do files+=("$f"); done \
  < <(find "$FSQL/q21/generated" -maxdepth 1 -name '*.v' -print0 2>/dev/null)

if [[ ${#files[@]} -eq 0 ]]; then
  echo "check_q21_rocq_no_admitted FAIL: no Q21 Rocq sources found" >&2
  exit 1
fi

echo "check_q21_rocq_no_admitted: scanning ${#files[@]} file(s)"
if grep -nE "$PATTERN" "${files[@]}"; then
  echo "check_q21_rocq_no_admitted FAIL: found Admitted/admit/Axiom above" >&2
  exit 1
fi

echo "check_q21_rocq_no_admitted OK: no Admitted/admit/Axiom on the Q21 path"
