#!/usr/bin/env bash
# Build a small SEC EDGAR DuckDB for shared-interface tests.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE="${SEC_EDGAR_SOURCE_DB:-${ROOT}/../accelerator-codegen-duckdb/sec_edgar_sf3.duckdb}"
DEST="${SEC_EDGAR_TEST_DB:-${ROOT}/../accelerator-codegen-duckdb/sec_edgar_test.duckdb}"
ORCH_VENV="${ROOT}/pipeline/orchestrator/.venv/bin/python"

resolve_python() {
  if [[ -n "${SEC_EDGAR_PYTHON:-}" ]] && "${SEC_EDGAR_PYTHON}" -c "import duckdb" 2>/dev/null; then
    echo "${SEC_EDGAR_PYTHON}"
    return 0
  fi
  if [[ -x "${ORCH_VENV}" ]] && "${ORCH_VENV}" -c "import duckdb" 2>/dev/null; then
    echo "${ORCH_VENV}"
    return 0
  fi
  if python3 -c "import duckdb" 2>/dev/null; then
    echo "python3"
    return 0
  fi
  echo "duckdb python package is required to build sec_edgar_test.duckdb" >&2
  exit 1
}

PYTHON="$(resolve_python)"
echo "=== SEC EDGAR test DB setup ==="
echo "Source: ${SOURCE}"
echo "Dest:   ${DEST}"
echo "Python: ${PYTHON}"

"${PYTHON}" "${ROOT}/scripts/create_sec_edgar_test_db.py" \
  --source "${SOURCE}" \
  --dest "${DEST}" \
  "$@"

echo ""
echo "Use for tests:"
echo "  export SEC_EDGAR_TEST_DB=${DEST}"
