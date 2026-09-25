#!/usr/bin/env bash
# Q12 verification gates (ResultSpec path — Q1-strength assurance).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

echo "=== Logos lower (already produced under formalsql/q12) ==="
if [[ ! -f tests/verification/formalsql/q12/generated/Queries.v ]]; then
  scripts/logos_lower_catalog.sh q12
fi

echo "=== Rocq ResultSpec normalize ==="
tests/verification/formalsql/scripts/compile_q12_resultspec.sh

echo "=== Rocq NormalizeSound_Q12 (NORMALIZE_SOUND_Q12) ==="
tests/verification/formalsql/scripts/compile_q12_normalize_sound.sh

echo "=== SpecExportCorrect pin (RESULTSPEC_MODEL_EXPORT_OK_Q12) ==="
tests/verification/formalsql/resultspec/check_spec_export_q12.sh

echo "=== Dafny SortedUniqueMap (shared) ==="
tools/dafny/dafny verify tests/verification/patterns/dafny/SortedUniqueMap.dfy

echo "=== Dafny Q12 Aux / Bridge / Maintenance / Runtime ==="
# Q12Aux: isolate-assertions + capped cores avoids z3 batch timeouts on fold commutation.
tools/dafny/dafny verify tests/verification/q12/dafny/Q12Aux.dfy --allow-warnings --isolate-assertions --cores 2 --verification-time-limit 180
tools/dafny/dafny verify tests/verification/q12/dafny/Q12ResultSpecBridge.dfy --allow-warnings --cores 2 --verification-time-limit 120
tools/dafny/dafny verify tests/verification/q12/dafny/Q12Maintenance.dfy --allow-warnings --cores 2 --verification-time-limit 120
tools/dafny/dafny verify tests/verification/q12/dafny/Q12RuntimeResultSpec.dfy --allow-warnings --cores 2 --verification-time-limit 120

# ORDER_MAINT_PROVED: no assume{:axiom} in order-maintenance lemmas
if rg -n 'assume \{\:axiom\}' tests/verification/q12/dafny/Q12Aux.dfy tests/verification/q12/dafny/Q12Maintenance.dfy; then
  echo "ORDER_MAINT_PROVED FAIL: residual assume in maint files" >&2
  exit 1
fi
echo "ORDER_MAINT_PROVED OK (no maint axioms)"

echo "run_verification_q12 OK"
