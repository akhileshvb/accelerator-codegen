#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
bash "$ROOT/scripts/run_verification_q1.sh"
bash "$ROOT/scripts/run_verification_q21.sh"
echo "run_verification_all: OK"
