#!/usr/bin/env bash
# Generate a TPC-H DuckDB database at an arbitrary scale factor.
#
# Usage:
#   SF=1   bash scripts/setup_tpch_sf_db.sh
#   SF=20  bash scripts/setup_tpch_sf_db.sh
#   SF=3000 MIN_DISK_GB=4000 bash scripts/setup_tpch_sf_db.sh
#
# Large SF (esp. 1000/3000): single-step dbgen needs ~0.6×SF GB RAM
# (SF3000 ≈ 1.8 TB). This host has ~376 GB, so we partition with
# children/step (DuckDB docs). Override with CHILDREN=N.
#
# Prefers the locally built tpch.duckdb_extension.
# Default DEST: /disk3/akhilvb_tpch/duckdb_data/tpch_sf${SF}.duckdb when present.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DUCKDB_ROOT="${DUCKDB_ROOT:-${ROOT}/../accelerator-codegen-duckdb}"
SF="${SF:?set SF= (e.g. 1, 20, 3000)}"

if [[ -z "${DEST:-}" ]]; then
  if [[ -d /disk3/akhilvb_tpch/duckdb_data ]]; then
    DEST="/disk3/akhilvb_tpch/duckdb_data/tpch_sf${SF}.duckdb"
  else
    DEST="${DUCKDB_ROOT}/tpch_sf${SF}.duckdb"
  fi
fi

# Rough free-disk floor: DuckDB SF3000 file ~800GB + temp spill.
DEFAULT_MIN=$((SF < 5 ? 5 : SF))
MIN_DISK_GB="${MIN_DISK_GB:-${DEFAULT_MIN}}"

# Partition count for large SF. SF3000 single-step ≈ 1799 GB RAM; with
# children=C peak is roughly that / C. Aim for <~200 GB peak on this box.
if [[ -z "${CHILDREN:-}" ]]; then
  if (( SF >= 3000 )); then
    CHILDREN=20
  elif (( SF >= 1000 )); then
    CHILDREN=10
  elif (( SF >= 300 )); then
    CHILDREN=5
  else
    CHILDREN=1
  fi
fi

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

db_ok() {
  local path="$1"
  [[ -f "${path}" ]] || return 1
  # Reject empty stubs left after a killed dbgen (often 12–20 KB).
  local bytes
  bytes="$(stat -c%s "${path}" 2>/dev/null || echo 0)"
  if (( bytes < 1000000 )); then
    return 1
  fi
  local rows
  rows="$("${BIN}" "${path}" -c "SELECT COUNT(*) FROM lineitem;" 2>/dev/null | tr -dc '0-9' || true)"
  [[ -n "${rows}" && "${rows}" -gt 0 ]]
}

EXT="${TPCH_DUCKDB_EXTENSION:-${DUCKDB_ROOT}/build/release/extension/tpch/tpch.duckdb_extension}"
BIN="$(duckdb_bin)"
mkdir -p "$(dirname "${DEST}")"
free_gb="$(df -BG "$(dirname "${DEST}")" | awk 'NR==2 {gsub(/G/,"",$4); print $4}')"
echo "free_disk_gb=${free_gb} dest=${DEST} sf=${SF} min_disk_gb=${MIN_DISK_GB} children=${CHILDREN}"
echo "using duckdb=${BIN}"
if [[ "${free_gb}" -lt "${MIN_DISK_GB}" ]]; then
  echo "ERROR: need >= ${MIN_DISK_GB} GB free disk, have ${free_gb} GB" >&2
  exit 1
fi

if [[ -f "${DEST}" && "${FORCE:-0}" != "1" ]]; then
  if db_ok "${DEST}"; then
    echo "already exists and looks valid: ${DEST}"
    ls -lh "${DEST}"
    "${BIN}" "${DEST}" -c "SELECT COUNT(*) AS lineitem_rows FROM lineitem;"
    exit 0
  fi
  echo "WARN: ${DEST} exists but is incomplete/corrupt; regenerating (set FORCE=0 only after a good file)"
  FORCE=1
fi

TMPDIR_DB="${TMPDIR_DB:-$(dirname "${DEST}")/tmp_sf${SF}}"
mkdir -p "${TMPDIR_DB}"
LOG="${LOG:-$(dirname "${DEST}")/../logs/setup_sf${SF}.log}"
mkdir -p "$(dirname "${LOG}")"

rm -f "${DEST}"
{
  echo "=== setup_tpch_sf_db SF=${SF} start $(date -Is) ==="
  echo "dest=${DEST} children=${CHILDREN} tmp=${TMPDIR_DB}"
} | tee -a "${LOG}"

LOAD_SQL="LOAD '${EXT}';"
if [[ ! -f "${EXT}" ]]; then
  echo "WARN: local extension missing; trying INSTALL tpch" | tee -a "${LOG}"
  LOAD_SQL="INSTALL tpch; LOAD tpch;"
else
  echo "loading local extension ${EXT}" | tee -a "${LOG}"
fi

# Memory: leave headroom for OS; spill temp to disk3.
# Threads must be SET on every duckdb -c invocation (each is a new process).
MEM_LIMIT="${DUCKDB_MEMORY_LIMIT:-200GB}"
THREADS="${DUCKDB_THREADS:-128}"

run_sql() {
  "${BIN}" "${DEST}" -c "$1" 2>&1 | tee -a "${LOG}"
}

session_preamble() {
  cat <<SQL
${LOAD_SQL}
SET memory_limit='${MEM_LIMIT}';
SET temp_directory='${TMPDIR_DB}';
SET threads=${THREADS};
SQL
}

echo "duckdb threads=${THREADS} memory_limit=${MEM_LIMIT}" | tee -a "${LOG}"

# Create empty DB file with configured session.
run_sql "$(session_preamble)"

if (( CHILDREN <= 1 )); then
  echo "=== dbgen single-step sf=${SF} threads=${THREADS} ===" | tee -a "${LOG}"
  run_sql "
$(session_preamble)
CALL dbgen(sf=${SF});
"
else
  for (( step = 0; step < CHILDREN; step++ )); do
    echo "=== dbgen sf=${SF} children=${CHILDREN} step=${step}/$((CHILDREN - 1)) threads=${THREADS} $(date -Is) ===" | tee -a "${LOG}"
    run_sql "
$(session_preamble)
CALL dbgen(sf=${SF}, children=${CHILDREN}, step=${step});
"
  done
fi

if ! db_ok "${DEST}"; then
  echo "ERROR: generation finished but ${DEST} has no usable lineitem" | tee -a "${LOG}"
  exit 1
fi

echo "created ${DEST}" | tee -a "${LOG}"
ls -lh "${DEST}" | tee -a "${LOG}"
"${BIN}" "${DEST}" -c "SELECT COUNT(*) AS lineitem_rows FROM lineitem;" | tee -a "${LOG}"
echo "=== setup_tpch_sf_db SF=${SF} done $(date -Is) ===" | tee -a "${LOG}"

LINK="${DUCKDB_ROOT}/tpch_sf${SF}.duckdb"
if [[ "$(readlink -f "${DEST}")" != "$(readlink -f "${LINK}" 2>/dev/null || true)" ]]; then
  ln -sfn "${DEST}" "${LINK}"
  echo "symlinked ${LINK} -> ${DEST}" | tee -a "${LOG}"
fi
