#pragma once

#include <unistd.h>
#include <sys/wait.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "backends/mysql/tests/mysql_tooling.hpp"
#include "eval/harness/tpch_test_recipes.hpp"
#include "backends/mysql/api/catalog/query_catalog.hpp"

namespace accel::mysql::tpch_harness {

using accel::mysql::DiscoverTools;
using accel::mysql::Tools;
using accelerator_codegen::aux_state::engine::mysql::LookupMysqlQuery;
using accelerator_codegen::aux_state::engine::mysql::MysqlSharedQueryCatalogEntry;

inline void Require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

inline std::string ShellEscape(const std::string &s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'')
      out += "'\\''";
    else
      out += c;
  }
  out += "'";
  return out;
}

inline std::string SqlLiteral(const std::string &s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'')
      out += "''";
    else
      out += c;
  }
  out += "'";
  return out;
}

inline std::string MysqlPrefix(const Tools &tools) {
  return "MYSQL_PWD=" + ShellEscape(tools.password) + " mysql -h " +
         ShellEscape(tools.host) + " -P " + std::to_string(tools.port) + " -u" +
         tools.user + " -D " + ShellEscape(tools.database) +
         " --batch --raw --skip-column-names";
}

inline std::string TabsToPipes(std::string text) {
  for (char &c : text) {
    if (c == '\t')
      c = '|';
  }
  return text;
}

inline std::string StripMysqlWarnings(std::string text) {
  const char *warn =
      "mysql: [Warning] Using a password on the command line interface can be insecure.\n";
  auto pos = text.find(warn);
  while (pos != std::string::npos) {
    text.erase(pos, std::strlen(warn));
    pos = text.find(warn);
  }
  return text;
}

inline bool RunSQL(const Tools &tools, const std::string &sql) {
  const std::string cmd =
      MysqlPrefix(tools) + " -e " + ShellEscape(sql) + " >/dev/null 2>&1";
  return std::system(cmd.c_str()) == 0;
}

inline std::pair<bool, std::string> QuerySQL(const Tools &tools,
                                             const std::string &sql) {
  const std::string cmd =
      MysqlPrefix(tools) + " -e " + ShellEscape(sql) + " 2>/dev/null";
  FILE *fp = popen(cmd.c_str(), "r");
  if (!fp)
    return {false, ""};
  std::string out;
  char buf[4096];
  while (fgets(buf, sizeof(buf), fp)) {
    out += buf;
  }
  int rc = pclose(fp);
  out = TabsToPipes(StripMysqlWarnings(out));
  return {WEXITSTATUS(rc) == 0, out};
}

inline std::string RunMysqlScript(const Tools &tools, const std::string &script) {
  const std::string path = std::string("/tmp/accel_mysql_h_") +
                           std::to_string(::getpid()) + "_" +
                           std::to_string(std::rand()) + ".sql";
  {
    std::ofstream out(path);
    out << script;
  }
  const std::string cmd =
      MysqlPrefix(tools) + " < " + ShellEscape(path) + " 2>&1";
  FILE *fp = popen(cmd.c_str(), "r");
  Require(fp != nullptr, "popen mysql script failed");
  std::string out;
  char buf[4096];
  while (fgets(buf, sizeof(buf), fp)) {
    out += buf;
  }
  int rc = pclose(fp);
  out = TabsToPipes(StripMysqlWarnings(out));
  Require(WEXITSTATUS(rc) == 0, "mysql script failed:\n" + out);
  return out;
}

inline double TimeSqlBatchUs(const Tools &tools, const std::string &sql,
                             int warmup, int iters) {
  if (warmup > 0) {
    std::ostringstream warm;
    for (int i = 0; i < warmup; ++i) {
      warm << sql << ";\n";
    }
    RunMysqlScript(tools, warm.str());
  }
  std::ostringstream timed;
  const int n = std::max(1, iters);
  for (int i = 0; i < n; ++i) {
    timed << sql << ";\n";
  }
  const auto start = std::chrono::steady_clock::now();
  RunMysqlScript(tools, timed.str());
  const auto end = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::micro>(end - start).count() /
         static_cast<double>(n);
}

inline bool RunMysqlFile(const Tools &tools, const std::string &path) {
  const std::string cmd =
      MysqlPrefix(tools) + " < " + ShellEscape(path) + " >/dev/null 2>&1";
  return std::system(cmd.c_str()) == 0;
}

