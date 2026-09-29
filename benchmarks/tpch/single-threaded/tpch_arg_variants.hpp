#pragma once

#include "accelerator_codegen/stateful/core/aux_types.hpp"

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
  std::string aux_sql;
};

// Optional catalog literals used as the first (pinned) variant.
struct TpchArgCatalogSeed {
  accelerator_codegen::aux_state::core::QueryArguments arguments;
  const char *base_sql = nullptr;
  const char *kernel_sql = nullptr;
  const char *aux_sql = nullptr;
};

// ACCEL_BENCH_VARY_ARGS: default on. Set 0/false/off to pin catalog defaults.
bool VaryArgsEnabled();

// ACCEL_BENCH_ARG_SEED (default 1) and ACCEL_BENCH_ARG_POOL (default 32).
uint64_t ArgSeed();
std::size_t ArgPoolSize();

// Official TPC-H default substitutions used by the generated catalogs.
accelerator_codegen::aux_state::core::QueryArguments
CatalogDefaultTpchArgs(int query_id);

// Sample one substitution set for query_id using a deterministic stream index.
accelerator_codegen::aux_state::core::QueryArguments
SampleTpchArgs(int query_id, uint64_t stream_index);

std::string FormatTpchBaseSql(
    int query_id,
    const accelerator_codegen::aux_state::core::QueryArguments &arguments);
std::string FormatTpchKernelSql(
    int query_id,
    const accelerator_codegen::aux_state::core::QueryArguments &arguments);
std::string FormatTpchAuxSql(
    int query_id,
    const accelerator_codegen::aux_state::core::QueryArguments &arguments);

std::string FormatTpchSqlString(const std::string &text);
std::string FormatTpchDateLiteral(
    const accelerator_codegen::aux_state::core::AuxDate &date);

// Invalidating probe SELECT whose columns match the DuckDB catalog build_probe
// for this query, with substitution parameters filled in.
std::string FormatTpchProbeSelectSql(
    int query_id,
    const accelerator_codegen::aux_state::core::QueryArguments &arguments);

// Catalog default first, then unique sampled substitutions up to `samples`.
// If seed is null, CatalogDefaultTpchArgs + FormatTpch* is used for variant 0.
std::vector<TpchArgVariant> BuildTpchDifferentialArgPool(
    int query_id, std::size_t samples,
    const TpchArgCatalogSeed *seed = nullptr);

// Pool of variants. When vary is disabled, returns a single catalog-default entry.
std::vector<TpchArgVariant> BuildTpchArgPool(
    int query_id, const TpchArgCatalogSeed &seed);

} // namespace accelerator_codegen::benchmarks::aux_sweep
