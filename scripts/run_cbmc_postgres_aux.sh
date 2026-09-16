#!/usr/bin/env bash
# Compile and run CBMC (or concrete g++) harnesses for Postgres aux kernels.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

QUERY="${1:-}"
HARNESS="${2:-}"
MODE="${3:-cbmc}" # cbmc | concrete

usage() {
  cat <<'EOF'
Usage: scripts/run_cbmc_postgres_aux.sh <q1|q6|all> [harness|all] [cbmc|concrete]

  q1 harnesses: kernel_homomorphism encode_decode execute_eq_fallback applydelta_eq_rebuild
  q6 harnesses: encode_decode execute_eq_fallback applydelta_eq_rebuild

  concrete — g++ build + run with fixed seeds (smoke; requires duckdb sibling)
  cbmc     — goto-cc + cbmc --unwind (requires cbmc/goto-cc on PATH)
EOF
}

if [[ -z "$QUERY" ]]; then
  usage
  exit 2
fi

HARNESS="${HARNESS:-all}"

DUCKDB_ROOT="${ACCEL_DUCKDB_ROOT:-$ROOT/../accelerator-codegen-duckdb}"
if [[ ! -f "$DUCKDB_ROOT/src/include/duckdb.hpp" ]]; then
  echo "DuckDB sibling not found at $DUCKDB_ROOT (set ACCEL_DUCKDB_ROOT)" >&2
  exit 1
fi

OUT_DIR="${ACCEL_CBMC_OUT:-$ROOT/build-verification}"
mkdir -p "$OUT_DIR"

COMMON_INCLUDES=(
  -I"$ROOT/include"
  -I"$ROOT/src/generated"
  -I"$ROOT/tests/verification/common"
  -I"$DUCKDB_ROOT/src/include"
)

COMMON_DEFS=(
  -DACCEL_VERIFICATION_CBMC=1
  -std=c++17
)

SUBSTRATE_SRCS=(
  "$ROOT/src/stateful/core/aux_registry.cpp"
  "$ROOT/src/stateful/core/aux_engine_services.cpp"
  "$ROOT/src/stateful/core/aux_maintenance_policy.cpp"
  "$ROOT/src/stateful/core/aux_maintenance_decision_log.cpp"
  "$ROOT/src/stateful/state/partitioned_state_store.cpp"
  "$ROOT/src/stateful/state/invalidation_plan.cpp"
  "$ROOT/src/stateful/maintenance/maintenance_action.cpp"
  "$ROOT/src/stateful/maintenance/action_selector.cpp"
  "$ROOT/tests/verification/common/accel_cbmc_bounds.cpp"
)

Q1_SRCS=(
  "$ROOT/src/generated/tpch/q1/postgres/aux/finalize.cpp"
  "$ROOT/src/generated/tpch/q1/postgres/aux/module.cpp"
)

Q6_SRCS=(
  "$ROOT/src/generated/tpch/q6/postgres/aux/finalize.cpp"
  "$ROOT/src/generated/tpch/q6/postgres/aux/module.cpp"
)

DUCKDB_LIB=""
if [[ -f "$DUCKDB_ROOT/build/release/src/libduckdb.so" ]]; then
  DUCKDB_LIB="$DUCKDB_ROOT/build/release/src/libduckdb.so"
elif [[ -f "$DUCKDB_ROOT/build/release/src/libduckdb_static.a" ]]; then
  DUCKDB_LIB="$DUCKDB_ROOT/build/release/src/libduckdb_static.a"
fi

run_one() {
  local query="$1"
  local name="$2"
  local harness="$ROOT/tests/verification/${query}/harness_${name}.cpp"
  if [[ ! -f "$harness" ]]; then
    echo "missing harness: $harness" >&2
    return 1
  fi

  local srcs=("${SUBSTRATE_SRCS[@]}" "$harness")
  if [[ "$query" == "q1" ]]; then
    if [[ "$name" == "kernel_homomorphism" ]]; then
      srcs=("$ROOT/src/generated/tpch/q1/postgres/aux/finalize.cpp" "$harness")
    else
      srcs+=("${Q1_SRCS[@]}")
    fi
  else
    srcs+=("${Q6_SRCS[@]}")
  fi

  local out_base="$OUT_DIR/${query}_${name}"
  echo "==> $query / $name ($MODE)"

  if [[ "$MODE" == "concrete" ]]; then
    if [[ -z "$DUCKDB_LIB" ]]; then
      echo "DuckDB library not built under $DUCKDB_ROOT/build/release/src" >&2
      return 1
    fi
    local duckdb_libdir
    duckdb_libdir="$(dirname "$DUCKDB_LIB")"
    g++ "${COMMON_DEFS[@]}" -DACCEL_VERIFICATION_CONCRETE=1 \
      "${COMMON_INCLUDES[@]}" \
      -o "${out_base}.bin" "${srcs[@]}" \
      "$DUCKDB_LIB" -lpthread -ldl -Wl,-rpath,"$duckdb_libdir"
    "${out_base}.bin"
    echo "PASS concrete $query/$name"
  elif [[ "$MODE" == "cbmc" ]]; then
    if ! command -v goto-cc >/dev/null 2>&1 || ! command -v cbmc >/dev/null 2>&1; then
      echo "cbmc/goto-cc not on PATH. Install CBMC or use mode 'concrete'." >&2
      echo "  e.g. https://github.com/diffblue/cbmc/releases" >&2
      return 1
    fi
    # goto-cc: produce a goto binary; link duckdb only when needed for concrete
    # symbols. Prefer --function main and bounded unwind.
    goto-cc "${COMMON_DEFS[@]}" "${COMMON_INCLUDES[@]}" \
      -o "${out_base}.goto" "${srcs[@]}" \
      ${DUCKDB_LIB:+"$DUCKDB_LIB"} || {
        echo "goto-cc failed; retrying without duckdb lib (headers only)" >&2
        goto-cc "${COMMON_DEFS[@]}" "${COMMON_INCLUDES[@]}" \
          -o "${out_base}.goto" "${srcs[@]}"
      }
    cbmc "${out_base}.goto" \
      --unwind 8 \
      --unwinding-assertions \
      --trace \
      --verbosity 4
    echo "PASS cbmc $query/$name"
  else
    echo "unknown mode: $MODE" >&2
    return 2
  fi
}

Q1_HARNESSES=(kernel_homomorphism encode_decode execute_eq_fallback applydelta_eq_rebuild)
Q6_HARNESSES=(encode_decode execute_eq_fallback applydelta_eq_rebuild)

run_query() {
  local q="$1"
  local h="$2"
  local -a list
  if [[ "$q" == "q1" ]]; then
    list=("${Q1_HARNESSES[@]}")
  else
    list=("${Q6_HARNESSES[@]}")
  fi
  if [[ "$h" == "all" ]]; then
    for name in "${list[@]}"; do
      run_one "$q" "$name"
    done
  else
    run_one "$q" "$h"
  fi
}

case "$QUERY" in
  q1) run_query q1 "$HARNESS" ;;
  q6) run_query q6 "$HARNESS" ;;
  all)
    run_query q1 "$HARNESS"
    run_query q6 "$HARNESS"
    ;;
  -h|--help) usage ;;
  *) usage; exit 2 ;;
esac
