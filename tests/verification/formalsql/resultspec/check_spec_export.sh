# RESULTSPEC_MODEL_EXPORT_OK — SpecExportCorrect TCB freshness check (Q1).
#
# Architecture (TCB shrink):
#   Rocq QueryResultSpec.v (AST) + EvalSpec_Q1 (→ AuxAnswerSpec)
#        |
#        | SpecExportCorrect [TCB — pin-checked dual]
#        |   now split as:
#        |     (1) data transcription  Q1ResultSpecData.dfy ↔ QueryResultSpec.v
#        |     (2) evaluator           Q1ResultSpec.dfy EvalResultSpec
#        v
#   Dafny EvalResultSpec (used by Q1ExecCorrectOrdered)
#
# Old trusted surface: entire Q1ResultSpec.dfy as opaque Rocq↔Dafny dual.
# New trusted surface: AST pin (BaseDate/Cutoff/S_Q1 aggs) + that the Dafny
# evaluator matches Rocq EvalSpec_Q1.  SpecApply contrib formulas remain
# query-specific (not yet a generic AggKind interpreter) — still in TCB, but
# demarcated from the data dual.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
ROCQ="$ROOT/tests/verification/formalsql/resultspec/QueryResultSpec.v"
EVAL="$ROOT/tests/verification/formalsql/resultspec/EvalSpec.v"
DFY="$ROOT/tests/verification/q1/dafny/generated/Q1ResultSpec.dfy"
DATA="$ROOT/tests/verification/q1/dafny/generated/Q1ResultSpecData.dfy"
PIN="q1-resultspec-poc-v1"

[[ -f "$ROCQ" ]] || { echo "missing $ROCQ" >&2; exit 1; }
[[ -f "$EVAL" ]] || { echo "missing $EVAL" >&2; exit 1; }
[[ -f "$DFY" ]] || { echo "missing $DFY" >&2; exit 1; }
[[ -f "$DATA" ]] || { echo "missing $DATA" >&2; exit 1; }
grep -q "$PIN" "$DFY" || { echo "Dafny Spec missing pin $PIN" >&2; exit 1; }
grep -q "$PIN" "$DATA" || { echo "Dafny Data missing pin $PIN" >&2; exit 1; }
grep -q "BaseDate: int := 10561" "$DATA" || { echo "Data BaseDate mismatch" >&2; exit 1; }
grep -q "q1_base_date : Z := 10561" "$ROCQ" || { echo "Rocq base date mismatch" >&2; exit 1; }
grep -q "Cutoff" "$DATA" || { echo "Data missing Cutoff" >&2; exit 1; }
grep -q "S_Q1" "$DATA" || { echo "Data missing S_Q1" >&2; exit 1; }
grep -q "EvalResultSpec" "$DFY" || { echo "Dafny Spec missing EvalResultSpec" >&2; exit 1; }
grep -q "EvalSpec_Q1" "$EVAL" || { echo "Rocq EvalSpec_Q1 missing" >&2; exit 1; }
grep -q "Q1ResultSpecData" "$DFY" || { echo "Evaluator must include Data module" >&2; exit 1; }
echo "RESULTSPEC_MODEL_EXPORT_OK (SpecExportCorrect TCB pin=$PIN; Data AST + EvalSpec.v)"
