#!/usr/bin/env bash
# Fail if a Q1 final theorem still assumes the whole-query bag equality.
# Q1_GroupBagsPerm_holds is a lemma; using it as a hypothesis is a residual.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
Q1="$ROOT/tests/verification/formalsql/q1"

files=(
  "$Q1/Q1NormalizeSound.v"
  "$Q1/Q1TCBAuxCorrect.v"
  "$Q1/Q1GroupsCorrespond.v"
)

if rg -n 'Q1_GroupBagsPerm db delta ->' "${files[@]}"; then
  echo "NO_QUERY_SEMANTIC_RESIDUAL FAIL: Q1_GroupBagsPerm is still a hypothesis" >&2
  exit 1
fi

echo "NO_QUERY_SEMANTIC_RESIDUAL OK"
