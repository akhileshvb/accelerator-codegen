#pragma once

#include "accelerator_codegen/stateful/core/aux_types.hpp"
#include "catalog/query_catalog.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace accelerator_codegen::benchmarks::aux_sweep {

// One TPC-H substitution instance: structured args + matching DuckDB / kernel SQL.
struct TpchArgVariant {
  accelerator_codegen::aux_state::core::QueryArguments arguments;
  std::string base_sql;
  std::string kernel_sql;
};

// ACCEL_BENCH_VARY_ARGS: default on. Set 0/false/off to pin catalog defaults.
bool VaryArgsEnabled();

// ACCEL_BENCH_ARG_SEED (default 1) and ACCEL_BENCH_ARG_POOL (default 32).
uint64_t ArgSeed();
std::size_t ArgPoolSize();

// Sample one substitution set for query_id using a deterministic stream index.
accelerator_codegen::aux_state::core::QueryArguments
SampleTpchArgs(int query_id, uint64_t stream_index);

std::string FormatTpchBaseSql(
    int query_id,
    const accelerator_codegen::aux_state::core::QueryArguments &arguments);
std::string FormatTpchKernelSql(
    int query_id,
    const accelerator_codegen::aux_state::core::QueryArguments &arguments);

// Pool of variants. When vary is disabled, returns a single catalog-default entry.
std::vector<TpchArgVariant> BuildTpchArgPool(
    int query_id,
    const accelerator_codegen::aux_state::engine::SharedQueryCatalogEntry &entry);

} // namespace accelerator_codegen::benchmarks::aux_sweep
