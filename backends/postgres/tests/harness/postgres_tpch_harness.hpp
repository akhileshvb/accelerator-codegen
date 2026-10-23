#pragma once

// Shared PostgreSQL TPC-H SQL-vs-accel helpers (psql client, result compare,
// catalog SQL, extension + statement-level mutation triggers).

#include <unistd.h>
#include <sys/wait.h>

#include <algorithm>
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

#include "backends/postgres/tests/postgres_tooling.hpp"
#include "eval/harness/tpch_test_recipes.hpp"
#include "backends/postgres/api/catalog/query_catalog.hpp"
#include "tpch/common/postgres_tpch_table_ids.hpp"

namespace accel::postgres::tpch_harness {

using accel::postgres::DiscoverTools;
using accel::postgres::Tools;
using accelerator_codegen::aux_state::engine::postgres::LookupPostgresQuery;
using accelerator_codegen::aux_state::engine::postgres::PostgresSharedQueryCatalogEntry;

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

inline std::string PsqlPrefix(const Tools &tools) {
  return "PGPASSWORD=" + ShellEscape(tools.password) + " " +
         ShellEscape(tools.psql.string()) + " -h " + ShellEscape(tools.host) +
         " -p " + std::to_string(tools.port) + " -U " + ShellEscape(tools.user) +
         " -d " + ShellEscape(tools.database);
}

inline bool RunSQL(const Tools &tools, const std::string &sql) {
  const std::string cmd =
      PsqlPrefix(tools) + " -v ON_ERROR_STOP=1 -t -A -c " + ShellEscape(sql);
  return std::system(cmd.c_str()) == 0;
}

inline std::pair<bool, std::string> QuerySQL(const Tools &tools,
                                             const std::string &sql) {
  const std::string cmd =
      PsqlPrefix(tools) + " -t -A -c " + ShellEscape(sql) + " 2>/dev/null";
  FILE *fp = popen(cmd.c_str(), "r");
  if (!fp)
    return {false, ""};
  std::string out;
  char buf[4096];
  while (fgets(buf, sizeof(buf), fp)) {
    out += buf;
  }
  int rc = pclose(fp);
  return {WEXITSTATUS(rc) == 0, out};
}

inline std::string RunPsqlScript(const Tools &tools, const std::string &script) {
  const std::string path =
      std::string("/tmp/accel_pg_h_") + std::to_string(::getpid()) + "_" +
      std::to_string(std::rand()) + ".sql";
  {
    std::ofstream out(path);
    out << script;
  }
  const std::string cmd = PsqlPrefix(tools) +
                          " -v ON_ERROR_STOP=1 -q -t -A -f " + ShellEscape(path) +
                          " 2>&1";
  FILE *fp = popen(cmd.c_str(), "r");
  Require(fp != nullptr, "popen psql script failed");
  std::string out;
  char buf[4096];
  while (fgets(buf, sizeof(buf), fp)) {
    out += buf;
  }
  int rc = pclose(fp);
  Require(WEXITSTATUS(rc) == 0, "psql script failed:\n" + out);
  return out;
}

inline bool RunPsqlFile(const Tools &tools, const std::string &path) {
  const std::string cmd =
      PsqlPrefix(tools) + " -v ON_ERROR_STOP=1 -q -f " + ShellEscape(path);
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
            label + ": col count mismatch at row " + std::to_string(i));
    for (size_t j = 0; j < rc.size(); ++j) {
      Require(CellsApproxEqual(rc[j], ac[j]),
              label + ": cell mismatch row=" + std::to_string(i) + " col=" +
                  std::to_string(j) + " ref=[" + rc[j] + "] accel=[" + ac[j] +
                  "]");
    }
  }
}

inline std::string ConcreteBaseSql(int query_num,
                                   const PostgresSharedQueryCatalogEntry &entry) {
  return tpch_params::ReferenceSql(query_num, entry.base_sql ? entry.base_sql : "");
}