inline std::string TrimLines(const std::string &input) {
  std::string result;
  result.reserve(input.size());
  size_t start = 0;
  for (size_t i = 0; i <= input.size(); ++i) {
    if (i == input.size() || input[i] == '\n') {
      size_t end = i;
      while (end > start &&
             (input[end - 1] == ' ' || input[end - 1] == '\t' ||
              input[end - 1] == '\r')) {
        --end;
      }
      if (end > start) {
        result.append(input, start, end - start);
        result += '\n';
      }
      start = i + 1;
    }
  }
  return result;
}

inline std::vector<std::string> SplitLines(const std::string &s) {
  std::vector<std::string> lines;
  std::string cur;
  for (char c : s) {
    if (c == '\n') {
      if (!cur.empty())
        lines.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty())
    lines.push_back(cur);
  return lines;
}

inline bool CellsApproxEqual(const std::string &a, const std::string &b) {
  if (a == b)
    return true;
  try {
    double da = std::stod(a);
    double db = std::stod(b);
    double scale = std::max({1.0, std::fabs(da), std::fabs(db)});
    return std::fabs(da - db) <= 1e-6 * scale;
  } catch (...) {
    return false;
  }
}

inline void RequireOrderedMatch(const std::string &ref, const std::string &accel,
                                const std::string &label) {
  auto rlines = SplitLines(TrimLines(ref));
  auto alines = SplitLines(TrimLines(accel));
  Require(rlines.size() == alines.size(),
          label + ": row count mismatch ref=" + std::to_string(rlines.size()) +
              " accel=" + std::to_string(alines.size()) + "\nref:\n" +
              TrimLines(ref) + "accel:\n" + TrimLines(accel));
  for (size_t i = 0; i < rlines.size(); ++i) {
    auto SplitPipe = [](const std::string &line) {
      std::vector<std::string> cells;
      std::string cur;
      for (char c : line) {
        if (c == '|') {
          cells.push_back(cur);
          cur.clear();
        } else {
          cur += c;
        }
      }
      cells.push_back(cur);
      return cells;
    };
    auto rc = SplitPipe(rlines[i]);
    auto ac = SplitPipe(alines[i]);
    Require(rc.size() == ac.size(),
            label + ": col count mismatch at row " + std::to_string(i) +
                " ref=[" + rlines[i] + "] accel=[" + alines[i] + "]");
    for (size_t j = 0; j < rc.size(); ++j) {
      Require(CellsApproxEqual(rc[j], ac[j]),
              label + ": cell mismatch row=" + std::to_string(i) + " col=" +
                  std::to_string(j) + " ref=[" + rc[j] + "] accel=[" + ac[j] +
                  "]");
    }
  }
}

inline std::string ConcreteBaseSql(int query_num,
                                   const MysqlSharedQueryCatalogEntry &entry) {
  return tpch_params::ReferenceSql(query_num, entry.base_sql ? entry.base_sql : "");
}

inline std::string ConcreteAccelSql(int query_num,
                                    const MysqlSharedQueryCatalogEntry &entry,
                                    bool prefer_aux) {
  const char *tmpl =
      prefer_aux && entry.aux_sql && *entry.aux_sql ? entry.aux_sql
                                                    : entry.accelerator_sql;
  Require(tmpl && *tmpl,
          "catalog missing accelerator/aux SQL for Q" + std::to_string(query_num));
  return tpch_params::AcceleratorSql(query_num, tmpl);
}

inline bool PreferAux(int /*query_num*/) {
  const char *kind = std::getenv("ACCEL_MYSQL_DIFF_KIND");
  if (kind && std::string(kind) == "aux")
    return true;
  if (kind && std::string(kind) == "base")
    return false;
  const char *wk = std::getenv("ACCEL_WORKLOAD_KIND");
  return !wk || std::string(wk) == "aux_state";
}

inline std::string RepoRootHint() {
  if (const char *src = std::getenv("ACCEL_SRC"); src && *src)
    return src;
  if (const char *root = std::getenv("ACCEL_REPO_ROOT"); root && *root)
    return root;
  return "/accel-src";
}

inline void EnsureUdf(const Tools &tools, const std::string &name,
                      const std::string &returns_type) {
  auto present = QuerySQL(tools, "SELECT 1 FROM mysql.func WHERE name = " +
                                     SqlLiteral(name));
  if (present.first && TrimLines(present.second).find('1') != std::string::npos) {
    return;
  }
  Require(RunSQL(tools, "CREATE FUNCTION " + name + " RETURNS " + returns_type +
                            " SONAME " + SqlLiteral(tools.plugin_so)),
          "CREATE FUNCTION " + name + " failed");
}

inline void EnsureExtension(const Tools &tools) {
  RunSQL(tools, "INSTALL PLUGIN accelerator_codegen_audit SONAME " +
                    SqlLiteral(tools.plugin_so));
  EnsureUdf(tools, "mysql_accel_capture_row", "STRING");
  EnsureUdf(tools, "mysql_accel_observer_mutation_count", "INTEGER");
  EnsureUdf(tools, "mysql_accel_reset_observer", "STRING");
  EnsureUdf(tools, "accel_set_force_maintenance_action", "STRING");
  EnsureUdf(tools, "accel_get_execute_counts", "STRING");
  EnsureUdf(tools, "accel_get_last_maintenance_decision", "STRING");
  // Optional: SF=1's copied plugin may predate this UDF. Cost routing still
  // honors ACCEL_EXPECTED_QUERIES_BEFORE_WRITE / the default of 10.
  {
    auto present =
        QuerySQL(tools, "SELECT 1 FROM mysql.func WHERE name = " +
                            SqlLiteral("mysql_accel_set_expected_queries"));
    if (!(present.first &&
          TrimLines(present.second).find('1') != std::string::npos)) {
      RunSQL(tools,
             "CREATE FUNCTION mysql_accel_set_expected_queries RETURNS STRING "
             "SONAME " +
                 SqlLiteral(tools.plugin_so));
    }
  }
  EnsureUdf(tools, "accel_get_last_maintenance_action", "STRING");
  EnsureUdf(tools, "accel_get_execute_counts", "STRING");
  for (int q = 1; q <= 22; ++q) {
    EnsureUdf(tools, "mysql_accel_tpch_q" + std::to_string(q) + "_aux", "STRING");
  }
}

inline Tools ConnectTpch() {
  auto tools = DiscoverTools();
  Require(tools.ok, "MySQL not available: " + tools.error);
  const char *tpch_db = std::getenv("TPCH_MYSQL_DB");
  tools.database = (tpch_db && *tpch_db) ? tpch_db : tools.database;
  auto check = QuerySQL(tools, "SELECT COUNT(*) FROM lineitem");
  Require(check.first, "TPC-H tables missing — run setup_accel_mysql_tpch_test_db.sh");
  EnsureExtension(tools);
  return tools;
}

inline void EnsureMutationTriggers(const Tools &tools) {
  EnsureExtension(tools);
  const std::string path = RepoRootHint() + "/scripts/mysql_accel_mutation_triggers.sql";
  Require(RunMysqlFile(tools, path), "failed to install mutation triggers from " + path);
}

inline void RequireQueryMatch(const Tools &tools, int query_num, bool prefer_aux,
                              const std::string &label) {
  const auto &entry = LookupMysqlQuery("tpch", query_num);
  const std::string ref_sql = ConcreteBaseSql(query_num, entry);
  const std::string accel_sql = ConcreteAccelSql(query_num, entry, prefer_aux);
  auto ref = QuerySQL(tools, ref_sql);
  Require(ref.first, label + ": reference SQL failed: " + ref_sql + "\n" +
                         ref.second);
  auto accel = QuerySQL(tools, accel_sql);
  Require(accel.first, label + ": accelerator SQL failed: " + accel_sql + "\n" +
                           accel.second);
  RequireOrderedMatch(ref.second, accel.second, label);
}

inline std::string InvalidatingUpdateSql(int query_num) {
  return tpch_params::InvalidatingUpdateSql(query_num, tpch_params::UpdateDialect::OrderLimit);
}

inline std::vector<int> ParseQueryList(const char *env_name,
                                       const std::vector<int> &fallback) {
  const char *raw = std::getenv(env_name);
  if (!raw || !*raw)
    return fallback;
  std::vector<int> queries;
  std::stringstream stream(raw);
  std::string token;
  while (std::getline(stream, token, ',')) {
    if (!token.empty())
      queries.push_back(std::stoi(token));
  }
  return queries.empty() ? fallback : queries;
}

} // namespace accel::mysql::tpch_harness
