# Common settings for the 40v40 PG MV-vs-accelerator sweep (sourced by the other scripts).
# Required: W (work dir; results go to $W/results, logs to $W/logs), RF (dbgen refresh-set dir).
export SCRIPTS=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
export REPO=${REPO:-$(git -C "$SCRIPTS" rev-parse --show-toplevel)}
export W=${W:?W: work directory for results/ and logs/}
export RF=${RF:?RF: directory of the dbgen refresh sets}
export PG_PORT=5470 PG_DB=accel_tpch_sf20 PGPASSWORD=postgres PGUSER=postgres
export T=accel_pg40
export SOCKET0=0-19,40-59 SOCKET1=20-39,60-79
export READERS=8 PGW=4                 # 8 readers x (leader + 4 workers) = 40
export MV_REFRESH_WORKERS=39           # Q21/Q22 refreshes may use the whole 40-thread budget
export SEED=35489017
PSQL="psql -h 127.0.0.1 -p $PG_PORT -U postgres -d $PG_DB -Atq"
CLIENT="taskset -c $SOCKET1"           # clients off the measured socket
pin_server() { docker update --cpuset-cpus "$SOCKET0" $T >/dev/null; }
