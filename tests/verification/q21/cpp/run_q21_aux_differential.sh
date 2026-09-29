#!/usr/bin/env bash
# Build + run the Q21 aux differential for every engine variant.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
OUT="${TMPDIR:-/tmp}/q21_aux_differential"
mkdir -p "$OUT"
SRC=("$ROOT/src/stateful/core/aux_engine_services.cpp"
     "$ROOT/src/stateful/core/aux_maintenance_policy.cpp"
     "$ROOT/src/stateful/core/aux_maintenance_decision_log.cpp"
     "$ROOT/src/stateful/maintenance/action_selector.cpp"
     "$ROOT/src/stateful/state/invalidation_plan.cpp")
for engine in postgres duckdb mysql; do
  g++ -std=c++20 -O2 -DQ21_HDR="\"tpch/q21/$engine/aux/module.hpp\"" \
    -I"$ROOT/include" -I"$ROOT/src/generated" -I"$ROOT/src" \
    "$ROOT/tests/verification/q21/cpp/q21_aux_differential.cpp" \
    "$ROOT/src/generated/tpch/q21/$engine/aux/module.cpp" "${SRC[@]}" \
    -o "$OUT/q21_$engine"
  echo "== $engine"
  "$OUT/q21_$engine" | grep -E 'MISMATCH|^fuzz:|^mismatches'
done
echo "q21_aux_differential OK"
