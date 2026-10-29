#include "backends/duckdb/tests/harness/common.hpp"

#include "backends/duckdb/api/catalog/query_catalog.hpp"
#include "backends/duckdb/api/duckdb_runtime.hpp"

namespace {
namespace h = accelerator_codegen::duckdb_harness;
namespace engine = accelerator_codegen::aux_state::engine;

std::string PrivateCopy(const std::string &source) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto path = h::TempDirectory() /
                    ("accel_harness_" + std::to_string(stamp) + ".duckdb");
  std::filesystem::copy_file(source, path, std::filesystem::copy_options::overwrite_existing);
  const auto wal = source + ".wal";
  if (std::filesystem::exists(wal)) std::filesystem::copy_file(wal, path.string() + ".wal");
  return path.string();
}

std::string ResultFingerprint(duckdb::Connection &connection, const std::string &sql, bool unordered) {
  auto result = h::Query(connection, sql);
  std::vector<std::string> rows;
  for (duckdb::idx_t row = 0; row < result->RowCount(); ++row) {
    std::string cells;
    for (duckdb::idx_t col = 0; col < result->ColumnCount(); ++col)
      cells += result->GetValue(col, row).ToString() + '\x1f';
    rows.push_back(std::move(cells));
  }
  if (unordered) std::sort(rows.begin(), rows.end());
  std::string out;
  for (const auto &row : rows) out += row + '\x1e';
  return out;
}

bool IsRollbackTransaction(const std::filesystem::path &path) {
  std::istringstream input(h::ReadFile(path));
  for (std::string line; std::getline(input, line);) {
    const auto first = line.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) continue;
    return line.compare(first, 10, "-- rollback") == 0;
  }
  return false;
}

std::vector<std::string> ReferenceSqls(const engine::SharedQueryCatalogEntry &entry,
                                       const std::vector<std::vector<std::string>> &tuples) {
  std::vector<std::string> sqls;
  if (tuples.empty()) sqls.push_back(entry.base_sql ? entry.base_sql : "");
  for (const auto &tuple : tuples)
    sqls.push_back(entry.fallback_sql ? h::BindSql(entry.fallback_sql, tuple) : entry.base_sql);
  return sqls;
}

void WriteSuiteReport(bool ok, std::size_t checks, const std::vector<std::uint64_t> &tuple_rows,
                      const std::vector<std::pair<std::string, std::size_t>> &txn_effects,
                      const std::string &error) {
  const char *path = std::getenv("ACCEL_SUITE_REPORT");
  if (!path || !*path) return;
  std::ofstream report(path);
  report << "{\"ok\":" << (ok ? "true" : "false") << ",\"seed\":"
         << (std::getenv("ACCEL_SUITE_SEED") ? std::getenv("ACCEL_SUITE_SEED") : "0")
         << ",\"tuple_rows\":[";
  for (std::size_t i = 0; i < tuple_rows.size(); ++i) report << (i ? "," : "") << tuple_rows[i];
  report << "],\"txn_effects\":[";
  for (std::size_t i = 0; i < txn_effects.size(); ++i) {
    report << (i ? "," : "") << "{\"txn\":\"" << txn_effects[i].first
           << "\",\"changes_answers_of\":" << txn_effects[i].second << '}';
  }
  report << "],\"checks\":" << checks << ",\"mismatches\":[],\"write_errors\":[],\"executes\":"
         << checks << ",\"examples\":[],\"errors\":[";
  if (!error.empty()) {
    report << '"';
    for (char ch : error) {
      if (ch == '"' || ch == '\\') report << '\\';
      if (ch == '\n' || ch == '\r') continue;
      report << ch;
    }
    report << '"';
  }
  report << "],\"phases\":{}}\n";
}

} // namespace

