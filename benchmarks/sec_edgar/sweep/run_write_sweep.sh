#!/usr/bin/env bash
# Write-frequency sweep: DuckDB at 10/100/1000 writes/s, Postgres at 1k/10k/100k
# writes/s, random parameters by default. Run inside tmux.
#
#   SWEEP_READS=4 ./run_write_sweep.sh
# Knobs (besides lib.sh): DUCK_WPS="10 100 1000", PG_WPS="1000 10000 100000",
# PARAM_MODES="random" (set "default random" for both), INCLUDE_READONLY=1 adds interval 0.
# Results: results/sweep-<ts>/ (results/sweep-latest).
ulimit -c 0
set -uo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
SWEEP_READS="${SWEEP_READS:-4}"
PARAM_MODES="${PARAM_MODES:-random}"
DUCK_WPS="${DUCK_WPS:-10 100 1000}"
PG_WPS="${PG_WPS:-1000 10000 100000}"
sweep_init sweep
duck_iv="$(wps_to_intervals "$DUCK_WPS")"
pg_iv="$(wps_to_intervals "$PG_WPS" | xargs | tr ' ' ',')"
if [[ -n "${INCLUDE_READONLY:-}" ]]; then duck_iv="0 $duck_iv"; pg_iv="0,$pg_iv"; fi
log "sweep: reads=$SWEEP_READS qps=$SWEEP_QPS modes=[$PARAM_MODES] seed=$ACCEL_SWEEP_PARAM_SEED duck_wps=[$DUCK_WPS] pg_wps=[$PG_WPS] out=$OUT"
run_all "$duck_iv" "$pg_iv" "$PARAM_MODES"