inline std::string ConcreteAccelSql(int query_num,
                                    const PostgresSharedQueryCatalogEntry &entry,
                                    bool prefer_aux) {
  const char *tmpl =
      prefer_aux && entry.aux_sql && *entry.aux_sql ? entry.aux_sql
                                                    : entry.accelerator_sql;
  Require(tmpl && *tmpl,
          "catalog missing accelerator/aux SQL for Q" + std::to_string(query_num));
  return tpch_params::AcceleratorSql(query_num, tmpl);
}

inline bool PreferAux(int /*query_num*/) {
  const char *kind = std::getenv("ACCEL_PG_DIFF_KIND");
  if (kind && std::string(kind) == "aux")
    return true;
  if (kind && std::string(kind) == "base")
    return false;
  const char *wk = std::getenv("ACCEL_WORKLOAD_KIND");
  return wk && std::string(wk) == "aux_state";
}

inline std::string RepoRootHint() {
  if (const char *src = std::getenv("ACCEL_SRC")) {
    if (*src)
      return src;
  }
  if (const char *root = std::getenv("ACCEL_REPO_ROOT")) {
    if (*root)
      return root;
  }
  return "/accel-src";
}

inline void EnsureExtension(const Tools &tools) {
  if (!tools.extension_so.empty()) {
    Require(RunSQL(tools, "LOAD '" + tools.extension_so + "'"),
            "LOAD " + tools.extension_so + " failed");
  }
  const std::string register_sql =
      RepoRootHint() + "/backends/postgres/host/register.sql";
  // Re-create the per-query functions from the loaded module every time: a DB
  // restored from an older build may carry stale signatures (calling one with
  // the wrong argument types crashes the backend). Older DBs lack the
  // registrar function itself; the full script installs it first.
  auto present = QuerySQL(
      tools, "SELECT 1 FROM pg_proc WHERE proname = 'accel_register_queries'");
  if (present.first && TrimLines(present.second).find('1') != std::string::npos) {
    Require(RunSQL(tools, "SELECT accel_register_queries()"),
            "accel_register_queries() failed");
  } else {
    Require(RunPsqlFile(tools, register_sql),
            "failed to register TPC-H SQL functions from " + register_sql);
  }
}

