#!/usr/bin/env bash
# Reorganize src/ into runtime/, stateful/, harness/, and generated/ layouts.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

mkdir -p \
  src/runtime/duckdb \
  src/runtime/parallel \
  src/runtime/catalog \
  src/runtime/postgres \
  src/stateful/core \
  src/stateful/maintenance \
  src/stateful/state \
  src/stateful/duckdb \
  src/stateful/postgres \
  src/harness \
  src/generated/catalog \
  src/generated/postgres

git_mv() {
  if [[ -e "$1" ]]; then
    mkdir -p "$(dirname "$2")"
    git mv "$1" "$2"
  fi
}

# --- runtime scaffolding ---
git_mv src/query_runtime.cpp src/runtime/duckdb/query_runtime.cpp
git_mv src/register.cpp src/runtime/duckdb/register.cpp
git_mv src/duckdb_scan_adapter.cpp src/runtime/duckdb/duckdb_scan_adapter.cpp
git_mv src/parallel/parallel_runtime.cpp src/runtime/parallel/parallel_runtime.cpp
rmdir src/parallel 2>/dev/null || true

git_mv src/aux_state/engine/shared/shared_query_engine.cpp src/runtime/catalog/shared_query_engine.cpp
git_mv src/aux_state/engine/duckdb/duckdb_runtime.cpp src/runtime/duckdb/duckdb_runtime.cpp

git_mv src/aux_state/engine/postgres/postgres_scan_adapter.cpp src/runtime/postgres/scan_adapter.cpp
git_mv src/aux_state/engine/postgres/postgres_kernel_executor.cpp src/runtime/postgres/kernel_executor.cpp
git_mv src/aux_state/engine/postgres/postgres_bind_helper.cpp src/runtime/postgres/bind_helper.cpp
git_mv src/aux_state/engine/postgres/postgres_extension.cpp src/runtime/postgres/extension.cpp
git_mv src/aux_state/engine/postgres/postgres_query_functions.cpp src/runtime/postgres/query_functions.cpp
git_mv src/aux_state/engine/postgres/README.md src/runtime/postgres/README.md
git_mv src/aux_state/engine/postgres/accelerator_codegen_postgres--0.1.sql src/runtime/postgres/accelerator_codegen_postgres--0.1.sql
git_mv src/aux_state/engine/postgres/accelerator_codegen_postgres.control src/runtime/postgres/accelerator_codegen_postgres.control

# --- stateful scaffolding ---
git_mv src/aux_state/core src/stateful/core
git_mv src/aux_state/maintenance src/stateful/maintenance
git_mv src/aux_state/partitioned_state_store.cpp src/stateful/state/partitioned_state_store.cpp
git_mv src/aux_state/invalidation_plan.cpp src/stateful/state/invalidation_plan.cpp

git_mv src/aux_state/engine/duckdb/duckdb_aux_backend.cpp src/stateful/duckdb/duckdb_aux_backend.cpp
git_mv src/aux_state/engine/duckdb/duckdb_aux_table_functions.cpp src/stateful/duckdb/duckdb_aux_table_functions.cpp
git_mv src/aux_state/engine/duckdb/duckdb_state_store.cpp src/stateful/duckdb/duckdb_state_store.cpp

git_mv src/aux_state/engine/postgres/postgres_aux_backend.cpp src/stateful/postgres/postgres_aux_backend.cpp
git_mv src/aux_state/engine/postgres/postgres_mutation_capture.cpp src/stateful/postgres/postgres_mutation_capture.cpp
git_mv src/aux_state/engine/postgres/postgres_state_store.cpp src/stateful/postgres/postgres_state_store.cpp
git_mv src/aux_state/engine/postgres/postgres_xact_callbacks.cpp src/stateful/postgres/postgres_xact_callbacks.cpp

# --- harness ---
git_mv src/aux_state/harness/differential.cpp src/harness/differential.cpp
git_mv src/aux_state/harness/cost_calibration.cpp src/harness/cost_calibration.cpp

# --- generated query-specific (postgres + catalog) ---
git_mv src/aux_state/engine/shared/query_catalog.cpp src/generated/catalog/query_catalog.cpp
git_mv src/aux_state/engine/postgres/postgres_kernel_runner.cpp src/generated/postgres/kernel_runner.cpp
git_mv src/aux_state/engine/postgres/postgres_kernel_runner_remaining.cpp src/generated/postgres/kernel_runner_remaining.cpp
git_mv src/aux_state/engine/postgres/postgres_runtime.cpp src/generated/postgres/runtime.cpp

