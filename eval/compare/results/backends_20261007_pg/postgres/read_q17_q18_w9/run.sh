#!/bin/bash
source ~/accel_pg_env.sh
cd /home/akhilvb/accelerator-codegen
$PYTHON /home/akhilvb/accelerator-codegen/benchmarks/compare/results/backends_20261007_pg/postgres/read_q17_q18_w9/run_read_w9.py read --backend postgres --db $PG_DB --shared-state --sf 20 --out /home/akhilvb/accelerator-codegen/benchmarks/compare/results/backends_20261007_pg/postgres/read_q17_q18_w9 \
  --queries "17 18" --seed 2716834049 --repeats 3 --sets 4 --reps 5 --base-reps 2 --query-timeout 3600 2>&1 | tee -i /home/akhilvb/accelerator-codegen/benchmarks/compare/results/backends_20261007_pg/postgres/read_q17_q18_w9/run.log
