#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
bash "$ROOT/scripts/run_verification_q1.sh"
# Q21 Stages 2-5 (+ live Postgres runtime gates when accel_pg_test is up);
# Q1 regressions already ran above.
bash "$ROOT/tests/verification/q21/run_q21_verification.sh" --skip-q1
echo "run_verification_all: OK"
