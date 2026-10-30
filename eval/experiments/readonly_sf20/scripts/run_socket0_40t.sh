#!/usr/bin/env bash
# Emulate a single Xeon Gold 6230 (20 cores / 40 HW threads): socket 0 only.
# CPUs 0-19 + HT siblings 40-59, memory bound to NUMA node 0. All systems at 40 threads.
# Produces data/sf20_40t_socket0_bespoke_gendb.csv (merged by hand from the three output dirs).
# Env: VENV_DIR (holds .venv144 and .venv, the DuckDB 1.4.4 / default envs), RESULTS (output dir); the Python
# scripts also need BESPOKE_ROOT, BESPOKE_DRIVER, BESPOKE_PARQUET, GENDB_ROOT, GENDB_DIR, GENDB_OUT (see their headers).
set -uo pipefail
cd "$(dirname "$0")"
VENV_DIR=${VENV_DIR:?VENV_DIR: dir containing .venv144 and .venv}; R=${RESULTS:?RESULTS: output dir}
PIN=(numactl --physcpubind=0-19,40-59 --membind=0)
export THREADS=40
export CORE_IDS="$(python3 -c 'print(",".join(map(str, [*range(0,20), *range(40,60)])))')"
echo "### Bespoke MT (40t) + DuckDB 1.4.4 (40t), Bespoke protocol"
"${PIN[@]}" $VENV_DIR/.venv144/bin/python -I bespoke_mt_allcore_repro.py 5 "$R/repro_bespoke_mt_40t_socket0_duckdb144" 2>&1 | grep -E "^Q|TOTAL|threads|Error|Traceback|assert"
grep -E "BUILD|POOL" "$R/repro_bespoke_mt_40t_socket0_duckdb144/bespoke_stdout.txt"
for v in .venv144 .venv; do
  echo "### GenDB SF20 (40t) + DuckDB ($v, 40t), GenDB protocol"
  "${PIN[@]}" $VENV_DIR/$v/bin/python -I gendb_paper_repro.py sf20 "$R/repro_gendb_sf20_dbgen_40t_socket0" 2>&1 | grep -E "^Q|TOTAL|threads|Error|Traceback|FAIL"
done
echo "ALL DONE"
