#!/usr/bin/env bash
# Postgres-only aux performance sweep on production-scale benchmark databases.
#
# Prerequisites (run once):
#   bash scripts/setup_postgres_aux_performance_dbs.sh
#
# Run a subset of queries, e.g. TPC-H Q6 only:
#   TPCH_QUERIES=6 SKIP_SEC_QUERIES=1 bash scripts/run_postgres_aux_performance_sweep.sh
#
# SEC EDGAR only (e.g. Q2–Q4, Q6, Q24 after fixes):
#   SKIP_TPCH_QUERIES=1 SEC_QUERIES="2 3 4 6 24" bash scripts/run_postgres_aux_performance_sweep.sh
#
# Results:
#   benchmarks/aux_state/results/postgres_aux_performance_sweep.csv
#   benchmarks/aux_state/results/postgres_aux_performance_summary.csv
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

export SKIP_DUCKDB=1
export SKIP_DIFFERENTIAL="${SKIP_DIFFERENTIAL:-1}"
export PG_TPCH_DB="${PG_TPCH_DB:-accel_aux_bench_q10}"
export PG_SEC_DB="${PG_SEC_DB:-accel_sec_edgar_sf3}"
export OUT="${OUT:-${ROOT}/benchmarks/aux_state/results/postgres_aux_performance_sweep.csv}"
export SUMMARY="${SUMMARY:-${ROOT}/benchmarks/aux_state/results/postgres_aux_performance_summary.csv}"
export LOG_DIR="${LOG_DIR:-${ROOT}/benchmarks/aux_state/results/postgres_aux_performance_logs}"

exec bash "${ROOT}/scripts/run_shared_aux_performance_sweep.sh"
