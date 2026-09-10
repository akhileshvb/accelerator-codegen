#!/usr/bin/env bash
# Bench all 22 vs native PG SQL at 0% and 10% write-mix; dual-axis check.
set -euo pipefail
CONTAINER="${ACCEL_PG_CONTAINER:-accel_pg_test}"
MIN_RO="${ACCEL_AUX_MIN_SPEEDUP:-2.0}"
OUT="${1:-/tmp/pg_tpch_pgo_all22.csv}"
echo "query,write_pct,ref_ms,accel_ms,speedup" > "$OUT"
fail=0
for q in $(seq 1 22); do
  echo "=== bench Q${q} ==="
  declare -A sp
  for wp in 0 10; do
    set +e
    log=$(docker exec \
      -e ACCEL_PG_DOCKER=1 \
      -e PGHOST=127.0.0.1 -e PGPORT=5432 \
      -e PGUSER="${PGUSER:-postgres}" -e PGPASSWORD="${PGPASSWORD:-postgres}" \
      -e TPCH_PG_DB="${TPCH_PG_DB:-accel_tpch_test}" \
      -e ACCEL_ENGINE_NAME=postgres \
      -e ACCEL_WORKLOAD_KIND=aux_state \
      -e LD_LIBRARY_PATH=/accel-build/accel:/accel-build/duckdb/src:/usr/lib/postgresql/17/lib \
      "${CONTAINER}" \
      /accel-build/accel/benchmark_postgres_tpch_aux "${q}" "${wp}" 2>&1)
    rc=$?
    set -e
    echo "$log" | tee -a /tmp/pg_tpch_pgo_q${q}.log | tail -3
    line=$(echo "$log" | grep '^query,' | tail -1 || true)
    if [[ -z "$line" || $rc -ne 0 ]]; then
      echo "bench FAIL Q${q} wp=${wp}"
      fail=$((fail + 1))
      sp[$wp]=0
      continue
    fi
    # query,3,write_pct,0,ref_ms,...,accel_ms,...,workload_speedup,...
    speedup=$(echo "$line" | awk -F, '{for(i=1;i<=NF;i++) if($i=="workload_speedup") print $(i+1)}')
    ref=$(echo "$line" | awk -F, '{for(i=1;i<=NF;i++) if($i=="ref_ms") print $(i+1)}')
    accel=$(echo "$line" | awk -F, '{for(i=1;i<=NF;i++) if($i=="accel_ms") print $(i+1)}')
    echo "${q},${wp},${ref},${accel},${speedup}" >> "$OUT"
    sp[$wp]=$speedup
  done
  ro="${sp[0]:-0}"
  wm="${sp[10]:-0}"
  python3 - "$q" "$ro" "$wm" "$MIN_RO" <<'PY' || fail=$((fail + 1))
import math, sys
q, ro, wm, min_ro = int(sys.argv[1]), float(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4])
score = (max(ro, 1e-12) ** 0.75) * (max(wm, 1e-12) ** 0.25)
ok = ro >= min_ro and not (score < 1.0 and wm < 0.5)
print(f"Q{q} dual-axis ro={ro:.3f} wm={wm:.3f} score={score:.3f} {'PASS' if ok else 'FAIL'}")
sys.exit(0 if ok else 1)
PY
done
echo "wrote ${OUT}; bench_gate_failures=${fail}"
exit "${fail}"
