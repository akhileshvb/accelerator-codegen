#!/usr/bin/env bash
# READ-ONLY experiment (write interval 0): all six SEC-EDGAR queries, DuckDB and
# Postgres, in both default and random parameter modes. Run inside tmux.
#
#   SWEEP_READS=8 ./run_readonly.sh          # PARAM_MODES="default random" by default
# See lib.sh for all env knobs. Results: results/readonly-<ts>/ (results/readonly-latest).
ulimit -c 0
set -uo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
SWEEP_READS="${SWEEP_READS:-8}"
PARAM_MODES="${PARAM_MODES:-default random}"
sweep_init readonly
log "read-only: reads=$SWEEP_READS qps=$SWEEP_QPS modes=[$PARAM_MODES] seed=$ACCEL_SWEEP_PARAM_SEED out=$OUT"
run_all "0" "0" "$PARAM_MODES"
