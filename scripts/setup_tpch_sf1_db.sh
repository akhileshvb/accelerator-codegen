#!/usr/bin/env bash
# Create tpch_sf1.duckdb via the accelerator-codegen-duckdb fork's TPC-H extension.
#
# Usage:
#   bash scripts/setup_tpch_sf1_db.sh
#   DEST=/path/to/tpch_sf1.duckdb FORCE=1 bash scripts/setup_tpch_sf1_db.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DUCKDB_ROOT="${DUCKDB_ROOT:-${ROOT}/../accelerator-codegen-duckdb}"
DEST="${DEST:-${DUCKDB_ROOT}/tpch_sf1.duckdb}"
SF="${SF:-1}"
MIN_DISK_GB="${MIN_DISK_GB:-2}"

duckdb_bin() {
  if [[ -n "${DUCKDB_BIN:-}" && -x "${DUCKDB_BIN}" ]]; then
    echo "${DUCKDB_BIN}"
    return
  fi
  if [[ -x "${DUCKDB_ROOT}/build/release/duckdb" ]]; then
    echo "${DUCKDB_ROOT}/build/release/duckdb"
    return
  fi
  if command -v duckdb >/dev/null 2>&1; then
    command -v duckdb
    return
  fi
  echo "ERROR: duckdb binary not found; set DUCKDB_BIN" >&2
  exit 1
}

free_gb="$(df -BG "$(dirname "${DEST}")" | awk 'NR==2 {gsub(/G/,"",$4); print $4}')"
echo "free_disk_gb=${free_gb} dest=${DEST} sf=${SF}"
if [[ "${free_gb}" -lt "${MIN_DISK_GB}" ]]; then
  echo "ERROR: need >= ${MIN_DISK_GB} GB free disk, have ${free_gb} GB" >&2
  exit 1
fi

if [[ -f "${DEST}" && "${FORCE:-0}" != "1" ]]; then
  echo "already exists: ${DEST} (set FORCE=1 to regenerate)"
  ls -lh "${DEST}"
  exit 0
fi

BIN="$(duckdb_bin)"
echo "using duckdb=${BIN}"

rm -f "${DEST}"
"${BIN}" "${DEST}" <<SQL
LOAD tpch;
CALL dbgen(sf=${SF});
SQL

echo "created ${DEST}"
ls -lh "${DEST}"
"${BIN}" "${DEST}" -c "SELECT 'lineitem' AS t, COUNT(*) AS n FROM lineitem
UNION ALL SELECT 'part', COUNT(*) FROM part
UNION ALL SELECT 'partsupp', COUNT(*) FROM partsupp
UNION ALL SELECT 'supplier', COUNT(*) FROM supplier;"
