#!/usr/bin/env bash
# Q1 structural inductive POC: dafny verify (Finalize + ModuleTwin + Aux) + conformance
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

DFY_DIR="$ROOT/tests/verification/q1/dafny"
CONF_SRC="$ROOT/tests/verification/q1/conformance/test_q1_kernel_conformance.cpp"
FINALIZE="$ROOT/src/generated/tpch/q1/postgres/aux/finalize.cpp"
MODULE="$ROOT/src/generated/tpch/q1/postgres/aux/module.cpp"

find_dafny() {
  if command -v dafny >/dev/null 2>&1; then
    command -v dafny
    return
  fi
  if [[ -x "$ROOT/tools/dafny/dafny" ]]; then
    echo "$ROOT/tools/dafny/dafny"
    return
  fi
  return 1
}

if ! DAFNY_BIN="$(find_dafny)"; then
  cat <<'EOF' >&2
dafny not found on PATH or at tools/dafny/dafny.

Install options:
  - Download: https://github.com/dafny-lang/dafny/releases
    (e.g. dafny-*-x64-ubuntu-*.zip → extract to tools/dafny/)
  - Or: dotnet tool install --global dafny
EOF
  exit 1
fi

echo "==> dafny verify Q1Finalize"
"$DAFNY_BIN" verify "$DFY_DIR/Q1Finalize.dfy"

echo "==> dafny verify Q1ModuleTwin"
"$DAFNY_BIN" verify "$DFY_DIR/Q1ModuleTwin.dfy"

echo "==> dafny verify Q1Aux (derived Fold/Inv lemmas)"
"$DAFNY_BIN" verify "$DFY_DIR/Q1Aux.dfy"

echo "==> dafny verify Q1Maintenance (Inv under all maint actions)"
"$DAFNY_BIN" verify "$DFY_DIR/Q1Maintenance.dfy"

echo "==> dafny verify Q1MaintenanceEdges (delete-any, Install/CAS, policy)"
"$DAFNY_BIN" verify "$DFY_DIR/Q1MaintenanceEdges.dfy"

echo "==> dafny verify Q1Sql (MathSem SQL ≡ aux)"
"$DAFNY_BIN" verify "$DFY_DIR/Q1Sql.dfy"

DUCKDB_ROOT="${ACCEL_DUCKDB_ROOT:-$ROOT/../accelerator-codegen-duckdb}"
if [[ ! -f "$DUCKDB_ROOT/src/include/duckdb.hpp" ]]; then
  echo "DuckDB sibling not found at $DUCKDB_ROOT (set ACCEL_DUCKDB_ROOT)" >&2
  exit 1
fi

DUCKDB_LIB=""
if [[ -f "$DUCKDB_ROOT/build/release/src/libduckdb.so" ]]; then
  DUCKDB_LIB="$DUCKDB_ROOT/build/release/src/libduckdb.so"
elif [[ -f "$DUCKDB_ROOT/build/release/src/libduckdb_static.a" ]]; then
  DUCKDB_LIB="$DUCKDB_ROOT/build/release/src/libduckdb_static.a"
else
  echo "DuckDB library not built under $DUCKDB_ROOT/build/release/src" >&2
  exit 1
fi

OUT_DIR="${ACCEL_CBMC_OUT:-$ROOT/build-verification}"
mkdir -p "$OUT_DIR"
OUT="$OUT_DIR/q1_kernel_conformance.bin"
LIBDIR="$(dirname "$DUCKDB_LIB")"

SUBSTRATE=(
  "$ROOT/src/stateful/core/aux_registry.cpp"
  "$ROOT/src/stateful/core/aux_engine_services.cpp"
  "$ROOT/src/stateful/core/aux_maintenance_policy.cpp"
  "$ROOT/src/stateful/core/aux_maintenance_decision_log.cpp"
  "$ROOT/src/stateful/state/partitioned_state_store.cpp"
  "$ROOT/src/stateful/state/invalidation_plan.cpp"
  "$ROOT/src/stateful/maintenance/maintenance_action.cpp"
  "$ROOT/src/stateful/maintenance/action_selector.cpp"
)

echo "==> conformance (g++ vs postgres finalize.cpp + module.cpp)"
g++ -std=c++17 -O1 \
  -I"$ROOT/include" \
  -I"$ROOT/src/generated" \
  -I"$ROOT/tests/verification/common" \
  -I"$DUCKDB_ROOT/src/include" \
  -o "$OUT" \
  "$CONF_SRC" \
  "$FINALIZE" \
  "$MODULE" \
  "${SUBSTRATE[@]}" \
  "$DUCKDB_LIB" -lpthread -ldl \
  -Wl,-rpath,"$LIBDIR"

"$OUT"
echo "PASS q1 kernel conformance"

echo "==> SQL MathSem conformance"
SQL_OUT="$OUT_DIR/q1_sql_sem_conformance.bin"
g++ -std=c++17 -O1 \
  -I"$ROOT/include" \
  -I"$ROOT/src/generated" \
  -I"$DUCKDB_ROOT/src/include" \
  -o "$SQL_OUT" \
  "$ROOT/tests/verification/q1/conformance/test_q1_sql_sem_conformance.cpp" \
  "$FINALIZE" \
  "$DUCKDB_LIB" -lpthread -ldl \
  -Wl,-rpath,"$LIBDIR"
"$SQL_OUT"
echo "PASS q1 inductive POC (Finalize + ModuleTwin + Aux + Maintenance + Edges + Q1Sql + conformance)"
