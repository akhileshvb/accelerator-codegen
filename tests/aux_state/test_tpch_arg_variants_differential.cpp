#include "accelerator_codegen/harness/differential.hpp"
#include "accelerator_codegen/runtime/duckdb/duckdb_runtime.hpp"
#include "accelerator_codegen/stateful/core/aux_maintenance_policy.hpp"
#include "catalog/query_catalog.hpp"
#include "tpch_arg_variants.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "duckdb.hpp"

namespace {

namespace catalog = accelerator_codegen::aux_state::engine;
namespace harness = accelerator_codegen::aux_state::harness;
namespace aux_sweep = accelerator_codegen::benchmarks::aux_sweep;
using catalog::duckdb::DuckdbRuntime;

constexpr int kDefaultReadSamples = 4;
constexpr int kDefaultMaintSamples = 2;
constexpr const char *kMaintActions[] = {"", "ApplyDelta", "RebuildPartition",
                                         "RebuildAll"};

void Require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

int EnvInt(const char *name, int fallback) {
  const char *value = std::getenv(name);
  if (!value || !*value) {
    return fallback;
  }
  return std::max(1, std::stoi(value));
}

void CopyDb(const std::filesystem::path &src, const std::filesystem::path &dst) {
  Require(std::filesystem::exists(src), "missing source DB: " + src.string());
  std::filesystem::create_directories(
      dst.parent_path().empty() ? std::filesystem::path(".") : dst.parent_path());
  std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing);
}

std::vector<int> QueryList() {
  if (const char *raw = std::getenv("ACCEL_DUCKDB_DIFF_QUERIES"); raw && *raw) {
    std::vector<int> out;
    std::string token;
    for (char c : std::string(raw) + " ") {
      if (c == ' ' || c == ',' || c == '\n' || c == '\t') {
        if (!token.empty()) {
          out.push_back(std::stoi(token));
          token.clear();
        }
      } else {
        token += c;
      }
    }
    Require(!out.empty(), "ACCEL_DUCKDB_DIFF_QUERIES parsed empty");
    return out;
  }
  return catalog::SharedQueriesForWorkload("tpch");
}

struct BoundVariant {
  aux_sweep::TpchArgVariant variant;
  std::string probe_sql;
  catalog::SharedQueryCatalogEntry entry;
};

BoundVariant BindVariant(const catalog::SharedQueryCatalogEntry &base,
                         aux_sweep::TpchArgVariant variant) {
  BoundVariant bound;
  bound.entry = base;
  bound.variant = std::move(variant);
  bound.probe_sql = aux_sweep::FormatTpchProbeSelectSql(base.query_id,
                                                       bound.variant.arguments);
  bound.entry.base_sql = bound.variant.base_sql.c_str();
  bound.entry.duckdb_aux_sql = bound.variant.aux_sql.c_str();
  bound.entry.postgres_base_kernel_sql = bound.variant.kernel_sql.c_str();
  bound.entry.postgres_aux_sql = bound.variant.aux_sql.c_str();
  bound.entry.probe_select_sql = bound.probe_sql.c_str();
  bound.entry.benchmark_arguments.values = bound.variant.arguments.values;
  return bound;
}

void RefreshVariant(BoundVariant &bound, aux_sweep::TpchArgVariant variant) {
  bound.variant = std::move(variant);
  bound.probe_sql = aux_sweep::FormatTpchProbeSelectSql(
      bound.entry.query_id, bound.variant.arguments);
  bound.entry.base_sql = bound.variant.base_sql.c_str();
  bound.entry.duckdb_aux_sql = bound.variant.aux_sql.c_str();
  bound.entry.postgres_base_kernel_sql = bound.variant.kernel_sql.c_str();
  bound.entry.postgres_aux_sql = bound.variant.aux_sql.c_str();
  bound.entry.probe_select_sql = bound.probe_sql.c_str();
  bound.entry.benchmark_arguments.values = bound.variant.arguments.values;
}

