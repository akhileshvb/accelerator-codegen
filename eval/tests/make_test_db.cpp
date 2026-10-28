// Builds a benchmark's small test database (workloads/<bench>/test_db_subset.sql) from its source database, for
// any benchmark that has no maker of its own: ctest runs it as the <bench>_test_db fixture, so tests get a fresh,
// private copy in the build dir instead of a shared file that may be stale or modified by other runs.
//
// usage: make_test_db <bench> <test_db_subset.sql> <dest.duckdb> [source.duckdb]
//   source defaults to ACCEL_<BENCH>_DB, attached read-only.
// Exit 77 (ctest skip) when the source is missing or has no rows in any table.
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
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

std::string SourceEnv(std::string bench) {
  std::transform(bench.begin(), bench.end(), bench.begin(), [](unsigned char c) { return std::toupper(c); });
  return "ACCEL_" + bench + "_DB";
}

// Rows of every table of `catalog`, one "  <table>: <rows> rows" line each; the total.
int64_t ReportRows(duckdb::Connection &connection, const std::string &catalog) {
  auto tables = connection.Query("SELECT table_name FROM information_schema.tables WHERE table_catalog = " +
                                 SqlQuote(catalog) + " AND table_type = 'BASE TABLE' ORDER BY table_name");
  Require(!tables->HasError(), tables->GetError());
  int64_t total = 0;
  for (duckdb::idx_t i = 0; i < tables->RowCount(); ++i) {
    const auto table = tables->GetValue(0, i).ToString();
    auto count = connection.Query("SELECT COUNT(*) FROM " + catalog + "." + table);
    Require(!count->HasError(), count->GetError());
    const auto rows = count->GetValue(0, 0).GetValue<int64_t>();
    std::cerr << "  " << table << ": " << rows << " rows\n";
    total += rows;
  }
  return total;
}

} // namespace

int main(int argc, char **argv) {
  namespace fs = std::filesystem;
  try {
    Require(argc == 4 || argc == 5, "usage: make_test_db <bench> <test_db_subset.sql> <dest.duckdb> [source.duckdb]");
    const std::string sql = ReadFile(argv[2]);
    const fs::path dest = argv[3];
    const std::string env = SourceEnv(argv[1]);
    const char *configured = std::getenv(env.c_str());
    const std::string source = argc == 5 ? argv[4] : configured ? configured : "";
    if (source.empty() || !fs::exists(source)) {
      std::cerr << "SKIP: missing source DB " << source << " (set " << env << ")\n";
      return 77;
    }

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
      if (ReportRows(connection, "src") == 0) {
        std::cerr << "SKIP: source DB " << source << " has no rows\n";
        fs::remove(tmp);
        fs::remove(tmp.string() + ".wal");
        return 77;
      }
      auto built = connection.Query(sql);
      Require(!built->HasError(), "subset SQL failed: " + built->GetError());
      auto detach = connection.Query("DETACH src");
      Require(!detach->HasError(), "detach failed: " + detach->GetError());
      auto checkpoint = connection.Query("CHECKPOINT");
      Require(!checkpoint->HasError(), "checkpoint failed: " + checkpoint->GetError());
      auto current = connection.Query("SELECT current_database()");
      Require(!current->HasError(), current->GetError());
      ReportRows(connection, current->GetValue(0, 0).ToString());
    }
    fs::remove(tmp.string() + ".wal");
    fs::remove(dest.string() + ".wal");
    fs::rename(tmp, dest);
    std::cerr << "wrote " << dest.string() << " from " << source << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "make_test_db failed: " << error.what() << '\n';
    return 1;
  }
}
