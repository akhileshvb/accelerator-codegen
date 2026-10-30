#!/usr/bin/env bash
# After the MV sweep: drop every view (no IVM triggers during accelerator writes; frees /dev/shm), register the
# TPC-H accelerators + capture triggers, thread GUCs = 40, then the same rf-fixed sweep.   accel_phase.sh FIRST_REP REPEATS
# Output: data/sweep_accel, data/logs/{register,sweep_accel_r0}.log. Env: W, RF (see env.sh).
set -uo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
FIRST_REP=${1:-0} REPEATS=${2:-1}
cd $REPO/eval/compare
python3 -B mv_defs.py drop --schema public
$PSQL -c "select count(*) as ivm_triggers from pg_trigger where tgname like 'IVM_trigger%'" -c "select count(*) as matviews from pg_matviews"
if ! $PSQL -c "select 1 from pg_proc where proname = 'accel_tpch_q1_aux'" | grep -q 1; then
  psql -h 127.0.0.1 -p $PG_PORT -U postgres -d $PG_DB -v ON_ERROR_STOP=1 -v scripts=$REPO/scripts \
    -f $REPO/scripts/perf/pg_sf20/register.sql > $W/logs/register.log 2>&1 || { echo "register FAILED"; tail -20 $W/logs/register.log; exit 1; }
fi
N=40 T=$T PG_PORT=$PG_PORT DB=$PG_DB bash $REPO/scripts/perf/pg_sf20/set_gucs.sh
docker inspect -f 'cpuset {{.HostConfig.CpusetCpus}}' $T
$SCRIPTS/sweep.sh accel $FIRST_REP $REPEATS
