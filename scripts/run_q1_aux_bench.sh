#!/usr/bin/env bash
# Q1 wrapper around the generic aux bench sweep.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

export ACCEL_Q1_WRITE_LOCALITY="${ACCEL_Q1_WRITE_LOCALITY:-single_month}"
export ACCEL_Q1_BENCH_MONTH="${ACCEL_Q1_BENCH_MONTH:-1995-06-01}"
export ACCEL_Q1_BENCH_DAY_OFFSET="${ACCEL_Q1_BENCH_DAY_OFFSET:-90}"
export ACCEL_Q1_BENCH_SUITES="${ACCEL_Q1_BENCH_SUITES:-lineitem_only}"
export ACCEL_Q1_WRITE_FREQUENCIES="${ACCEL_Q1_WRITE_FREQUENCIES:-0,1,10,50}"

exec "${ROOT}/scripts/run_aux_bench_sweep.sh" 1 "$@"
