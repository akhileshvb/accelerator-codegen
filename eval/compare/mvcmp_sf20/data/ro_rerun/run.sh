#!/usr/bin/env bash
# Read-only accel windows (interval 0) with the Q10-windows build, same flags / seed / reps as the MV sweep's rep 1 and 2.
cd /home/akhilvb/accelerator-codegen
export PATH=/home/akhilvb/accelerator-codegen/.venv-pg-test/bin:$PATH PGPASSWORD=postgres PG_PORT=5460
O=/disk3/akhilvb_tpch/pg_sf20r/q10dev/ro_rerun
for K in 1 2; do
  R=first; [ $K = 2 ] && R=never
  python3 eval/compare/run_sql_backend.py writerate --backend postgres --db accel_tpch_sf20 --sf 20 --out $O/rep$K \
    --queries "1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22" --rates 0 --seconds 300 --warmup 30 --readers 8 \
    --dim-after-rf --rf-dir /disk3/akhilvb_tpch/refresh_sf20 --batch-orders 500 --query-timeout 1800 --repeats 3 --sets 4 \
    --seed 20261018 --only-rep $K --min-reads 3 --max-seconds 900 --pg-workers 9 --systems accel --worker-restart $R \
    --max-commits 15000 2>&1 | tee -a $O/run.log
done
echo "=== $(date) RO RERUN DONE" | tee -a $O/run.log
