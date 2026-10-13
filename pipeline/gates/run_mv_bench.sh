#!/usr/bin/env bash
# Read-only latency vs native reference SQL; write-mix includes REFRESH cost.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
if [[ -z "${QUERY}" ]]; then
  echo "usage: run_mv_bench.sh <query_number>" >&2
  exit 1
fi

# shellcheck source=mv_gate_common.sh
source "$(dirname "${BASH_SOURCE[0]}")/mv_gate_common.sh"
load_mv_gate_context "${ROOT}" "${QUERY}"
mkdir -p "${GATE_OUT}"

CONTAINER="${ACCEL_PG_CONTAINER:-accel_pg_test}"
DB="${TPCH_PG_DB:-accel_tpch_test}"
WARMUP="$(python3 -c "import json,sys; print((json.loads(sys.argv[1]).get('mv_bench') or {}).get('warmup', 2))" "${CTX}")"
QUERIES="$(python3 -c "import json,sys; print((json.loads(sys.argv[1]).get('mv_bench') or {}).get('queries', 10))" "${CTX}")"

if ! ensure_mv_postgres_ready "${ROOT}"; then
  write_mv_gate_json "mv_bench" "fail" "postgres not ready" "${QUERY}"
  exit 1
fi

set +e
bash "${ROOT}/scripts/postgres_matview_apply.sh" "${QUERY}" >/tmp/accel_mv_bench_apply.log 2>&1
APPLY_RC=$?
set -e
if [[ ${APPLY_RC} -ne 0 ]]; then
  write_mv_gate_json "mv_bench" "fail" "apply failed" "${QUERY}"
  exit 1
fi

REF_SQL="${ROOT}/${MV_REFERENCE_SQL}"
if [[ ! -f "${REF_SQL}" && -n "${REFERENCE_SQL_PATH}" && -f "${REFERENCE_SQL_PATH}" ]]; then
  REF_SQL="${REFERENCE_SQL_PATH}"
fi
QUERY_SQL="${ROOT}/${MV_QUERY_SQL}"

if [[ ! -f "${REF_SQL}" || ! -f "${QUERY_SQL}" ]]; then
  write_mv_gate_json "mv_bench" "fail" "missing reference or residual SQL" "${QUERY}"
  exit 1
fi

python3 - "${REF_SQL}" "${QUERY_SQL}" "${MV_RELATION_NAME}" \
  "${GATE_OUT}/_mv_bench.sql" "${WARMUP}" "${QUERIES}" <<'PY'
import sys
from pathlib import Path

ref = Path(sys.argv[1]).read_text(encoding="utf-8").strip().rstrip(";")
mv = Path(sys.argv[2]).read_text(encoding="utf-8").strip().rstrip(";")
rel = sys.argv[3]
out = Path(sys.argv[4])
warmup = int(sys.argv[5])
queries = int(sys.argv[6])

lines = [
    "CREATE TEMP TABLE IF NOT EXISTS _mv_bench_times (kind text, ms double precision);",
    "TRUNCATE _mv_bench_times;",
]
for kind, sql in (("native", ref), ("matview", mv)):
    for _ in range(warmup):
        lines.append(f"SELECT COUNT(*) FROM ({sql}) AS _q;")
    for _ in range(queries):
        lines.append(
            "DO $$ DECLARE t0 timestamptz; t1 timestamptz; BEGIN "
            f"t0 := clock_timestamp(); PERFORM COUNT(*) FROM ({sql}) AS _q; "
            "t1 := clock_timestamp(); "
            f"INSERT INTO _mv_bench_times VALUES ('{kind}', 1000.0 * EXTRACT(EPOCH FROM (t1 - t0))); "
            "END $$;"
        )
lines.append(
    "DO $$ DECLARE t0 timestamptz; t1 timestamptz; BEGIN "
    f"t0 := clock_timestamp(); REFRESH MATERIALIZED VIEW {rel}; "
    "t1 := clock_timestamp(); "
    "INSERT INTO _mv_bench_times VALUES ('refresh', 1000.0 * EXTRACT(EPOCH FROM (t1 - t0))); "
    "END $$;"
)
lines.append(
    "SELECT kind, "
    "percentile_cont(0.5) WITHIN GROUP (ORDER BY ms) AS median_ms, "
    "avg(ms) AS mean_ms, count(*) AS n "
    "FROM _mv_bench_times GROUP BY kind ORDER BY kind;"
)
out.write_text("\n".join(lines) + "\n", encoding="utf-8")
PY

set +e
BENCH_OUT="$(docker exec -i \
  -e PGPASSWORD="${PGPASSWORD:-postgres}" \
  "${CONTAINER}" \
  psql -v ON_ERROR_STOP=1 -U "${PGUSER:-postgres}" -d "${DB}" -F ',' -A -t \
  < "${GATE_OUT}/_mv_bench.sql" 2>&1)"
BENCH_RC=$?
set -e

echo "${BENCH_OUT}" > "${GATE_OUT}/mv_bench_raw.txt"

if [[ ${BENCH_RC} -ne 0 ]]; then
  write_mv_gate_json "mv_bench" "fail" "bench failed: ${BENCH_OUT:0:400}" "${QUERY}"
  echo "${BENCH_OUT}" >&2
  exit 1
fi

python3 - "${GATE_OUT}/mv_bench.json" "${BENCH_OUT}" "${QUERY}" "${MV_RELATION_NAME}" "${MV_FUNCTION_NAME}" <<'PY'
import json, sys
out_path, raw, query, rel, fn = sys.argv[1:6]
native = matview = refresh = None
for line in raw.strip().splitlines():
    parts = [p.strip() for p in line.split(",")]
    if len(parts) < 3:
        continue
    kind, median, mean = parts[0], parts[1], parts[2]
    try:
        row = {"median_ms": float(median), "mean_ms": float(mean)}
    except ValueError:
        continue
    if kind == "native":
        native = row
    elif kind == "matview":
        matview = row
    elif kind == "refresh":
        refresh = row
speedup = None
if native and matview and matview["median_ms"] > 0:
    speedup = native["median_ms"] / matview["median_ms"]
payload = {
    "gate": "mv_bench",
    "query": int(query),
    "status": "pass" if speedup is not None else "fail",
    "reason": "timed native vs matview residual + refresh sample" if speedup is not None else "missing timings",
    "workload_kind": "materialized_view",
    "mv_relation_name": rel,
    "mv_function_name": fn,
    "native_median_ms": None if not native else native["median_ms"],
    "matview_median_ms": None if not matview else matview["median_ms"],
    "refresh_median_ms": None if not refresh else refresh["median_ms"],
    "read_only_speedup_vs_native": speedup,
}
open(out_path, "w", encoding="utf-8").write(json.dumps(payload, indent=2) + "\n")
raise SystemExit(0 if payload["status"] == "pass" else 1)
PY
