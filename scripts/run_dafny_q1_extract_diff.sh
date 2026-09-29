#!/usr/bin/env bash
# Compile Q1 Dafny twins to C# and differentially run them against generated
# postgres aux C++ on the shared bounded grid D.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

DFY_MAIN="$ROOT/tests/verification/q1/extract/Q1ExtractMain.dfy"
CPP_MAIN="$ROOT/tests/verification/q1/extract/q1_cpp_extract_diff.cpp"
COMPARE="$ROOT/tests/verification/q1/extract/compare_extract_diff.py"

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
EOF
  exit 1
fi

if [[ -x "${DOTNET_ROOT:-$HOME/.dotnet}/dotnet" ]]; then
  export DOTNET_ROOT="${DOTNET_ROOT:-$HOME/.dotnet}"
  export PATH="$DOTNET_ROOT:$PATH"
fi
if ! command -v dotnet >/dev/null 2>&1; then
  echo "dotnet not found on PATH (required for dafny build --target:cs)" >&2
  echo "Install the .NET SDK or set DOTNET_ROOT." >&2
  exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
  echo "python3 not found on PATH (required to compare avg rational vs double)" >&2
  exit 1
fi

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
LIBDIR="$(dirname "$DUCKDB_LIB")"

OUT_DIR="${ACCEL_EXTRACT_OUT:-$ROOT/build-verification}"
mkdir -p "$OUT_DIR"
EXTRACT_DIR="$OUT_DIR/q1_extract"
rm -rf "$EXTRACT_DIR"
mkdir -p "$EXTRACT_DIR"

echo "==> dafny build --target:cs Q1ExtractMain"
"$DAFNY_BIN" build --target:cs --no-verify --output "$EXTRACT_DIR/Q1ExtractMain" "$DFY_MAIN"

run_csharp() {
  local dll=""
  if [[ -f "$EXTRACT_DIR/Q1ExtractMain.dll" ]]; then
    dll="$EXTRACT_DIR/Q1ExtractMain.dll"
  else
    dll="$(find "$EXTRACT_DIR" -name 'Q1ExtractMain.dll' -print -quit || true)"
  fi
  if [[ -n "$dll" ]]; then
    dotnet exec "$dll"
    return
  fi
  if [[ -x "$EXTRACT_DIR/Q1ExtractMain" ]]; then
    "$EXTRACT_DIR/Q1ExtractMain"
    return
  fi
  echo "could not find compiled C# extract at $EXTRACT_DIR" >&2
  find "$EXTRACT_DIR" -maxdepth 3 -type f | head -n 50 >&2
  return 1
}

echo "==> run C# extract"
run_csharp >"$OUT_DIR/q1_extract_dafny.out"

echo "==> g++ q1_cpp_extract_diff"
g++ -std=c++17 -O1 \
  -I"$ROOT/include" \
  -I"$ROOT/src/generated" \
  -I"$ROOT/tests/verification/common" \
  -I"$DUCKDB_ROOT/src/include" \
  -o "$OUT_DIR/q1_cpp_extract_diff.bin" \
  "$CPP_MAIN" \
  "$ROOT/src/generated/tpch/q1/postgres/aux/finalize.cpp" \
  "$ROOT/src/generated/tpch/q1/postgres/aux/module.cpp" \
  "$ROOT/src/stateful/core/aux_engine_services.cpp" \
  "$ROOT/src/stateful/core/aux_maintenance_policy.cpp" \
  "$ROOT/src/stateful/core/aux_maintenance_decision_log.cpp" \
  "$ROOT/src/stateful/maintenance/action_selector.cpp" \
  "$ROOT/src/stateful/maintenance/maintenance_action.cpp" \
  "$DUCKDB_LIB" -lpthread -ldl \
  -Wl,-rpath,"$LIBDIR"

echo "==> run C++ dump"
"$OUT_DIR/q1_cpp_extract_diff.bin" >"$OUT_DIR/q1_extract_cpp.out"

echo "==> compare dumps"
python3 "$COMPARE" "$OUT_DIR/q1_extract_dafny.out" "$OUT_DIR/q1_extract_cpp.out"
echo "PASS q1 Dafny-extract ≡ C++ on grid D"
