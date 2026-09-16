#!/usr/bin/env bash
# Substrate + Postgres backend structural inductive verification
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

find_dafny() {
  if command -v dafny >/dev/null 2>&1; then command -v dafny; return; fi
  if [[ -x "$ROOT/tools/dafny/dafny" ]]; then echo "$ROOT/tools/dafny/dafny"; return; fi
  return 1
}

if ! DAFNY_BIN="$(find_dafny)"; then
  echo "dafny not found (PATH or tools/dafny/dafny)" >&2
  exit 1
fi

DFY_FILES=(
  "$ROOT/tests/verification/substrate/dafny/Store.dfy"
  "$ROOT/tests/verification/substrate/dafny/MaintenancePolicy.dfy"
  "$ROOT/tests/verification/substrate/dafny/Coordinator.dfy"
  "$ROOT/tests/verification/postgres/dafny/PostgresBackend.dfy"
  "$ROOT/tests/verification/postgres/dafny/RuntimeScan.dfy"
  "$ROOT/tests/verification/postgres/dafny/BackendCompose.dfy"
  "$ROOT/tests/verification/postgres/dafny/Q1BackendProofs.dfy"
)

for f in "${DFY_FILES[@]}"; do
  echo "==> dafny verify $(basename "$f")"
  "$DAFNY_BIN" verify "$f"
done

DUCKDB_ROOT="${ACCEL_DUCKDB_ROOT:-$ROOT/../accelerator-codegen-duckdb}"
if [[ ! -f "$DUCKDB_ROOT/src/include/duckdb.hpp" ]]; then
  echo "DuckDB sibling not found at $DUCKDB_ROOT" >&2
  exit 1
fi
DUCKDB_LIB=""
if [[ -f "$DUCKDB_ROOT/build/release/src/libduckdb.so" ]]; then
  DUCKDB_LIB="$DUCKDB_ROOT/build/release/src/libduckdb.so"
elif [[ -f "$DUCKDB_ROOT/build/release/src/libduckdb_static.a" ]]; then
  DUCKDB_LIB="$DUCKDB_ROOT/build/release/src/libduckdb_static.a"
else
  echo "DuckDB library missing under $DUCKDB_ROOT/build/release/src" >&2
  exit 1
fi
LIBDIR="$(dirname "$DUCKDB_LIB")"
OUT_DIR="${ACCEL_CBMC_OUT:-$ROOT/build-verification}"
mkdir -p "$OUT_DIR"

COMMON_INC=(
  -I"$ROOT/include"
  -I"$ROOT/src/generated"
  -I"$ROOT/tests/verification/common"
  -I"$DUCKDB_ROOT/src/include"
)

SUBSTRATE_SRCS=(
  "$ROOT/src/stateful/state/partitioned_state_store.cpp"
  "$ROOT/src/stateful/core/aux_maintenance_policy.cpp"
  "$ROOT/src/stateful/core/aux_maintenance_decision_log.cpp"
  "$ROOT/src/stateful/core/aux_engine_services.cpp"
  "$ROOT/src/stateful/maintenance/action_selector.cpp"
  "$ROOT/src/stateful/maintenance/maintenance_action.cpp"
)

echo "==> substrate conformance"
g++ -std=c++17 -O1 "${COMMON_INC[@]}" \
  -o "$OUT_DIR/substrate_conformance.bin" \
  "$ROOT/tests/verification/substrate/conformance/test_substrate_conformance.cpp" \
  "${SUBSTRATE_SRCS[@]}" \
  "$DUCKDB_LIB" -lpthread -ldl -Wl,-rpath,"$LIBDIR"
"$OUT_DIR/substrate_conformance.bin"

PG_SRCS=(
  "$ROOT/src/stateful/postgres/postgres_state_store.cpp"
  "$ROOT/src/stateful/postgres/postgres_aux_backend.cpp"
  "$ROOT/src/stateful/core/aux_registry.cpp"
  "$ROOT/src/stateful/state/invalidation_plan.cpp"
  "${SUBSTRATE_SRCS[@]}"
)

echo "==> postgres backend conformance"
g++ -std=c++17 -O1 "${COMMON_INC[@]}" \
  -o "$OUT_DIR/postgres_backend_conformance.bin" \
  "$ROOT/tests/verification/postgres/conformance/test_postgres_backend_conformance.cpp" \
  "${PG_SRCS[@]}" \
  "$DUCKDB_LIB" -lpthread -ldl -Wl,-rpath,"$LIBDIR"
"$OUT_DIR/postgres_backend_conformance.bin"

echo "==> runtime scan projection conformance"
g++ -std=c++17 -O1 "${COMMON_INC[@]}" \
  -o "$OUT_DIR/runtime_scan_conformance.bin" \
  "$ROOT/tests/verification/postgres/conformance/test_runtime_scan_conformance.cpp" \
  "$DUCKDB_LIB" -lpthread -ldl -Wl,-rpath,"$LIBDIR"
"$OUT_DIR/runtime_scan_conformance.bin"

echo "PASS substrate + postgres backend inductive POC (+ Q1BackendProofs)"
