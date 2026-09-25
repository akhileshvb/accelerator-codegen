#!/usr/bin/env bash
# RESULTSPEC_MODEL_EXPORT_OK — SpecExportCorrect TCB pin for Q12.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
ROCQ="$ROOT/tests/verification/formalsql/resultspec/QueryResultSpecQ12.v"
EVAL="$ROOT/tests/verification/formalsql/resultspec/Q12EvalSpec.v"
DFY="$ROOT/tests/verification/q12/dafny/generated/Q12ResultSpec.dfy"
DATA="$ROOT/tests/verification/q12/dafny/generated/Q12ResultSpecData.dfy"

[[ -f "$ROCQ" ]] || { echo "missing $ROCQ" >&2; exit 1; }
[[ -f "$EVAL" ]] || { echo "missing $EVAL" >&2; exit 1; }
[[ -f "$DFY" ]] || { echo "missing $DFY" >&2; exit 1; }
[[ -f "$DATA" ]] || { echo "missing $DATA" >&2; exit 1; }
grep -q "q12_catalog_start : Z := 9131" "$ROCQ" || { echo "Rocq catalog start mismatch" >&2; exit 1; }
grep -q "CatalogStart: int := 9131" "$DATA" || { echo "Dafny CatalogStart mismatch" >&2; exit 1; }
grep -q "S_Q12_catalog" "$ROCQ" || { echo "Rocq missing S_Q12_catalog" >&2; exit 1; }
grep -q "S_Q12_catalog" "$DATA" || { echo "Dafny Data missing S_Q12_catalog" >&2; exit 1; }
grep -q "EvalResultSpec" "$DFY" || { echo "Dafny missing EvalResultSpec" >&2; exit 1; }
grep -q "EvalSpec_Q12" "$EVAL" || { echo "Rocq missing EvalSpec_Q12" >&2; exit 1; }
echo "RESULTSPEC_MODEL_EXPORT_OK_Q12 (SpecExportCorrect TCB pin; Data AST + Q12EvalSpec.v)"
