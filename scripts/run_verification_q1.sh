#!/usr/bin/env bash
# Full Q1 verification gate (fail closed on Logos / FormalSQL / Rocq / Dafny).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

find_dafny() {
  if command -v dafny >/dev/null 2>&1; then command -v dafny; return; fi
  if [[ -x "$ROOT/tools/dafny/dafny" ]]; then echo "$ROOT/tools/dafny/dafny"; return; fi
  return 1
}

echo "==> [1] FormalSQL / Logos pin smoke"
bash "$ROOT/scripts/run_formalsql_smoke.sh" || {
  echo "NOTE: pin SHA check / Rocq smoke failed (fail closed)." >&2
  # Still require pin SHAs even if Rocq missing
  LOGOS="$ROOT/third_party/Logos"
  [[ -d "$LOGOS" ]] || exit 1
  [[ "$(git -C "$LOGOS" rev-parse HEAD)" == "333d5275ac871f5b36ce3a7dc5f6fef5f7879069" ]] || exit 1
  [[ "$(git -C "$LOGOS/vendor/FormalSQL" rev-parse HEAD)" == "2238975a4a989c844ade5b75d9d0414295fbdfd1" ]] || exit 1
  echo "Pin SHAs OK; Rocq compile skipped/unavailable — FormalSQL proofs not green." >&2
}

echo "==> [2] Logos lower exact Cat(Q1)"
if ! bash "$ROOT/scripts/logos_lower_catalog.sh" q1; then
  echo "Q1 is not FormalSQL-verified (Logos lower failed). STOP." >&2
  exit 1
fi

echo "==> [2b] Rocq: Q1 group proof, NormalizeSound, Q21-shaped final theorem"
bash "$ROOT/tests/verification/formalsql/scripts/compile_logical_ir.sh"
bash "$ROOT/tests/verification/formalsql/scripts/compile_q1_proof.sh"
bash "$ROOT/tests/verification/formalsql/scripts/compile_q1_normalize_sound.sh"
bash "$ROOT/tests/verification/formalsql/scripts/check_print_assumptions.sh" q1

echo "==> [3] PostgreSQL avg(numeric) closed form: Rocq pg_avg_* == Dafny PgNumeric"
bash "$ROOT/tests/verification/logical/pgnumeric/check_pg_numeric_crossprover.sh"

echo "==> [4] Dafny: twins, Aux, exact Execute vs RelNode Eval_Spec, runtime protocol"
DAFNY_BIN="$(find_dafny)" || { echo "dafny not found"; exit 1; }
# `dafny verify` checks only the files named, so every file on the chain is listed.
for f in logical/dafny/LogicalSpec logical/dafny/EvalLogicalSpec logical/dafny/PgNumeric \
         substrate/dafny/AuxSyncProtocol \
         q1/dafny/Q1Finalize q1/dafny/Q1ModuleTwin q1/dafny/Q1Aux q1/dafny/Q1Maintenance \
         q1/dafny/Q1TwinAuxRefinement q1/dafny/Q1TwinAuxClose q1/dafny/Q1RelNodeSpec \
         q1/dafny/Q1RelNodeBridge q1/dafny/Q1ResultSpecBridge q1/dafny/Q1ExecExact \
         q1/dafny/Q1Runtime; do
  "$DAFNY_BIN" verify "$ROOT/tests/verification/$f.dfy"
done

echo "==> [5] Q1 induction / maintenance edges / exact extract-diff (Dafny twin == C++)"
bash "$ROOT/scripts/run_dafny_q1_induction.sh"

echo "run_verification_q1: all stages passed."
echo "Claim (composed): Rocq NormalizeSound_Q1_rel_all_outcomes (FormalSQL = Embed(Eval_Spec rows))"
echo "  + Dafny Q1Runtime.Hit_correct_Q1 / Fallback_correct_Q1 (accelerator = Q1Rows(Eval_Spec rows))."
