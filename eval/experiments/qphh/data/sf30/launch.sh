#!/usr/bin/env bash
# QphH@30, same tree and settings as the SF20 headline (perf/sf20_20261020/qphh_full); STREAMS=4 is the spec minimum at SF30.
until awk '{exit !($1 < 4)}' /proc/loadavg; do sleep 30; done
cd /flash1/akhilvb/sf3000-qphh
DB=/flash1/akhilvb/tpch/tpch_sf30.duckdb RF_DIR=/flash1/akhilvb/tpch/refresh_sf30 RUNS=3 STREAMS=4 TMPDIR=/dev/shm \
  scripts/perf/run_qphh.sh /flash1/akhilvb/sf3000-qphh 30 0-39 /flash1/akhilvb/perf/sf30_20261023/qphh_full > /flash1/akhilvb/perf/sf30_20261023/qphh_full/console.log 2>&1
echo "exit=$?" > /flash1/akhilvb/perf/sf30_20261023/qphh_full/DONE
