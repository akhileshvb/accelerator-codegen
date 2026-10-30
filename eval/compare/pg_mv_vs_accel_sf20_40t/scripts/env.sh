# common settings for the 40v40 PG MV-vs-accelerator sweep (source me)
export REPO=/spinning/akhilvb/accel_sf20/accelerator-codegen
export W=/spinning/akhilvb/pg40
export RF=$W/refresh_sf20
export PG_PORT=5470 PG_DB=accel_tpch_sf20 PGPASSWORD=postgres PGUSER=postgres
export T=accel_pg40
export SOCKET0=0-19,40-59 SOCKET1=20-39,60-79
export READERS=8 PGW=4                 # 8 readers x (leader + 4 workers) = 40
export MV_REFRESH_WORKERS=39           # Q21/Q22 refreshes may use the whole 40-thread budget
export SEED=35489017
PSQL="psql -h 127.0.0.1 -p $PG_PORT -U postgres -d $PG_DB -Atq"
CLIENT="taskset -c $SOCKET1"           # clients off the measured socket
pin_server() { docker update --cpuset-cpus "$SOCKET0" $T >/dev/null; }
