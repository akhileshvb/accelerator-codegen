#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "eval/harness/differential.hpp"
#include "catalog/query_catalog.hpp"

namespace {

namespace fs = std::filesystem;

void Require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

int GetEnvInt(const char *name, int fallback) {
  const char *value = std::getenv(name);
  return value && *value ? std::stoi(value) : fallback;
}

std::vector<int> DefaultQueries() {
  // Shared DuckDB catalog RF-capable aux queries used in SF20 RF sweeps.
  return {1, 3, 6, 9, 10, 12, 14, 17, 18, 21};
}

std::vector<int> ParseQueries(const char *spec) {
  if (!spec || !*spec) {
    return DefaultQueries();
  }
  std::vector<int> out;
  std::string cur;
  for (const char *p = spec;; ++p) {
    if (*p == ',' || *p == '\0') {
      if (!cur.empty()) {
        out.push_back(std::stoi(cur));
        cur.clear();
      }
      if (*p == '\0') {
        break;
      }
    } else if (!std::isspace(static_cast<unsigned char>(*p))) {
      cur.push_back(*p);
    }
  }
  Require(!out.empty(), "ACCEL_TPCH_RF_DIFF_QUERIES is empty");
  return out;
}

void CopyDb(const fs::path &src, const fs::path &dst) {
  Require(fs::exists(src), "missing source DB: " + src.string());
  Require(src != dst, "work DB path must differ from source DB");
  fs::create_directories(dst.parent_path().empty() ? fs::path(".")
                                                   : dst.parent_path());
  fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
}

} // namespace

int main(int argc, char **argv) {
  try {
    Require(argc == 2 || argc == 3,
            "usage: test_shared_tpch_rf_differential <tpch_sf1.duckdb> "
            "[work_copy.duckdb]");
    const fs::path source = argv[1];
    const fs::path work =
        argc == 3 ? fs::path(argv[2])
                  : (fs::temp_directory_path() / "tpch_rf_diff_work.duckdb");
    const int num_chunks = GetEnvInt("ACCEL_TPCH_RF_DIFF_CHUNKS", 4);
    const int order_count = GetEnvInt("ACCEL_TPCH_RF_CHUNK_ORDERS", 1);
    const auto queries = ParseQueries(std::getenv("ACCEL_TPCH_RF_DIFF_QUERIES"));

    std::cerr << "RF differential: source=" << source << " work=" << work
              << " chunks=" << num_chunks << " orders/chunk=" << order_count
              << '\n';

    // ACCEL_TPCH_DIFF_MODE=sql: every write kind as captured SQL, all 22 queries by default
    const char *mode = std::getenv("ACCEL_TPCH_DIFF_MODE");
    if (mode && std::string(mode) == "sql") {
      const char *spec = std::getenv("ACCEL_TPCH_RF_DIFF_QUERIES");
      std::vector<int> all;
      for (int q = 1; q <= 22; ++q)
        all.push_back(q);
      const auto sql_queries = spec && *spec ? queries : all;
      const int rounds = GetEnvInt("ACCEL_TPCH_DIFF_ROUNDS", 2);
      int failed = 0;
      for (const int query_id : sql_queries) {
        const auto &entry = accelerator_codegen::aux_state::engine::LookupSharedQuery("tpch", query_id);
        CopyDb(source, work);
        try {
          accelerator_codegen::aux_state::harness::RunSqlWritesDifferentialCheck(work.string(), entry, rounds);
          std::cerr << "PASS tpch Q" << query_id << '\n';
        } catch (const std::exception &ex) {
          ++failed;
          std::cerr << "FAIL tpch Q" << query_id << ": " << ex.what() << '\n';
        }
      }
      std::cerr << (failed ? "FAILED " : "PASS ") << failed << " of " << sql_queries.size() << '\n';
      return failed ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    for (const int query_id : queries) {
      if (query_id == 11) {
        std::cerr << "SKIP tpch Q11 (RF does not touch partsupp/supplier)\n";
        continue;
      }
      const auto &entry =
          accelerator_codegen::aux_state::engine::LookupSharedQuery("tpch",
                                                                   query_id);
      // Fresh copy per query so earlier RF mutations do not leak.
      CopyDb(source, work);
      std::cerr << "RF differential: tpch Q" << query_id << '\n';
      accelerator_codegen::aux_state::harness::RunRfChunkDifferentialCheck(
          work.string(), entry, num_chunks, order_count);
      std::cerr << "PASS tpch Q" << query_id << '\n';
    }
    std::cerr << "PASS all RF differentials\n";
    return EXIT_SUCCESS;
  } catch (const std::exception &ex) {
    std::cerr << "test_shared_tpch_rf_differential failed: " << ex.what()
              << '\n';
    return EXIT_FAILURE;
  }
}