int main(int argc, char **argv) {
  int query = 0;
  try {
    query = h::QueryArgument(argc, argv, "test_duckdb_<benchmark>_differential <query>");
    if (query == 0) return 0;
    const auto &entry = h::Lookup(query);
    const auto db_path = PrivateCopy(h::DbPath());
    std::size_t checks = 0;
    std::vector<std::uint64_t> tuple_rows;
    std::vector<std::pair<std::string, std::size_t>> txn_effects;
    const auto tuples = h::SuiteTuples(query);
    const auto references = ReferenceSqls(entry, tuples);
    try {
      auto database = h::OpenDatabase(db_path);
      auto reader = h::Connect(*database);
      auto writer = h::Connect(*database);
      const char *mode = std::getenv("ACCEL_SUITE_MODE");
      if (mode && std::string(mode) == "validate") {
        for (const auto &sql : references) {
          auto result = h::Query(*reader, sql);
          tuple_rows.push_back(result->RowCount());
          ++checks;
        }
        for (const auto &txn : h::SuiteTransactions()) {
          std::vector<std::string> before;
          for (const auto &sql : references) before.push_back(ResultFingerprint(*reader, sql, entry.compare_unordered));
          writer->BeginTransaction();
          h::Exec(*writer, h::ReadFile(txn));
          writer->Rollback();
          writer->BeginTransaction();
          h::Exec(*writer, h::ReadFile(txn));
          if (IsRollbackTransaction(txn)) writer->Rollback();
          else writer->Commit();
          std::size_t changed = 0;
          for (std::size_t i = 0; i < references.size(); ++i)
            changed += before[i] != ResultFingerprint(*reader, references[i], entry.compare_unordered);
          txn_effects.emplace_back(txn.filename().string(), changed);
          ++checks;
        }
      } else {
        h::Compare(*reader, entry);
        if (tuples.empty()) tuple_rows.push_back(h::Query(*reader, references.front())->RowCount());
        ++checks;
        for (const auto &tuple : tuples) {
          const auto sql = entry.fallback_sql ? h::BindSql(entry.fallback_sql, tuple) : entry.base_sql;
          tuple_rows.push_back(h::Query(*reader, sql)->RowCount());
          h::Compare(*reader, entry, tuple, "suite tuple " + std::to_string(checks));
          ++checks;
        }
        if (entry.probe_select_sql && *entry.probe_select_sql && entry.build_probe) {
          auto probe = engine::FindSharedProbe(*reader, entry);
          const auto update = engine::ProbeWriteSql(probe);
          h::Exec(*writer, update);
          h::Compare(*reader, entry, {}, "after catalog probe");
          ++checks;
          const auto restore = probe.toggles || !probe.restore_sql.empty() ? probe.restore_sql : std::string{};
          if (!restore.empty()) {
            h::Exec(*writer, restore);
            h::Compare(*reader, entry, {}, "after probe restore");
            ++checks;
          }
        }
        for (const auto &txn : h::SuiteTransactions()) {
          std::vector<std::string> before;
          for (const auto &sql : references) before.push_back(ResultFingerprint(*reader, sql, entry.compare_unordered));
          writer->BeginTransaction();
          h::Exec(*writer, h::ReadFile(txn));
          if (IsRollbackTransaction(txn)) writer->Rollback();
          else writer->Commit();
          std::size_t changed = 0;
          for (std::size_t i = 0; i < references.size(); ++i) {
            h::Compare(*reader, entry, tuples.empty() ? std::vector<std::string>{} : tuples[i],
                       "after " + txn.filename().string());
            changed += before[i] != ResultFingerprint(*reader, references[i], entry.compare_unordered);
            ++checks;
          }
          txn_effects.emplace_back(txn.filename().string(), changed);
        }
      }
      WriteSuiteReport(true, checks, tuple_rows, txn_effects, "");
    } catch (const std::exception &error) {
      WriteSuiteReport(false, checks, tuple_rows, txn_effects, error.what());
      throw;
    }
    std::filesystem::remove(db_path);
    std::filesystem::remove(db_path + ".wal");
    std::cout << "differential passed for " << entry.workload_name << " query " << query << " (" << checks
              << " checks)\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "differential failed: " << error.what() << '\n';
    return 1;
  }
}
