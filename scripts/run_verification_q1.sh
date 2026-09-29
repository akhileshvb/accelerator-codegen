#!/usr/bin/env bash
# Full Q1 verification gate (fail closed on Logos / FormalSQL / AuxSpec / Dafny).
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

echo "==> [2b] Rocq FormalSQLAuxCorrect / Embed compile"
bash "$ROOT/tests/verification/formalsql/scripts/compile_q1_proof.sh"

echo "==> [3] AuxSpec freshness"
python3 "$ROOT/tests/verification/auxspec/gen_auxspec.py" --check

echo "==> [4] Dafny Twin≡Spec + twins + substrate"
DAFNY_BIN="$(find_dafny)" || { echo "dafny not found"; exit 1; }
"$DAFNY_BIN" verify "$ROOT/tests/verification/q1/dafny/generated/Q1AuxSpec.dfy"
"$DAFNY_BIN" verify "$ROOT/tests/verification/q1/dafny/Q1TwinSpecBridge.dfy"
"$DAFNY_BIN" verify "$ROOT/tests/verification/q1/dafny/Q1Finalize.dfy"
"$DAFNY_BIN" verify "$ROOT/tests/verification/q1/dafny/Q1ModuleTwin.dfy"
"$DAFNY_BIN" verify "$ROOT/tests/verification/q1/dafny/Q1Aux.dfy"
"$DAFNY_BIN" verify "$ROOT/tests/verification/q1/dafny/Q1TwinAuxRefinement.dfy"
"$DAFNY_BIN" verify "$ROOT/tests/verification/q1/dafny/Q1TwinAuxClose.dfy"
"$DAFNY_BIN" verify "$ROOT/tests/verification/substrate/dafny/VersionedPairing.dfy"
"$DAFNY_BIN" verify "$ROOT/tests/verification/substrate/dafny/SnapshotPinning.dfy"
"$DAFNY_BIN" verify "$ROOT/tests/verification/substrate/dafny/ReadHandle.dfy"
"$DAFNY_BIN" verify "$ROOT/tests/verification/q1/dafny/Q1RuntimeAuxCorrect.dfy"

echo "==> [5] Existing Q1 induction / extract-diff"
bash "$ROOT/scripts/run_dafny_q1_induction.sh"

echo "run_verification_q1: stages that ran completed."
echo "Composed assurance claim JetStreamFormalSQLCorrect requires Rocq FormalSQLAuxCorrect + Dafny RuntimeAuxCorrect (see PIPELINE.md)."
