#!/usr/bin/env bash
# Warm back-to-back C++ accelerator latency, all 22 queries, same 40 parameter sets as cpp_read, 30 rounds each.
# Output (stdout) is data/warm_sweep.log, read by plot_fig5.py.
# Env: ACCEL_DUCKDB_ROOT, DB (a scratch copy of tpch_sf20_pk.duckdb; aux_compare opens it for writing),
# CASES_DIR (holds the cases_q<N>_cpp_all.tsv parameter sets that cpp_read used); optional REPO.
REPO=${REPO:-$(git rev-parse --show-toplevel)}
cd "$REPO"
DB=${DB:?DB: scratch copy of tpch_sf20_pk.duckdb}; CASES_DIR=${CASES_DIR:?CASES_DIR: dir with cases_q<N>_cpp_all.tsv}
export ACCEL_DUCKDB_ROOT=${ACCEL_DUCKDB_ROOT:?ACCEL_DUCKDB_ROOT: root of the DuckDB capture fork}
export ACCEL_COMPARE_DUCKDB_THREADS=40 ACCEL_AUX_THREADS=40 ACCEL_AUX_READ_THREADS=40 ACCEL_AUX_BUILD_THREADS=40 ACCEL_AUX_MAINT_THREADS=40 ACCEL_AUX_DIAG=0
for q in $(seq 1 22); do
  unset ACCELERATOR_CODEGEN_DUCKDB_AUX_COST_MODEL
  m=$PWD/accelerators/tpch/q$q/config/q${q}_aux_cost_model_sf20.json; [ -f $m ] && export ACCELERATOR_CODEGEN_DUCKDB_AUX_COST_MODEL=$m
  r=$(ACCEL_COMPARE_PROFILE_LOOP=30 numactl --membind=0 taskset -c 0-19,40-59 build-release/aux_compare read \
      $DB tpch:$q workloads/tpch/q$q.sql $CASES_DIR/cases_q${q}_cpp_all.tsv 5 2>&1 | grep "profile loop")
  echo "Q$q $r"
done
echo DONE
