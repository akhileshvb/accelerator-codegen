// Compare forced ApplyDelta RF maintenance vs full rebuild on TPC-H.
// Env:
//   ACCEL_TPCH_DB          path to duckdb (use a writable copy)
//   ACCEL_RF_COMPARE_QUERY comma list, default 6,14,1,18
//   ACCEL_RF_COMPARE_ORDERS comma list of RF order-chunk sizes
//                           default 64,256,1024,4096,16384,30000
//   ACCEL_RF_COMPARE_KINDS  rf2,rf1 or both (default rf2,rf1)
// Official RF is ~0.1% of SF orders (SF20 ≈ 30k orders).

#include "eval/tests/test_support/db_paths.hpp"
#include "eval/harness/tpch_rf.hpp"
#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "backends/duckdb/api/register.hpp"
#include "catalog/query_catalog.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "duckdb.hpp"

namespace {
namespace catalog = accelerator_codegen::aux_state::engine;
using catalog::duckdb::DuckdbRuntime;
using Clock = std::chrono::steady_clock;

void Require(bool ok, const std::string &msg) {
  if (!ok) {
    throw std::runtime_error(msg);
  }
}

std::string GetEnvString(const char *name, const std::string &fallback) {
  const char *v = std::getenv(name);
  return (v && *v) ? std::string(v) : fallback;
}

std::vector<int> ParseIntList(const std::string &value) {
  std::vector<int> out;
  std::stringstream ss(value);
  std::string item;
  while (std::getline(ss, item, ',')) {
    if (!item.empty()) {
      out.push_back(std::stoi(item));
    }
  }
  return out;
}

std::vector<std::string> ParseStringList(const std::string &value) {
  std::vector<std::string> out;
  std::stringstream ss(value);
  std::string item;
  while (std::getline(ss, item, ',')) {
    if (!item.empty()) {
      out.push_back(item);
    }
  }
  return out;
}

void SetForceAction(const char *action) {
  if (action == nullptr || !*action) {
    unsetenv("ACCEL_FORCE_MAINTENANCE_ACTION");
  } else {
    setenv("ACCEL_FORCE_MAINTENANCE_ACTION", action, 1);
  }
}

double MsSince(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start)
      .count();
}

int64_t CountOrders(duckdb::Connection &conn) {
  auto result = conn.Query("SELECT COUNT(*)::BIGINT FROM orders");
  Require(result && !result->HasError(), "count orders failed");
  return result->GetValue(0, 0).GetValue<int64_t>();
}

} // namespace

