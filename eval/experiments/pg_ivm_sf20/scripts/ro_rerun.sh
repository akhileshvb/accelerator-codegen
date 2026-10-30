#!/usr/bin/env bash
# Paper run: burton, 2026-10-19/20. Writes $RO_OUT/rep{1,2}/ and run.log (data/ro_rerun/ here), read by ../plot.py for the Read-only column.
# Read-only accel windows (interval 0) with the Q10-windows build, same flags / seed / reps as the MV sweep's rep 1 and 2.
REPO=${REPO:-$(git rev-parse --show-toplevel)}
cd "$REPO"
RO_OUT=${RO_OUT:?set RO_OUT to the output dir for the read-only rerun}
RF_DIR=${RF_DIR:?set RF_DIR to the dbgen refresh sets (rf1/rf2 update files) for SF20}
export PATH=$REPO/.venv-pg-test/bin:$PATH PGPASSWORD=postgres PG_PORT=5460
for K in 1 2; do
  R=first; [ $K = 2 ] && R=never
  python3 eval/compare/run_sql_backend.py writerate --backend postgres --db accel_tpch_sf20 --sf 20 --out $RO_OUT/rep$K \
    --queries "1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22" --rates 0 --seconds 300 --warmup 30 --readers 8 \
    --dim-after-rf --rf-dir $RF_DIR --batch-orders 500 --query-timeout 1800 --repeats 3 --sets 4 \
    --seed 20261018 --only-rep $K --min-reads 3 --max-seconds 900 --pg-workers 9 --systems accel --worker-restart $R \
    --max-commits 15000 2>&1 | tee -a $RO_OUT/run.log
done
echo "=== $(date) RO RERUN DONE" | tee -a $RO_OUT/run.log