void SetForceAction(const char *action) {
  if (action && *action) {
    setenv("ACCEL_FORCE_MAINTENANCE_ACTION", action, 1);
    accelerator_codegen::aux_state::core::SetForcedMaintenanceActionName(action);
  } else {
    unsetenv("ACCEL_FORCE_MAINTENANCE_ACTION");
    accelerator_codegen::aux_state::core::SetForcedMaintenanceActionName("");
  }
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (argc < 2) {
      throw std::runtime_error(
          "usage: test_tpch_arg_variants_differential <tpch.duckdb> "
          "[read_samples] [maint_samples]");
    }
    const std::string db_path = argv[1];
    const int read_samples =
        argc >= 3 ? std::max(1, std::stoi(argv[2]))
                  : EnvInt("ACCEL_ARG_SAMPLES", kDefaultReadSamples);
    const int maint_samples =
        argc >= 4 ? std::max(1, std::stoi(argv[3]))
                  : EnvInt("ACCEL_ARG_MAINT_SAMPLES", kDefaultMaintSamples);

    setenv("ACCEL_BENCH_VARY_ARGS", "1", 0);
    setenv("ACCEL_BENCH_ARG_SEED", "42", 0);

    const auto queries = QueryList();
    for (const int query_id : queries) {
      const auto &entry = catalog::LookupSharedQuery("tpch", query_id);
      aux_sweep::TpchArgCatalogSeed seed;
      seed.arguments = catalog::ToCoreArguments(entry.benchmark_arguments);
      seed.base_sql = entry.base_sql;
      seed.kernel_sql = entry.postgres_base_kernel_sql;
      seed.aux_sql = entry.duckdb_aux_sql;
      auto pool = aux_sweep::BuildTpchDifferentialArgPool(
          query_id, static_cast<std::size_t>(read_samples), &seed);
      Require(!pool.empty(), "empty arg pool for Q" + std::to_string(query_id));

      std::cerr << "arg-variant differential: tpch Q" << query_id
                << " read_samples=" << pool.size() << '\n'
                << std::flush;

      {
        duckdb::DuckDB database(db_path.c_str());
        duckdb::Connection connection(database);
        Require(connection.Query("PRAGMA threads=1") != nullptr, "pragma failed");
        DuckdbRuntime runtime(connection, entry, 10.0);
        runtime.Build();
        for (std::size_t i = 0; i < pool.size(); ++i) {
          const auto &variant = pool[i];
          try {
            harness::RequireSharedMatchesSql(connection, runtime, entry,
                                             variant.arguments, variant.base_sql);
          } catch (const std::exception &ex) {
            throw std::runtime_error(std::string(ex.what()) + " (Q" +
                                     std::to_string(query_id) +
                                     " read variant=" + std::to_string(i) +
                                     " sql=" + variant.kernel_sql + ")");
          }
        }
      }
      std::cerr << "PASS read arg-variants tpch Q" << query_id << '\n'
                << std::flush;

      const std::filesystem::path work =
          std::filesystem::temp_directory_path() /
          ("tpch_arg_maint_q" + std::to_string(query_id) + ".duckdb");
      CopyDb(db_path, work);

      duckdb::DuckDB work_db(work.string().c_str());
      duckdb::Connection work_conn(work_db);
      Require(work_conn.Query("PRAGMA threads=1") != nullptr, "pragma failed");

      const std::size_t maint_n =
          std::min(pool.size(), static_cast<std::size_t>(maint_samples));
      auto bound = BindVariant(entry, pool[0]);
      DuckdbRuntime runtime(work_conn, bound.entry, 10.0);

      for (std::size_t i = 0; i < maint_n; ++i) {
        RefreshVariant(bound, pool[i]);
        for (const char *action : kMaintActions) {
          SetForceAction(action);
          const std::string force_label =
              (action && *action) ? action : "policy";
          try {
            // Rescan from the restored work copy so each forced action starts
            // from catalog state instead of a prior overlay/rebuild leftover.
            runtime.Build();
            harness::RequireSharedMatchesBase(work_conn, runtime, bound.entry);
            harness::ApplyInvalidatingProbeAndRematch(work_conn, runtime,
                                                      bound.entry);
          } catch (const std::exception &ex) {
            SetForceAction("");
            throw std::runtime_error(
                std::string(ex.what()) + " (Q" + std::to_string(query_id) +
                " maint variant=" + std::to_string(i) + " force=" + force_label +
                " sql=" + bound.variant.kernel_sql + ")");
          }
        }
      }
      SetForceAction("");
      std::cerr << "PASS maint arg-variants tpch Q" << query_id
                << " variants=" << maint_n << '\n'
                << std::flush;
    }

    std::cerr << "PASS all arg-variant differentials\n";
    return 0;
  } catch (const std::exception &ex) {
    std::cerr << "test_tpch_arg_variants_differential failed: " << ex.what()
              << '\n';
    return 1;
  }
}
