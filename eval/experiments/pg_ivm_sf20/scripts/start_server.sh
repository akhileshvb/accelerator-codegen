#!/usr/bin/env bash
# (Re)create accel_pg40 from image accel-pg40:17 (postgres:17.11 + pg_ivm 1.15 + accelerator libs) on the tmpfs data
# dir, pinned to socket 0. The same server settings serve both the MV and the accelerator runs.
# accel-pg40:17 was not scripted: docker commit of the load_sf20.sh container after installing postgresql-17-pg-ivm
# (pg_ivm 1.15, as scripts/ensure_pg_ivm.sh does) and copying the Release build's accelerator_codegen_pg.so and
# libaccelerator_codegen.so into /usr/lib/postgresql/17/lib. DATA is a restored tar of the loaded data dir.
# Env: W, RF (see env.sh), DATA.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
DATA=${DATA:-/tmp/pg40/data}
docker rm -f $T >/dev/null 2>&1 || true
docker run -d --name $T --cpuset-cpus "$SOCKET0" --shm-size=48g -e POSTGRES_PASSWORD=postgres \
  -p 127.0.0.1:$PG_PORT:5432 -v "$DATA":/var/lib/postgresql/data -v "$REPO":/accel-src:ro -v "$RF":/refresh:ro -v /dev/shm/pg40_ts:/ts \
  accel-pg40:17 \
  -c shared_buffers=16GB -c work_mem=256MB -c maintenance_work_mem=4GB -c max_wal_size=8GB -c min_wal_size=1GB \
  -c checkpoint_timeout=30min -c autovacuum=off -c wal_level=minimal -c max_wal_senders=0 \
  -c max_worker_processes=128 -c max_parallel_workers=40 -c max_parallel_workers_per_gather=$PGW \
  -c max_connections=200 >/dev/null
for _ in $(seq 180); do $PSQL -c 'select 1' >/dev/null 2>&1 && { echo "server up (cpuset $SOCKET0)"; exit 0; }; sleep 1; done
echo "server did not come up" >&2; docker logs --tail 30 $T >&2; exit 1
