#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

mkdir -p \
  include/accelerator_codegen/runtime/duckdb \
  include/accelerator_codegen/runtime/parallel \
  include/accelerator_codegen/runtime/catalog \
  include/accelerator_codegen/runtime/postgres \
  include/accelerator_codegen/stateful/core \
  include/accelerator_codegen/stateful/maintenance \
  include/accelerator_codegen/stateful/state \
  include/accelerator_codegen/stateful/duckdb \
  include/accelerator_codegen/stateful/postgres \
  include/accelerator_codegen/harness \
  include/accelerator_codegen/generated/catalog \
  include/accelerator_codegen/generated/postgres

git_mv() {
  if [[ -e "$1" ]]; then
    mkdir -p "$(dirname "$2")"
    git mv "$1" "$2"
  fi
}

# runtime headers (from top-level + engine)
for h in kernel.hpp query_runtime.hpp duckdb_scan_adapter.hpp chunk_source.hpp function_lifecycle.hpp register.hpp; do
  git_mv "include/accelerator_codegen/${h}" "include/accelerator_codegen/runtime/duckdb/${h}"
done
git_mv include/accelerator_codegen/parallel include/accelerator_codegen/runtime/parallel_tmp
mkdir -p include/accelerator_codegen/runtime/parallel
git_mv include/accelerator_codegen/runtime/parallel_tmp/parallel/* include/accelerator_codegen/runtime/parallel/ 2>/dev/null || \
  git_mv include/accelerator_codegen/runtime/parallel_tmp/* include/accelerator_codegen/runtime/parallel/
rmdir include/accelerator_codegen/runtime/parallel_tmp/parallel 2>/dev/null || true
rmdir include/accelerator_codegen/runtime/parallel_tmp 2>/dev/null || true

git_mv include/accelerator_codegen/generated/catalog/query_catalog.hpp include/accelerator_codegen/generated/catalog/query_catalog.hpp
git_mv include/accelerator_codegen/runtime/catalog/shared_query_engine.hpp include/accelerator_codegen/runtime/catalog/shared_query_engine.hpp

git_mv include/accelerator_codegen/runtime/duckdb/duckdb_runtime.hpp include/accelerator_codegen/runtime/duckdb/duckdb_runtime.hpp

for h in postgres_kernel_executor.hpp postgres_kernel_dispatch.hpp postgres_scan_adapter.hpp postgres_bind_helper.hpp postgres_row_writer.hpp postgres_runtime.hpp postgres_kernel_runner.hpp; do
  case "$h" in
    postgres_scan_adapter.hpp) dst=scan_adapter.hpp ;;
    postgres_kernel_executor.hpp) dst=kernel_executor.hpp ;;
    postgres_bind_helper.hpp) dst=bind_helper.hpp ;;
    postgres_row_writer.hpp) dst=row_writer.hpp ;;
    postgres_runtime.hpp) dst=runtime.hpp ;;
    postgres_kernel_runner.hpp) dst=kernel_runner.hpp ;;
    *) dst="$h" ;;
  esac
  if [[ "$h" == postgres_runtime.hpp || "$h" == postgres_kernel_runner.hpp ]]; then
    git_mv "include/accelerator_codegen/aux_state/engine/postgres/${h}" "include/accelerator_codegen/generated/postgres/${dst}"
  else
    git_mv "include/accelerator_codegen/aux_state/engine/postgres/${h}" "include/accelerator_codegen/runtime/postgres/${dst}"
  fi
done

# stateful
git_mv include/accelerator_codegen/aux_state/core include/accelerator_codegen/stateful/core
git_mv include/accelerator_codegen/aux_state/maintenance include/accelerator_codegen/stateful/maintenance
git_mv include/accelerator_codegen/stateful/state/partitioned_state_store.hpp include/accelerator_codegen/stateful/state/partitioned_state_store.hpp
git_mv include/accelerator_codegen/stateful/state/invalidation_plan.hpp include/accelerator_codegen/stateful/state/invalidation_plan.hpp
git_mv include/accelerator_codegen/stateful/state/partition_key.hpp include/accelerator_codegen/stateful/state/partition_key.hpp
git_mv include/accelerator_codegen/stateful/core/incremental_txn_buffer.hpp include/accelerator_codegen/stateful/core/incremental_txn_buffer.hpp

git_mv include/accelerator_codegen/stateful/duckdb/duckdb_aux_backend.hpp include/accelerator_codegen/stateful/duckdb/duckdb_aux_backend.hpp
git_mv include/accelerator_codegen/stateful/duckdb/duckdb_state_store.hpp include/accelerator_codegen/stateful/duckdb/duckdb_state_store.hpp
git_mv include/accelerator_codegen/stateful/postgres/postgres_aux_backend.hpp include/accelerator_codegen/stateful/postgres/postgres_aux_backend.hpp
git_mv include/accelerator_codegen/stateful/postgres/postgres_state_store.hpp include/accelerator_codegen/stateful/postgres/postgres_state_store.hpp
git_mv include/accelerator_codegen/stateful/postgres/postgres_mutation_capture.hpp include/accelerator_codegen/stateful/postgres/postgres_mutation_capture.hpp
git_mv include/accelerator_codegen/stateful/postgres/postgres_xact_callbacks.hpp include/accelerator_codegen/stateful/postgres/postgres_xact_callbacks.hpp

# harness
git_mv include/accelerator_codegen/aux_state/harness include/accelerator_codegen/harness_tmp
mkdir -p include/accelerator_codegen/harness
git_mv include/accelerator_codegen/harness_tmp/harness/* include/accelerator_codegen/harness/ 2>/dev/null || \
  git_mv include/accelerator_codegen/harness_tmp/* include/accelerator_codegen/harness/
rmdir include/accelerator_codegen/harness_tmp/harness 2>/dev/null || true
rmdir include/accelerator_codegen/harness_tmp 2>/dev/null || true

# generated aux module headers
move_aux_header() {
  local workload="$1"
  local file="$2"
  local q
  q="$(echo "$file" | sed -E 's/.*_q([0-9]+)_common\.hpp/\1/')"
  mkdir -p "include/accelerator_codegen/generated/${workload}/q${q}/aux"
  git_mv "include/accelerator_codegen/aux_state/query/${workload}/${file}" \
    "include/accelerator_codegen/generated/${workload}/q${q}/aux/module.hpp"
}

for f in tpch_q1_common.hpp tpch_q3_common.hpp tpch_q6_common.hpp tpch_q10_common.hpp tpch_q12_common.hpp tpch_q14_common.hpp; do
  move_aux_header tpch "$f"
done
for f in sec_edgar_q1_common.hpp sec_edgar_q2_common.hpp sec_edgar_q3_common.hpp sec_edgar_q4_common.hpp sec_edgar_q6_common.hpp sec_edgar_q24_common.hpp; do
  move_aux_header sec_edgar "$f"
done

git_mv include/accelerator_codegen/aux_state/query/README.md include/accelerator_codegen/generated/README.md

find include/accelerator_codegen/aux_state -type d -empty -delete 2>/dev/null || true
rm -rf include/accelerator_codegen/aux_state 2>/dev/null || true

echo "include reorg moves complete"