int main() {
  const std::string db_path = accelerator_codegen::test_support::TpchDbPath();
  const auto queries =
      ParseIntList(GetEnvString("ACCEL_RF_COMPARE_QUERY", "6,14,1,18"));
  const auto order_sizes = ParseIntList(
      GetEnvString("ACCEL_RF_COMPARE_ORDERS", "64,256,1024,4096,16384,30000"));
  const auto kinds =
      ParseStringList(GetEnvString("ACCEL_RF_COMPARE_KINDS", "rf2,rf1"));

  std::cout << "db=" << db_path << "\n";
  std::cout
      << "query,orders_in_db,order_chunk,rf_kind,write_seq,force_action,"
         "phase,wall_ms,valid_partitions,notes\n"
      << std::flush;

  for (const int query_id : queries) {
    if (query_id == 11) {
      std::cerr << "skip q11 (RF noop)\n";
      continue;
    }
    const auto &entry = catalog::LookupSharedQuery("tpch", query_id);
    duckdb::DuckDB database(db_path.c_str());
    duckdb::Connection conn(database);
    Require(conn.Query("PRAGMA threads=1") != nullptr, "pragma failed");
    accelerator_codegen::RegisterAcceleratorFunctions(conn);

    const int64_t orders_in_db = CountOrders(conn);
    DuckdbRuntime runtime(conn, entry, /*expected_queries_before_write=*/10.0);

    SetForceAction(nullptr);
    auto t0 = Clock::now();
    runtime.Build();
    const double full_build_ms = MsSince(t0);
    std::cout << query_id << "," << orders_in_db << ",0,n/a,-1,n/a,full_build,"
              << full_build_ms << "," << runtime.StoreDiagnosticsSnapshot().valid_partitions
              << ",baseline_rebuild\n"
              << std::flush;

    auto next_seq_for_kind = [](int &write_seq,
                                const std::string &kind) -> int {
      while (true) {
        const int seq = write_seq++;
        const bool even = (seq % 2) == 0;
        if (kind == "rf2" && even) {
          return seq;
        }
        if (kind == "rf1" && !even) {
          return seq;
        }
        if (kind != "rf1" && kind != "rf2") {
          throw std::runtime_error("unknown rf kind: " + kind);
        }
      }
    };

    int write_seq = 0;
    for (const int order_count : order_sizes) {
      if (order_count <= 0) {
        continue;
      }
      if (static_cast<int64_t>(order_count) > orders_in_db / 4) {
        std::cerr << "q" << query_id << " skip orders=" << order_count
                  << " (too large vs remaining DB)\n";
        continue;
      }
      for (const auto &kind : kinds) {
        // --- Forced incremental ---
        const int delta_seq = next_seq_for_kind(write_seq, kind);
        SetForceAction("ApplyDelta");
        t0 = Clock::now();
        accelerator_codegen::aux_state::harness::ApplyTpchRfChunk(conn, runtime, delta_seq, order_count);
        const double delta_ms = MsSince(t0);
        const auto delta_valid =
            runtime.StoreDiagnosticsSnapshot().valid_partitions;
        std::cout << query_id << "," << orders_in_db << "," << order_count
                  << "," << kind << "," << delta_seq << ",ApplyDelta,apply_rf,"
                  << delta_ms << "," << delta_valid
                  << ",sql+forced_ApplyDelta\n"
                  << std::flush;

        // --- Forced rebuild path: RF SQL + invalidate, then Build ---
        const int rebuild_seq = next_seq_for_kind(write_seq, kind);
        SetForceAction("RebuildAll");
        t0 = Clock::now();
        accelerator_codegen::aux_state::harness::ApplyTpchRfChunk(conn, runtime, rebuild_seq, order_count);
        const double rf_invalidate_ms = MsSince(t0);
        const auto after_inv =
            runtime.StoreDiagnosticsSnapshot().valid_partitions;
        std::cout << query_id << "," << orders_in_db << "," << order_count
                  << "," << kind << "," << rebuild_seq
                  << ",RebuildAll,apply_rf_invalidate," << rf_invalidate_ms
                  << "," << after_inv << ",sql+forced_RebuildAll\n"
                  << std::flush;

        t0 = Clock::now();
        runtime.Build();
        const double rebuild_ms = MsSince(t0);
        const auto after_rebuild =
            runtime.StoreDiagnosticsSnapshot().valid_partitions;
        std::cout << query_id << "," << orders_in_db << "," << order_count
                  << "," << kind << "," << rebuild_seq
                  << ",RebuildAll,rebuild_after_rf," << rebuild_ms << ","
                  << after_rebuild << ",full_Build_after_invalidate\n"
                  << std::flush;

        const double rebuild_path_ms = rf_invalidate_ms + rebuild_ms;
        std::cout << query_id << "," << orders_in_db << "," << order_count
                  << "," << kind << "," << rebuild_seq
                  << ",RebuildAll,rf_plus_rebuild," << rebuild_path_ms << ","
                  << after_rebuild << ",invalidate_plus_Build\n"
                  << std::flush;

        std::cout << query_id << "," << orders_in_db << "," << order_count
                  << "," << kind << "," << rebuild_seq
                  << ",compare,delta_vs_rebuild_ratio,"
                  << (rebuild_path_ms > 0.0 ? delta_ms / rebuild_path_ms : -1.0)
                  << "," << after_rebuild
                  << ",ratio<1 means ApplyDelta faster\n"
                  << std::flush;
      }
    }
  }

  SetForceAction(nullptr);
  return 0;
}