inline void EnsureMutationTriggers(const Tools &tools) {
  const std::string so =
      tools.extension_so.empty() ? "accelerator_codegen_pg" : tools.extension_so;
  Require(RunSQL(tools,
                 "CREATE OR REPLACE FUNCTION accel_aux_capture_transition() "
                 "RETURNS trigger AS " +
                     SqlLiteral(so) +
                     ", 'AcceleratorCodegenCaptureTransition' LANGUAGE C"),
          "install accel_aux_capture_transition failed");
  Require(RunSQL(tools,
                 "CREATE OR REPLACE FUNCTION accel_set_force_maintenance_action(text) "
                 "RETURNS void AS " +
                     SqlLiteral(so) +
                     ", 'accel_set_force_maintenance_action' LANGUAGE C"),
          "install force-maintenance helper failed");
  Require(RunSQL(tools,
                 "CREATE OR REPLACE FUNCTION accel_get_last_maintenance_decision() "
                 "RETURNS text AS " +
                     SqlLiteral(so) +
                     ", 'accel_get_last_maintenance_decision' LANGUAGE C"),
          "install last-decision helper failed");
  Require(RunSQL(tools,
                 "CREATE OR REPLACE FUNCTION accel_get_execute_counts() "
                 "RETURNS text AS " +
                     SqlLiteral(so) +
                     ", 'accel_get_execute_counts' LANGUAGE C STRICT"),
          "install execute-counts helper failed");
  Require(RunSQL(tools,
                 "CREATE OR REPLACE FUNCTION accel_get_last_maintenance_action() "
                 "RETURNS text AS " +
                     SqlLiteral(so) +
                     ", 'accel_get_last_maintenance_action' LANGUAGE C STRICT"),
          "install last-maintenance helper failed");

  // The workers scan the tables while they build: stop them (they restart on demand) so the
  // trigger DDL below cannot deadlock with a build.
  Require(RunSQL(tools,
                 "DO $$ BEGIN IF to_regproc('accel_stop_shard_workers') IS NOT NULL THEN "
                 "PERFORM accel_stop_shard_workers(); END IF; END $$"),
          "stop shard workers failed");

  // One statement-level trigger set per TPC-H table.
  Require(
      RunSQL(tools,
             "DO $$ DECLARE r record; BEGIN "
             "FOR r IN SELECT t.tgname, c.relname FROM pg_trigger t "
             "JOIN pg_class c ON c.oid = t.tgrelid "
             "JOIN pg_namespace n ON n.oid = c.relnamespace "
             "WHERE NOT t.tgisinternal AND n.nspname = 'public' "
             "AND c.relname IN ('lineitem','orders','customer','part','partsupp',"
             "'supplier','nation','region') "
             "AND (t.tgname LIKE 'accel_%' OR t.tgname LIKE 'accel_pg_%') "
             "LOOP EXECUTE format('DROP TRIGGER IF EXISTS %I ON %I', r.tgname, r.relname); "
             "END LOOP; END $$"),
      "drop legacy accel triggers failed");

  const char *tables[] = {"lineitem", "orders",  "customer", "part",
                          "partsupp", "supplier", "nation",   "region"};
  for (const char *t : tables) {
    std::string sql = std::string(
                          "DO $$ BEGIN "
                          "EXECUTE format('CREATE TRIGGER accel_tpch_%s_i AFTER INSERT ON %I "
                          "REFERENCING NEW TABLE AS new_rows FOR EACH STATEMENT "
                          "EXECUTE FUNCTION accel_aux_capture_transition()', '") +
                      t + "', '" + t +
                      "'); "
                      "EXECUTE format('CREATE TRIGGER accel_tpch_%s_d AFTER DELETE ON %I "
                      "REFERENCING OLD TABLE AS old_rows FOR EACH STATEMENT "
                      "EXECUTE FUNCTION accel_aux_capture_transition()', '" +
                      t + "', '" + t +
                      "'); "
                      "EXECUTE format('CREATE TRIGGER accel_tpch_%s_u AFTER UPDATE ON %I "
                      "REFERENCING OLD TABLE AS old_rows NEW TABLE AS new_rows "
                      "FOR EACH STATEMENT "
                      "EXECUTE FUNCTION accel_aux_capture_transition()', '" +
                      t + "', '" + t +
                      "'); "
                      "EXECUTE format('CREATE TRIGGER accel_tpch_%s_t AFTER TRUNCATE ON %I "
                      "FOR EACH STATEMENT "
                      "EXECUTE FUNCTION accel_aux_capture_transition()', '" +
                      t + "', '" + t + "'); END $$";
    Require(RunSQL(tools, sql), std::string("install statement trigger failed for ") + t);
  }
}

inline Tools ConnectTpch() {
  auto tools = DiscoverTools();
  Require(tools.ok, "PostgreSQL not available: " + tools.error);
  const char *tpch_db = std::getenv("TPCH_PG_DB");
  tools.database = (tpch_db && *tpch_db) ? tpch_db : "accel_tpch_test";
  EnsureExtension(tools);
  auto check = QuerySQL(tools, "SELECT COUNT(*) FROM lineitem");
  Require(check.first, "TPC-H tables missing — run setup_accel_postgres_tpch_test_db.sh");
  return tools;
}

inline void RequireQueryMatch(const Tools &tools, int query_num, bool prefer_aux,
                              const std::string &label) {
  const auto &entry = LookupPostgresQuery("tpch", query_num);
  const std::string ref_sql = ConcreteBaseSql(query_num, entry);
  const std::string accel_sql = ConcreteAccelSql(query_num, entry, prefer_aux);
  auto ref = QuerySQL(tools, ref_sql);
  Require(ref.first, label + ": reference SQL failed: " + ref_sql);
  auto accel = QuerySQL(tools, accel_sql);
  Require(accel.first, label + ": accelerator SQL failed: " + accel_sql +
                           "\n" + accel.second);
  RequireOrderedMatch(ref.second, accel.second, label);
}

inline std::string InvalidatingUpdateSql(int query_num) {
  return tpch_params::InvalidatingUpdateSql(query_num, tpch_params::UpdateDialect::Ctid);
}

inline std::vector<int> ParseQueryList(const char *env_name, const std::vector<int> &fallback) {
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

} // namespace accel::postgres::tpch_harness
