#!/usr/bin/env bash
# Emulate a single Xeon Gold 6230 (20 cores / 40 HW threads): socket 0 only.
# CPUs 0-19 + HT siblings 40-59, memory bound to NUMA node 0. All systems at 40 threads.
set -uo pipefail
cd "$(dirname "$0")"
PIN=(numactl --physcpubind=0-19,40-59 --membind=0)
export THREADS=40
export CORE_IDS="$(python3 -c 'print(",".join(map(str, [*range(0,20), *range(40,60)])))')"
R=$HOME/olap-80t/results
echo "### Bespoke MT (40t) + DuckDB 1.4.4 (40t), Bespoke protocol"
"${PIN[@]}" ../.venv144/bin/python -I bespoke_mt_allcore_repro.py 5 "$R/repro_bespoke_mt_40t_socket0_duckdb144" 2>&1 | grep -E "^Q|TOTAL|threads|Error|Traceback|assert"
grep -E "BUILD|POOL" "$R/repro_bespoke_mt_40t_socket0_duckdb144/bespoke_stdout.txt"
for v in .venv144 .venv; do
  echo "### GenDB SF20 (40t) + DuckDB ($v, 40t), GenDB protocol"
  "${PIN[@]}" ../$v/bin/python -I gendb_paper_repro.py sf20 "$R/repro_gendb_sf20_dbgen_40t_socket0" 2>&1 | grep -E "^Q|TOTAL|threads|Error|Traceback|FAIL"
done
echo "ALL DONE"
