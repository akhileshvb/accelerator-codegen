#!/usr/bin/env bash
# Setup SEC-EDGAR benchmark data and DuckDB database for accelerator-codegen.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SEC_DIR="${ROOT}/benchmarks/sec_edgar"
YEARS="${1:-3}"
DUCKDB_DEST="${ROOT}/../accelerator-codegen-duckdb/sec_edgar_sf3.duckdb"
ORCH_VENV="${ROOT}/pipeline/orchestrator/.venv/bin/python"

resolve_python() {
  local venv_dir="${ROOT}/benchmarks/sec_edgar/.venv"
  local venv_py="${venv_dir}/bin/python"

  if [[ -n "${SEC_EDGAR_PYTHON:-}" ]] && "${SEC_EDGAR_PYTHON}" -c "import duckdb" 2>/dev/null; then
    echo "${SEC_EDGAR_PYTHON}"
    return 0
  fi
  if [[ -x "${ORCH_VENV}" ]] && "${ORCH_VENV}" -c "import duckdb" 2>/dev/null; then
    echo "${ORCH_VENV}"
    return 0
  fi
  if [[ -x "${venv_py}" ]] && "${venv_py}" -c "import duckdb" 2>/dev/null; then
    echo "${venv_py}"
    return 0
  fi
  if python3 -c "import duckdb" 2>/dev/null; then
    echo "python3"
    return 0
  fi

  echo "Creating Python venv at ${venv_dir} (duckdb for load_data.py)..." >&2
  python3 -m venv "${venv_dir}"
  "${venv_py}" -m pip install --upgrade pip >/dev/null
  "${venv_py}" -m pip install duckdb pyyaml >/dev/null
  echo "${venv_py}"
}

PYTHON="$(resolve_python)"

echo "=== SEC-EDGAR setup (years=${YEARS}) ==="
echo "Using Python: ${PYTHON}"

# 1. Download raw SEC quarterly TSVs
quarter_count="$(find "${SEC_DIR}/data" -mindepth 1 -maxdepth 1 -type d -name '*q*' 2>/dev/null | wc -l | tr -d ' ')"
if [[ "${quarter_count}" -eq 0 ]]; then
  echo "Downloading SEC EDGAR data..."
  bash "${SEC_DIR}/setup_data.sh" "${YEARS}"
else
  echo "Data directory already has ${quarter_count} quarter(s): ${SEC_DIR}/data"
fi

# 2. Build DuckDB
echo "Loading data into DuckDB..."
"${PYTHON}" "${SEC_DIR}/load_data.py" --years "${YEARS}"

# 3. Split queries into per-query SQL files
"${PYTHON}" "${SEC_DIR}/split_queries.py"

# 4. Copy to sibling duckdb repo (TPC-H convention)
mkdir -p "$(dirname "${DUCKDB_DEST}")"
cp "${SEC_DIR}/duckdb/sec_edgar.duckdb" "${DUCKDB_DEST}"
echo "Installed database: ${DUCKDB_DEST}"

# 5. Optional ground truth (for precomputed test mode)
"${PYTHON}" "${SEC_DIR}/generate_ground_truth.py" --db-path "${SEC_DIR}/duckdb/sec_edgar.duckdb" || true

echo ""
echo "=== SEC-EDGAR setup complete ==="
echo "  DB: ${DUCKDB_DEST}"
echo "  Env: export ACCEL_SEC_EDGAR_DB=${DUCKDB_DEST}"
