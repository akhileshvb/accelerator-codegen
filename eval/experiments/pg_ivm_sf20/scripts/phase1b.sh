#!/usr/bin/env bash
# MV design selection: read latency (warm medians) of v1 and the v2 candidates, and writer-alone maintenance cost
# (whole TPC-H update sets + point writes, 2 RF pairs) per write chunk and per design. Output: data/phase1/.
# Step C builds the v2 candidates; the v1 views (phase1/create_v1.log, check_v1.log) were made by hand the same way with
# MV_DESIGN=v1: mv_defs.py create --schema public --jobs 3 --workers 39; check --schema public --sets 4 --edges
# --workers 4 --out phase1/draws. Env: W, RF (see env.sh).
set -uo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
cd $REPO/eval/compare
L=$W/results/phase1; mkdir -p $L
V2Q="2 3 10 15 21 22"
ALLQ="$(seq -s ' ' 1 22)"
writer() {  # <out> <chunk> [env...]
  local D=$1 C=$2; shift 2; mkdir -p $D
  env "$@" $CLIENT python3 -B run_sql_backend.py writerate --backend postgres --db $PG_DB --port $PG_PORT --sf 20 \
    --rates -1 --seconds 86400 --warmup 0 --readers 0 --dim-after-rf --rf-dir $RF --query-timeout 1800 --repeats 1 \
    --sets 1 --max-write-commits 14 --seed $SEED --pg-workers $PGW --systems mv --schema public --chunk $C \
    --queries "$ALLQ" --out $D > $D/run.log 2>&1
  python3 $SCRIPTS/writer_summary.py $D
}
echo "=== $(date +%T) A: v1 read latency"
MV_DESIGN=v1 $CLIENT python3 -B $SCRIPTS/mv_readlat.py --out $L/lat_v1.json
echo "=== $(date +%T) B: writer alone, v1, chunk 5000"; writer $L/w_v1_c5000 5000 MV_DESIGN=v1
echo "=== $(date +%T) B: writer alone, v1, chunk 500";  writer $L/w_v1_c500 500 MV_DESIGN=v1
echo "=== $(date +%T) C: build v2 candidates ($V2Q)"
MV_DESIGN="$V2Q" $CLIENT python3 -B mv_defs.py create --schema public --jobs 3 --workers 39 --queries "$V2Q"
MV_DESIGN="$V2Q" $CLIENT python3 -B mv_defs.py check --schema public --sets 4 --edges --workers $PGW --queries "$V2Q" --out $L/draws
MV_DESIGN="$V2Q" $CLIENT python3 -B $SCRIPTS/mv_readlat.py --queries "$V2Q" --out $L/lat_v2.json
echo "=== $(date +%T) D: drop the v1-only views of $V2Q, writer alone with the v2 set"
$PSQL -c "DROP TABLE IF EXISTS mv_q2, mv_q3, mv_q10, mv_q15 CASCADE" -c "DROP MATERIALIZED VIEW IF EXISTS mv_q21, mv_q22 CASCADE"
BEST=$(python3 $SCRIPTS/writer_summary.py --best $L/w_v1_c5000 $L/w_v1_c500)
echo "best chunk: $BEST"
writer $L/w_v2_c$BEST $BEST MV_DESIGN="$V2Q"
echo "=== $(date +%T) PHASE1B DONE"
