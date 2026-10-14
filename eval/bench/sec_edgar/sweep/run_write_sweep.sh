#!/usr/bin/env bash
# Write-frequency sweep, batched writes: both engines at 1k/10k/100k rows
# touched per second, random parameters by default. Run inside tmux.
#
# On the reader's virtual clock (SWEEP_QPS reads/s, SWEEP_READS reads), every
# row-write that has come due before a read is applied as ONE set-based UPDATE
# of that many distinct rows in ONE transaction (one write sample): at
# 100k rows/s and 2 reads/s, 3 batches of 50k rows before reads 2..4.
#
#   SWEEP_READS=4 ./run_write_sweep.sh
# Knobs (besides lib.sh): ROWS_PER_SEC="1000 10000 100000" (both engines;
# DUCK_RPS / PG_RPS override per engine), PARAM_MODES="random" (set
# "default random" for both), INCLUDE_READONLY=1 adds interval 0.
# Results: results/sweep-<ts>/ (results/sweep-latest).
ulimit -c 0
set -uo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
SWEEP_READS="${SWEEP_READS:-4}"
PARAM_MODES="${PARAM_MODES:-random}"
ROWS_PER_SEC="${ROWS_PER_SEC:-1000 10000 100000}"
DUCK_RPS="${DUCK_RPS:-$ROWS_PER_SEC}"
PG_RPS="${PG_RPS:-$ROWS_PER_SEC}"
sweep_init sweep
# interval_sec = 1 / (rows touched per second)
duck_iv="$(wps_to_intervals "$DUCK_RPS")"
pg_iv="$(wps_to_intervals "$PG_RPS" | xargs | tr ' ' ',')"
if [[ -n "${INCLUDE_READONLY:-}" ]]; then duck_iv="0 $duck_iv"; pg_iv="0,$pg_iv"; fi
log "sweep (batched writes): reads=$SWEEP_READS qps=$SWEEP_QPS modes=[$PARAM_MODES] seed=$ACCEL_SWEEP_PARAM_SEED duck_rows_per_sec=[$DUCK_RPS] pg_rows_per_sec=[$PG_RPS] out=$OUT"
run_all "$duck_iv" "$pg_iv" "$PARAM_MODES"
