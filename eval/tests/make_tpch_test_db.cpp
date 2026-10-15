// Builds the small TPC-H test database (workloads/tpch/test_db_subset.sql)
// from a TPC-H source database: what scripts/create_tpch_test_db.py does,
// without Python. ctest runs it as a fixture so the tests that need the subset
// get a fresh, private copy in the build dir instead of a shared file that may
// be stale or modified by other runs.
//
// usage: make_tpch_test_db <test_db_subset.sql> <dest.duckdb> [source.duckdb]
//   source defaults to ACCEL_TPCH_DB / the shared SF1 (test_support::TpchDbPath),
//   attached read-only.
#include "eval/tests/test_support/db_paths.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "duckdb.hpp"

namespace {

void Require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::string ReadFile(const std::string &path) {
  std::ifstream in(path);
  Require(in.good(), "cannot read " + path);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

std::string SqlQuote(const std::string &text) {
  std::string out = "'";
  for (char c : text) {
    out += c;
    if (c == '\'') {
      out += '\'';
    }
  }
  return out + "'";
}

} // namespace

int main(int argc, char **argv) {
  namespace fs = std::filesystem;
  try {
    Require(argc == 3 || argc == 4,
            "usage: make_tpch_test_db <test_db_subset.sql> <dest.duckdb> [source.duckdb]");
    const std::string sql = ReadFile(argv[1]);
    const fs::path dest = argv[2];
    const std::string source =
        argc == 4 ? std::string(argv[3]) : accelerator_codegen::test_support::TpchDbPath();
    Require(fs::exists(source), "missing TPC-H source DB: " + source + " (set ACCEL_TPCH_DB)");

    if (!dest.parent_path().empty()) {
      fs::create_directories(dest.parent_path());
    }
    // Build next to dest, then rename: readers never see a half-built file.
    const fs::path tmp = dest.string() + ".tmp" + std::to_string(::getpid());
    fs::remove(tmp);
    fs::remove(tmp.string() + ".wal");
    {
      duckdb::DuckDB database(tmp.string());
      duckdb::Connection connection(database);
      auto attach = connection.Query("ATTACH " + SqlQuote(source) + " AS src (READ_ONLY)");
      Require(!attach->HasError(), "attach failed: " + attach->GetError());
      auto built = connection.Query(sql);
      Require(!built->HasError(), "subset SQL failed: " + built->GetError());
      auto detach = connection.Query("DETACH src");
      Require(!detach->HasError(), "detach failed: " + detach->GetError());
      auto checkpoint = connection.Query("CHECKPOINT");
      Require(!checkpoint->HasError(), "checkpoint failed: " + checkpoint->GetError());
      for (const char *table : {"region", "nation", "customer", "supplier", "part", "partsupp", "orders", "lineitem"}) {
        auto count = connection.Query(std::string("SELECT COUNT(*) FROM ") + table);
        Require(!count->HasError(), count->GetError());
        std::cerr << "  " << table << ": " << count->GetValue(0, 0).ToString() << " rows\n";
      }
    }
    fs::remove(tmp.string() + ".wal");
    fs::remove(dest.string() + ".wal");
    fs::rename(tmp, dest);
    std::cerr << "wrote " << dest.string() << " from " << source << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "make_tpch_test_db failed: " << error.what() << '\n';
    return 1;
  }
}
