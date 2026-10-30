#!/usr/bin/env bash
# Warm back-to-back C++ accelerator latency, all 22 queries, same 40 parameter sets as cpp_read, 30 rounds each.
# Output (stdout) is data/warm_sweep.log, the optional WARM_LOG argument of plot_40t_samebase.py.
# DB: a scratch copy of tpch_sf20_pk.duckdb (aux_compare opens it for writing).
cd /spinning/akhilvb/accel_sf20/accelerator-codegen
DB=${DB:-/spinning/akhilvb/accel_sf20/tmp/q17_probe.duckdb}
export ACCEL_DUCKDB_ROOT=/spinning/akhilvb/accel_sf20/accelerator-codegen-duckdb TMPDIR=/spinning/akhilvb/accel_sf20/tmp
export ACCEL_COMPARE_DUCKDB_THREADS=40 ACCEL_AUX_THREADS=40 ACCEL_AUX_READ_THREADS=40 ACCEL_AUX_BUILD_THREADS=40 ACCEL_AUX_MAINT_THREADS=40 ACCEL_AUX_DIAG=0
for q in $(seq 1 22); do
  unset ACCELERATOR_CODEGEN_DUCKDB_AUX_COST_MODEL
  m=$PWD/accelerators/tpch/q$q/config/q${q}_aux_cost_model_sf20.json; [ -f $m ] && export ACCELERATOR_CODEGEN_DUCKDB_AUX_COST_MODEL=$m
  r=$(ACCEL_COMPARE_PROFILE_LOOP=30 numactl --membind=0 taskset -c 0-19,40-59 build-release/aux_compare read \
      $DB tpch:$q workloads/tpch/q$q.sql /ssd1/akhilvb/accel_sf20_work/cases_q${q}_cpp_all.tsv 5 2>&1 | grep "profile loop")
  echo "Q$q $r"
done
echo DONE