# --- generated aux modules ---
move_aux_common() {
  local workload="$1"
  local file="$2"
  local q
  q="$(echo "$file" | sed -E 's/.*_q([0-9]+)_common\.cpp/\1/')"
  mkdir -p "src/generated/${workload}/q${q}/aux"
  git_mv "src/aux_state/query/${workload}/${file}" "src/generated/${workload}/q${q}/aux/module.cpp"
}

for f in tpch_q1_common.cpp tpch_q3_common.cpp tpch_q6_common.cpp tpch_q10_common.cpp tpch_q12_common.cpp tpch_q14_common.cpp; do
  move_aux_common tpch "$f"
done

for f in sec_edgar_q1_common.cpp sec_edgar_q2_common.cpp sec_edgar_q3_common.cpp sec_edgar_q4_common.cpp sec_edgar_q6_common.cpp sec_edgar_q24_common.cpp; do
  move_aux_common sec_edgar "$f"
done

move_aux_finalize() {
  local workload="$1"
  local src="$2"
  local q="$3"
  mkdir -p "src/generated/${workload}/q${q}/aux"
  git_mv "$src" "src/generated/${workload}/q${q}/aux/finalize.cpp"
}

move_aux_finalize tpch src/aux_state/query/tpch/q1/tpch_q1_finalize.cpp 1
move_aux_finalize tpch src/aux_state/query/tpch/q6/tpch_q6_finalize.cpp 6
move_aux_finalize sec_edgar src/aux_state/query/sec_edgar/q1/sec_edgar_q1_finalize.cpp 1
move_aux_finalize sec_edgar src/aux_state/query/sec_edgar/q2/sec_edgar_q2_finalize.cpp 2
move_aux_finalize sec_edgar src/aux_state/query/sec_edgar/q3/sec_edgar_q3_finalize.cpp 3
move_aux_finalize sec_edgar src/aux_state/query/sec_edgar/q4/sec_edgar_q4_finalize.cpp 4
move_aux_finalize sec_edgar src/aux_state/query/sec_edgar/q6/sec_edgar_q6_finalize.cpp 6
move_aux_finalize sec_edgar src/aux_state/query/sec_edgar/q24/sec_edgar_q24_finalize.cpp 24

# --- generated base kernels: tpch ---
for src in src/generated/tpch/single-threaded/*; do
  base="$(basename "$src")"
  if [[ "$base" =~ ^q([0-9]+)_(kernel|package|fused_scan)\.cpp$ ]]; then
    q="${BASH_REMATCH[1]}"
    kind="${BASH_REMATCH[2]}"
    mkdir -p "src/generated/tpch/q${q}/base/single-threaded"
    git_mv "$src" "src/generated/tpch/q${q}/base/single-threaded/${kind}.cpp"
  fi
done

for src in src/generated/tpch/parallel/*; do
  base="$(basename "$src")"
  if [[ "$base" =~ ^q([0-9]+)_parallel_(kernel|package|aligned_agg)\.cpp$ ]]; then
    q="${BASH_REMATCH[1]}"
    kind="${BASH_REMATCH[2]}"
    mkdir -p "src/generated/tpch/q${q}/base/parallel"
    git_mv "$src" "src/generated/tpch/q${q}/base/parallel/${kind}.cpp"
  fi
done

rmdir src/generated/tpch/single-threaded src/generated/tpch/parallel 2>/dev/null || true

# --- generated base kernels: sec_edgar ---
for src in src/generated/sec_edgar/single-threaded/*; do
  base="$(basename "$src")"
  if [[ "$base" =~ ^q([0-9]+)_(kernel|package)\.cpp$ ]]; then
    q="${BASH_REMATCH[1]}"
    kind="${BASH_REMATCH[2]}"
    mkdir -p "src/generated/sec_edgar/q${q}/base/single-threaded"
    git_mv "$src" "src/generated/sec_edgar/q${q}/base/single-threaded/${kind}.cpp"
  fi
done

rmdir src/generated/sec_edgar/single-threaded 2>/dev/null || true
git_mv src/generated/sec_edgar/sec_edgar_query_runtime.cpp src/generated/sec_edgar/runtime.cpp

# cleanup empty dirs
find src/aux_state -type d -empty -delete 2>/dev/null || true
rm -rf src/aux_state 2>/dev/null || true

echo "src reorg moves complete"
