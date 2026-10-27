#!/usr/bin/env bash
# A small cut of the SEC EDGAR `num` table in a fresh DuckDB file (the kit tests' TEST_DB).
#   make_test_db.sh <out.duckdb> [rows=5000]
# Env: SEC_NUM_TBL (default /flash1/akhilvb/datasets/sec_edgar_sf3_tbl/num.tbl), DUCKDB (the CLI).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
DUCKDB="${DUCKDB:-${ACCEL_DUCKDB_ROOT:-${ROOT}/../duckdb-dbcapture}/build/release/duckdb}"
OUT="$1"; ROWS="${2:-5000}"
rm -f "${OUT}" "${OUT}.wal"
"${DUCKDB}" "${OUT}" <<SQL
$(cat "${ROOT}/workloads/null_double/schema.sql")
INSERT INTO num SELECT * FROM read_csv('${SEC_NUM_TBL:-/flash1/akhilvb/datasets/sec_edgar_sf3_tbl/num.tbl}',
    delim = '|', header = false, auto_detect = false, ignore_errors = true,
    columns = {'adsh': 'VARCHAR', 'tag': 'VARCHAR', 'version': 'VARCHAR', 'ddate': 'INTEGER', 'qtrs': 'INTEGER',
               'uom': 'VARCHAR', 'coreg': 'VARCHAR', 'value': 'DOUBLE', 'footnote': 'VARCHAR'}) LIMIT ${ROWS};
SQL
