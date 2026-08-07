#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE="${TPCH_SOURCE_DB:-${ROOT}/../accelerator-codegen-duckdb/tpch_sf1.duckdb}"
DEST="${TPCH_TEST_DB:-${ROOT}/../accelerator-codegen-duckdb/tpch_test.duckdb}"
ORCH_VENV="${ROOT}/pipeline/orchestrator/.venv/bin/python"
PYTHON="${SEC_EDGAR_PYTHON:-}"
if [[ -z "${PYTHON}" ]]; then
  if [[ -x "${ORCH_VENV}" ]] && "${ORCH_VENV}" -c "import duckdb" 2>/dev/null; then
    PYTHON="${ORCH_VENV}"
  elif python3 -c "import duckdb" 2>/dev/null; then
    PYTHON="python3"
  else
    echo "duckdb python package is required" >&2
    exit 1
  fi
fi
"${PYTHON}" "${ROOT}/scripts/create_tpch_test_db.py" --source "${SOURCE}" --dest "${DEST}" "$@"
