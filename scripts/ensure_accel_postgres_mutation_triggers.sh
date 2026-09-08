#!/usr/bin/env bash
# Ensure statement-level mutation capture triggers exist for aux incremental tests.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DB="${1:?usage: ensure_accel_postgres_mutation_triggers.sh <database>}"

PG_HOST="${PG_HOST:-127.0.0.1}"
PG_PORT="${PG_PORT:-5434}"
export PGHOST="$PG_HOST"
export PGPORT="$PG_PORT"
export PGUSER="${PG_USER:-postgres}"

psql_cmd() {
  PGPASSWORD="${PGPASSWORD:-postgres}" psql -v ON_ERROR_STOP=1 -d "$DB" "$@"
}

# Per-query lineitem triggers use query-specific table IDs and projections.
REQUIRED_LINEITEM_TRIGGER_PREFIXES=(
  accel_codegen_tpch_q1_lineitem_
  accel_codegen_tpch_q3_lineitem_
  accel_codegen_tpch_q3_orders_orders_
  accel_codegen_tpch_q6_lineitem_
  accel_codegen_tpch_q9_lineitem_
  accel_codegen_tpch_q12_lineitem_
  accel_codegen_tpch_q12_orders_orders_
  accel_codegen_tpch_q14_lineitem_
  accel_codegen_tpch_q17_lineitem_
  accel_codegen_tpch_q18_lineitem_
  accel_codegen_tpch_q21_lineitem_
  accel_codegen_tpch_q22_orders_
  accel_codegen_tpch_q22_customer_
  accel_codegen_tpch_q2_partsupp_
  accel_codegen_tpch_q4_lineitem_
  accel_codegen_tpch_q5_lineitem_
  accel_codegen_tpch_q7_lineitem_
  accel_codegen_tpch_q8_lineitem_
  accel_codegen_tpch_q13_orders_
  accel_codegen_tpch_q15_lineitem_
  accel_codegen_tpch_q16_partsupp_
  accel_codegen_tpch_q19_lineitem_
  accel_codegen_tpch_q20_lineitem_
)

missing_per_query_triggers() {
  local prefix count
  for prefix in "${REQUIRED_LINEITEM_TRIGGER_PREFIXES[@]}"; do
    count="$(psql_cmd -tAc "SELECT COUNT(*) FROM pg_trigger WHERE tgname LIKE '${prefix}%'")"
    if [[ "${count}" -lt 3 ]]; then
      return 0
    fi
  done
  return 1
}

trigger_count="$(psql_cmd -tAc "SELECT COUNT(*) FROM pg_trigger WHERE NOT tgisinternal")"
if [[ "${trigger_count}" -gt 0 ]] && ! missing_per_query_triggers; then
  echo "mutation triggers already present on ${DB} (${trigger_count})"
  exit 0
fi

if ! psql_cmd -tAc "SELECT 1 FROM pg_proc WHERE proname = 'accel_aux_capture_transition'" >/dev/null 2>&1; then
  echo "database ${DB} is missing accel_aux_capture_transition; install extension SQL first" >&2
  exit 1
fi

if [[ "${trigger_count}" -gt 0 ]]; then
  echo "installing missing per-query lineitem mutation triggers on ${DB}"
else
  echo "installing mutation capture triggers on ${DB}"
fi
psql_cmd -f "${ROOT}/scripts/postgres_accel_mutation_triggers.sql"
