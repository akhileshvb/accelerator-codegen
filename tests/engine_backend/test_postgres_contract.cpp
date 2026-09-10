// GENERATED BACKEND - PostgreSQL engine contract harness (implementation-owned).
// Spec: pipeline/prompts/engine_backend/contract_tests.md
// Build: test_postgres_contract  CTest: engine_postgres_contract
//
// CMake/SDK: ACCELERATOR_CODEGEN_BUILD_POSTGRES=ON. In Docker mode
// (ACCEL_PG_DOCKER=1) builds inside accel_pg_test container and runs via
// docker exec. Run: ctest -R engine_postgres_contract or
// ./build-*/test_postgres_contract.
//
// Hard rules: real native path enabled; value asserts; real mutation hooks;
// no TODO Fail left in main; no TPC-H/SEC modules; NEVER SKIP-then-PASS.
//
// Architecture audit evidence (acceptance_checklist.md):
//   native_scan: TestNativeScanContract calls accel_postgres_native_scan()
//     which uses table_open/table_beginscan/table_scan_getnextslot/slot_getattr.
//   value_checked_scan: seeds 3 rows + NULL, projects subset, filter EQ drops
//     rows, asserts exact cell values including NULL.
//     The native scan SRF collects all rows; for batch_size=1 the extension
//     increments the internal callback counter per row. The SRF output rows
//     are the concatenated golden set.
//   mutation_hooks: TestMutationCaptureContract installs real AFTER ROW trigger,
//     drives INSERT/UPDATE/DELETE, asserts value-level old/new cell values.
//   txn_lifecycle: TestTxnLifecycleContract tests Abort clears + Commit publishes.
//   visibility: TestVisibilityContract uses staged toy builds (defer mode),
//     concurrent psql sessions, and epoch capture to prove uncommitted-write
//     isolation and token/epoch inequality across commit boundary.
//   store_roundtrip: TestStateStoreRoundTrip exercises PostgresStateStore via
//     accel_pg_toy_check_validity/accel_pg_toy_invalidate bridge UDFs:
//     Build → CheckValidity(Valid) → Read → Invalidate → CheckValidity(!Valid) → fail-closed read.
//   fail_closed: DiscoverTools failure -> Fail() without SKIP.
//   host_glue: _PG_init, PG_MODULE_MAGIC, CREATE FUNCTION, real trigger/callbacks.
//   independence: no DuckDB adapter, no tpch/sec_edgar modules, no shared DataChunk.

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "accelerator_codegen/engine_backend/postgres_tooling.hpp"

namespace {

using namespace accel::postgres;

[[noreturn]] void Fail(const char *msg) {
  std::cerr << "engine_contract[postgres] FAIL: " << msg << "\n";
  std::abort();
}

void Expect(bool cond, const char *msg) {
  if (!cond) Fail(msg);
}

// Execute a SQL statement via psql using a quoted heredoc (robust to
// embedded quotes). Returns true on success.
bool RunSQL(const Tools &tools, const std::string &sql) {
  std::string cmd = "PGPASSWORD='" + tools.password + "' '" +
                    tools.psql.string() + "' -h '" + tools.host +
                    "' -p " + std::to_string(tools.port) +
                    " -U '" + tools.user + "' -d '" + tools.database +
                    "' -v ON_ERROR_STOP=1 -t -A <<'ACCELL_SQL_EOF'\n" +
                    sql + "\nACCELL_SQL_EOF\n";
  return std::system(cmd.c_str()) == 0;
}

// Execute a SQL statement and capture output. Returns (success, stdout).
std::pair<bool, std::string> QuerySQL(const Tools &tools, const std::string &sql) {
  std::string cmd = "PGPASSWORD='" + tools.password + "' '" +
                    tools.psql.string() + "' -h '" + tools.host +
                    "' -p " + std::to_string(tools.port) +
                    " -U '" + tools.user + "' -d '" + tools.database +
                    "' -t -A <<'ACCELL_SQL_EOF'\n" +
                    sql + "\nACCELL_SQL_EOF\n 2>/dev/null";
  FILE *fp = popen(cmd.c_str(), "r");
  if (!fp) return {false, ""};
  std::string out;
  char buf[4096];
  while (fgets(buf, sizeof(buf), fp)) out += buf;
  int rc = pclose(fp);
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
    out.pop_back();
  return {rc == 0, out};
}

// Register all extension helper functions.
void RegisterHelpers(const Tools &tools) {
  const char *so = tools.extension_so.c_str();

  // Trigger function
  {
    std::string trig_sql = "CREATE OR REPLACE FUNCTION accel_postgres_capture_mutation_trigger() "
    "RETURNS TRIGGER AS $$" + std::string(so) + "$$, $$accel_postgres_capture_mutation_trigger$$ LANGUAGE C STRICT";
    if (!RunSQL(tools, trig_sql)) {
      Fail("failed to register capture_mutation_trigger");
    }
  }

  // Observer functions
  const char *helpers[] = {
    "accel_postgres_get_observer_pre_committed", "BOOLEAN",
    "accel_postgres_get_observer_committed", "BOOLEAN",
    "accel_postgres_get_observer_aborted", "BOOLEAN",
    "accel_postgres_get_observer_mutation_count", "BIGINT",
    "accel_postgres_get_observer_mutation_at", "TEXT",
    "accel_postgres_toy_read", "BIGINT",
    "accel_postgres_reset_observer", "VOID",
    "accel_postgres_get_scan_row_count", "BIGINT",
    "accel_postgres_get_scan_column_count", "BIGINT",
    "accel_postgres_get_scan_callback_count", "BIGINT",
    "accel_postgres_get_scan_row_cell", "TEXT",
    "accel_postgres_get_observer_mutation_row", "TEXT",
    "accel_postgres_toy_current_epoch", "BIGINT",
    "accel_postgres_toy_invalidate", "VOID",
    "accel_postgres_get_last_new_json", "TEXT",
    "accel_postgres_get_last_old_json", "TEXT",
    nullptr
  };

  for (int i = 0; helpers[i]; i += 2) {
    std::string sql = std::string("CREATE OR REPLACE FUNCTION ") +
          helpers[i] + "() RETURNS " + helpers[i+1] +
          " AS $$" + std::string(so) + "$$, $$" + helpers[i] + "$$ LANGUAGE C STRICT";
    if (!RunSQL(tools, sql)) {
      Fail(("failed to register " + std::string(helpers[i])).c_str());
    }
  }

  // Functions with params
  const char *helpers_param[] = {
    "accel_postgres_toy_build", "TEXT,BIGINT", "BOOLEAN",
    "accel_postgres_get_observer_mutation_at", "INT", "TEXT",
    "accel_postgres_get_scan_row_cell", "INT,INT", "TEXT",
    "accel_postgres_native_scan", "TEXT,TEXT,TEXT[],TEXT,INT,TEXT,INT",
        "SETOF TEXT",
    "accel_postgres_get_observer_mutation_row", "INT,INT,INT", "TEXT",
    "accel_postgres_toy_check_validity", "TEXT,BIGINT", "TEXT",
    nullptr
  };

  for (int i = 0; helpers_param[i]; i += 3) {
    std::string sql = std::string("CREATE OR REPLACE FUNCTION ") +
          helpers_param[i] + "(" + helpers_param[i+1] + ") RETURNS " +
          helpers_param[i+2] +
          " AS $$" + std::string(so) + "$$, $$" + helpers_param[i] + "$$ LANGUAGE C STRICT";
    if (!RunSQL(tools, sql)) {
      Fail(("failed to register " + std::string(helpers_param[i])).c_str());
    }
  }
}

// ----------------------------------------------------------------
// 1) Native scan contract (VALUE-CHECKED via UDF)
// ----------------------------------------------------------------
void TestNativeScanContract(const Tools &tools) {
  // Seed a small base table with >=3 known rows, including a NULL.
  Expect(RunSQL(tools, "DROP TABLE IF EXISTS test_native_scan_t CASCADE"),
         "drop failed");
  Expect(RunSQL(tools,
    "CREATE TABLE test_native_scan_t (id INT, name TEXT, val INT)"),
    "create failed");
  Expect(RunSQL(tools,
    "INSERT INTO test_native_scan_t VALUES "
    "(1, 'alpha', 100), (2, 'beta', 200), (3, NULL, 300), (4, 'beta2', 200)"),
    "seed failed");

  // --- First scan: project (id, val), filter val=200, batch_size=1 ---
  // Run scan UDF and all assertions in a single connection.
  auto [ok1, result1] = QuerySQL(tools,
    "SELECT * FROM accel_postgres_native_scan("
    "'public', 'test_native_scan_t', "
    "ARRAY['id','val'], 'val', 0, '200', 1);");
  Expect(ok1, "native scan query 1 failed");

  // SRF output rows appear as lines with '|' separator.
  // Count SRF rows and parse values.
  int srf_line_count = 0;
  std::string srf_lines[10];
  {
    std::string current;
    for (char c : result1) {
      if (c == '\n') {
        if (!current.empty()) {
          if (current.find('|') != std::string::npos) {
            if (srf_line_count < 10) srf_lines[srf_line_count] = current;
            srf_line_count++;
          }
          current.clear();
        }
      } else {
        current += c;
      }
    }
    if (!current.empty() && current.find('|') != std::string::npos) {
      if (srf_line_count < 10) srf_lines[srf_line_count] = current;
      srf_line_count++;
    }
  }

  Expect(srf_line_count == 2, "expected 2 SRF rows after filter val=200");

  // SRF row 0: "2|200"
  {
    size_t pipe = srf_lines[0].find('|');
    Expect(pipe != std::string::npos, "SRF row 0 missing pipe");
    Expect(srf_lines[0].substr(0, pipe) == "2", "row 0 col 0 expected '2'");
    Expect(srf_lines[0].substr(pipe + 1) == "200", "row 0 col 1 expected '200'");
  }
  // SRF row 1: "4|200"
  {
    size_t pipe = srf_lines[1].find('|');
    Expect(pipe != std::string::npos, "SRF row 1 missing pipe");
    Expect(srf_lines[1].substr(0, pipe) == "4", "row 1 col 0 expected '4'");
    Expect(srf_lines[1].substr(pipe + 1) == "200", "row 1 col 1 expected '200'");
  }

  // Callback / batch metadata must be observed in the *same* backend as the
  // scan (same QuerySQL). batch_size=1 → one callback per surviving row.
  auto [ok_meta, meta] = QuerySQL(tools,
    "SELECT * FROM accel_postgres_native_scan("
    "'public', 'test_native_scan_t', "
    "ARRAY['id','val'], 'val', 0, '200', 1);"
    "SELECT accel_postgres_get_scan_callback_count();"
    "SELECT accel_postgres_get_scan_row_count();");
  Expect(ok_meta, "native scan + callback metadata query failed");
  {
    std::vector<std::string> mlines;
    std::string cur;
    for (char c : meta) {
      if (c == '\n') {
        if (!cur.empty() && cur.find('|') == std::string::npos) mlines.push_back(cur);
        cur.clear();
      } else {
        cur += c;
      }
    }
    if (!cur.empty() && cur.find('|') == std::string::npos) mlines.push_back(cur);
    Expect(mlines.size() >= 2, "callback metadata lines missing");
    // Last two non-SRF lines: callback_count, row_count.
    Expect(mlines[mlines.size() - 2] == "2",
           "batch_size=1 must produce 2 consumer callbacks");
    Expect(mlines[mlines.size() - 1] == "2",
           "scan row count must be 2");
  }

  // --- Second scan: filter val=300, project (name, val), include NULL ---
  auto [ok2, result2] = QuerySQL(tools,
    "SELECT * FROM accel_postgres_native_scan("
    "'public', 'test_native_scan_t', "
    "ARRAY['name','val'], 'val', 0, '300', 2);");
  Expect(ok2, "native scan query 2 failed");

  // Parse second scan SRF output
  std::string srf2_lines[10];
  int srf2_count = 0;
  {
    std::string current;
    for (char c : result2) {
      if (c == '\n') {
        if (!current.empty()) {
          if (current.find('|') != std::string::npos) {
            if (srf2_count < 10) srf2_lines[srf2_count] = current;
            srf2_count++;
          }
          current.clear();
        }
      } else {
        current += c;
      }
    }
    if (!current.empty() && current.find('|') != std::string::npos) {
      if (srf2_count < 10) srf2_lines[srf2_count] = current;
      srf2_count++;
    }
  }

  Expect(srf2_count == 1, "expected 1 SRF row after val=300 filter");

  // Row: name=NULL, val=300
  {
    size_t pipe = srf2_lines[0].find('|');
    Expect(pipe != std::string::npos, "SRF2 missing pipe");
    Expect(srf2_lines[0].substr(0, pipe) == "NULL",
           "name should be NULL for id=3");
    Expect(srf2_lines[0].substr(pipe + 1) == "300",
           "val should be 300 for id=3");
  }

  Expect(RunSQL(tools, "DROP TABLE IF EXISTS test_native_scan_t CASCADE"),
         "cleanup failed");

  std::cout << "  native_scan: PASS (projection + EQ filter + NULL)\n";
}
// 2) Mutation capture (real AFTER ROW trigger, VALUE-LEVEL)
// ----------------------------------------------------------------

// Filter out psql DML command tag lines (INSERT X Y, DELETE X, UPDATE X, etc.)
// that appear in -t -A output because psql does not suppress command tags.
static bool IsDMLCommandTag(const std::string &line) {
  if (line.empty()) return false;
  // Common PG command tags that leak through -t
  const char *tags[] = {"INSERT ", "DELETE ", "UPDATE ", "DROP ", "CREATE ",
                        "SELECT ", "SET ", "BEGIN", "COMMIT", "ABORT",
                        "TRUNCATE ", "ALTER ", "COPY ", "ROLLBACK"};
  for (const char *tag : tags) {
    if (line.compare(0, strlen(tag), tag) == 0) return true;
  }
  return false;
}

void TestMutationCaptureContract(const Tools &tools) {
  Expect(RunSQL(tools, "DROP TABLE IF EXISTS test_mut_cap CASCADE"), "drp failed");
  Expect(RunSQL(tools,
    "CREATE TABLE test_mut_cap (id INT, name TEXT, val INT)"),
    "create failed");
  Expect(RunSQL(tools,
    "CREATE TRIGGER accel_mut_trg "
    "AFTER INSERT OR UPDATE OR DELETE ON test_mut_cap "
    "FOR EACH ROW EXECUTE FUNCTION accel_postgres_capture_mutation_trigger()"),
    "trigger creation failed");
  
  // Each mutation test runs in a single psql connection (same backend process).
  // Combine DML + observer check in one QuerySQL.

  // --- INSERT ---
  Expect(RunSQL(tools, "SELECT accel_postgres_reset_observer()"), "reset failed");
  
  // Run INSERT + observer count in one connection
  auto [ok_ins, res_ins] = QuerySQL(tools,
    "INSERT INTO test_mut_cap VALUES (1, 'alice', 100);"
    "SELECT accel_postgres_get_observer_mutation_count();"
    "SELECT accel_postgres_get_observer_mutation_at(0);"
    "SELECT accel_postgres_get_observer_mutation_row(0, 0, 1);");
  Expect(ok_ins, "insert batch query failed");
  
  // Parse multi-result output. INSERT returns nothing with -t, so we get:
  // line 0: 1 (count)
  // line 1: {table:...,kind:Insert,...}
  std::vector<std::string> rlines;
  {
    std::string cur;
    for (char c : res_ins) {
      if (c == '\n') {
        if (!cur.empty() && !IsDMLCommandTag(cur)) { rlines.push_back(cur); }
        cur.clear();
      } else { cur += c; }
    }
    if (!cur.empty() && !IsDMLCommandTag(cur)) rlines.push_back(cur);
  }
  
  Expect(rlines.size() >= 2, "insert batch output too short");
  Expect(rlines[0] == "1", "mutation count should be 1");
  Expect(rlines[1].find("Insert") != std::string::npos, "batch kind should be Insert");
  Expect(rlines[1].find("new_rows:1") != std::string::npos,
         "insert batch should carry 1 new row");
  Expect(rlines.size() >= 3, "insert new-row value output missing");
  Expect(rlines[2] == "1,alice,100",
         "insert new-row cells should be 1,alice,100");

  // --- DELETE ---
  Expect(RunSQL(tools, "SELECT accel_postgres_reset_observer()"), "reset2 failed");
  auto [ok_del, res_del] = QuerySQL(tools,
    "DELETE FROM test_mut_cap WHERE id = 1;"
    "SELECT accel_postgres_get_observer_mutation_count();"
    "SELECT accel_postgres_get_observer_mutation_at(0);"
    "SELECT accel_postgres_get_observer_mutation_row(0, 0, 0);");
  Expect(ok_del, "delete batch query failed");
  
  std::vector<std::string> dlines;
  {
    std::string cur;
    for (char c : res_del) {
      if (c == '\n') {
        if (!cur.empty() && !IsDMLCommandTag(cur)) { dlines.push_back(cur); }
        cur.clear();
      } else { cur += c; }
    }
    if (!cur.empty() && !IsDMLCommandTag(cur)) dlines.push_back(cur);
  }
  Expect(dlines.size() >= 3, "delete batch output too short");
  Expect(dlines[0] == "1", "delete mutation count should be 1");
  Expect(dlines[1].find("Delete") != std::string::npos, "delete kind wrong");
  Expect(dlines[1].find("old_rows:1") != std::string::npos,
         "delete batch should carry 1 old row");
  Expect(dlines[2] == "1,alice,100",
         "delete old-row cells should be 1,alice,100");

  // --- UPDATE ---
  Expect(RunSQL(tools, "SELECT accel_postgres_reset_observer()"), "reset3 failed");
  Expect(RunSQL(tools, "INSERT INTO test_mut_cap VALUES (2, 'bob', 200)"), "insert2 failed");
  Expect(RunSQL(tools, "SELECT accel_postgres_reset_observer()"), "reset4 failed");
  auto [ok_upd, res_upd] = QuerySQL(tools,
    "UPDATE test_mut_cap SET val = 300 WHERE id = 2;"
    "SELECT accel_postgres_get_observer_mutation_count();"
    "SELECT accel_postgres_get_observer_mutation_at(0);"
    "SELECT accel_postgres_get_observer_mutation_row(0, 0, 0);"
    "SELECT accel_postgres_get_observer_mutation_row(0, 0, 1);");
  Expect(ok_upd, "update batch query failed");
  
  std::vector<std::string> ulines;
  {
    std::string cur;
    for (char c : res_upd) {
      if (c == '\n') {
        if (!cur.empty() && !IsDMLCommandTag(cur)) { ulines.push_back(cur); }
        cur.clear();
      } else { cur += c; }
    }
    if (!cur.empty() && !IsDMLCommandTag(cur)) ulines.push_back(cur);
  }
  Expect(ulines.size() >= 2, "update batch output too short");
  Expect(ulines[0] == "1", "update mutation count should be 1");
  Expect(ulines[1].find("Update") != std::string::npos, "update kind wrong");
  Expect(ulines[1].find("new_rows:1") != std::string::npos,
         "update batch should carry 1 new row");
  Expect(ulines[1].find("old_rows:1") != std::string::npos,
         "update batch should carry 1 old row");
  Expect(ulines.size() >= 4, "update value output missing");
  Expect(ulines[2] == "2,bob,200", "update old-row cells should be 2,bob,200");
  Expect(ulines[3] == "2,bob,300", "update new-row cells should be 2,bob,300");

  Expect(RunSQL(tools, "DROP TABLE IF EXISTS test_mut_cap CASCADE"), "cleanup failed");

  std::cout << "  mutation_capture: PASS (insert/update/delete via trigger, value-level)\n";
}
// 3) Txn lifecycle contract
// ----------------------------------------------------------------
void TestTxnLifecycleContract(const Tools &tools) {
  Expect(RunSQL(tools, "DROP TABLE IF EXISTS test_txn CASCADE"), "drop failed");
  Expect(RunSQL(tools, "CREATE TABLE test_txn (id INT, x INT)"), "create failed");
  Expect(RunSQL(tools,
    "CREATE TRIGGER accel_txn_trig "
    "AFTER INSERT OR UPDATE OR DELETE ON test_txn "
    "FOR EACH ROW EXECUTE FUNCTION accel_postgres_capture_mutation_trigger()"),
    "trigger creation failed");

  // Baseline published epoch/value before abort path.
  Expect(RunSQL(tools, "SELECT accel_postgres_toy_build('txn_key', 42)"),
         "txn baseline build failed");
  auto [ok_base_e, base_epoch_s] =
      QuerySQL(tools, "SELECT accel_postgres_toy_current_epoch()");
  Expect(ok_base_e, "baseline epoch failed");
  const auto base_epoch = static_cast<uint64_t>(std::stoull(base_epoch_s));

  // Abort: mutation + staged toy build must not publish / advance epoch.
  auto [ok_ab, res_ab] = QuerySQL(tools,
      "SELECT accel_postgres_reset_observer();"
      "BEGIN;"
      "INSERT INTO test_txn VALUES (1, 10);"
      "SELECT accel_postgres_toy_build('txn_key', 99);"
      "ABORT;"
      "SELECT 'aborted=' || accel_postgres_get_observer_aborted()::text;"
      "SELECT 'committed=' || accel_postgres_get_observer_committed()::text;"
      "SELECT 'epoch=' || accel_postgres_toy_current_epoch()::text;"
      "SELECT 'read=' || COALESCE(accel_postgres_toy_read()::text, '');");
  Expect(ok_ab, "abort txn query failed");

  std::vector<std::string> alines;
  {
    std::string cur;
    for (char c : res_ab) {
      if (c == '\n') {
        if (!cur.empty() && !IsDMLCommandTag(cur)) alines.push_back(cur);
        cur.clear();
      } else { cur += c; }
    }
    if (!cur.empty() && !IsDMLCommandTag(cur)) alines.push_back(cur);
  }
  auto find_line = [&](const char *prefix) -> std::string {
    for (const auto &l : alines) {
      if (l.rfind(prefix, 0) == 0) return l.substr(std::strlen(prefix));
    }
    return {};
  };
  Expect(find_line("aborted=") == "true" || find_line("aborted=") == "t",
         "observer should show aborted after ABORT");
  Expect(find_line("committed=") == "false" || find_line("committed=") == "f",
         "observer should NOT show committed after ABORT");
  Expect(std::stoull(find_line("epoch=")) == base_epoch,
         "abort must not advance published epoch");
  Expect(find_line("read=") == "42", "abort must not publish staged toy value");

  // Commit: observer committed + epoch advances + value publishes.
  auto [ok_cm, res_cm] = QuerySQL(tools,
      "SELECT accel_postgres_reset_observer();"
      "BEGIN;"
      "INSERT INTO test_txn VALUES (2, 20);"
      "SELECT accel_postgres_toy_build('txn_key', 77);"
      "COMMIT;"
      "SELECT 'committed=' || accel_postgres_get_observer_committed()::text;"
      "SELECT 'epoch=' || accel_postgres_toy_current_epoch()::text;"
      "SELECT 'read=' || COALESCE(accel_postgres_toy_read()::text, '');");
  Expect(ok_cm, "commit txn query failed");

  std::vector<std::string> clines;
  {
    std::string cur;
    for (char c : res_cm) {
      if (c == '\n') {
        if (!cur.empty() && !IsDMLCommandTag(cur)) clines.push_back(cur);
        cur.clear();
      } else { cur += c; }
    }
    if (!cur.empty() && !IsDMLCommandTag(cur)) clines.push_back(cur);
  }
  auto find_cline = [&](const char *prefix) -> std::string {
    for (const auto &l : clines) {
      if (l.rfind(prefix, 0) == 0) return l.substr(std::strlen(prefix));
    }
    return {};
  };
  Expect(find_cline("committed=") == "true" || find_cline("committed=") == "t",
         "observer should show committed after COMMIT");
  Expect(std::stoull(find_cline("epoch=")) > base_epoch,
         "commit must advance published epoch");
  Expect(find_cline("read=") == "77", "commit must publish staged toy value");

  Expect(RunSQL(tools, "DROP TABLE IF EXISTS test_txn CASCADE"), "cleanup failed");
  std::cout << "  txn_lifecycle: PASS (abort no-publish, commit publishes once)\n";
}

// ----------------------------------------------------------------
// 4) Visibility / isolation contract
//
// Proves: uncommitted writes are not visible to concurrent readers; after
// commit a fresh token/epoch differs from pre-commit.
// ----------------------------------------------------------------
void TestVisibilityContract(const Tools &tools) {
  Expect(RunSQL(tools, "SELECT accel_postgres_reset_observer()"), "reset failed");

  const char *staged_path = "/tmp/accel_pg_vis_staged";
  const char *go_path = "/tmp/accel_pg_vis_go";
  const char *done_path = "/tmp/accel_pg_vis_done";
  const char *sql_path = "/tmp/accel_pg_vis_writer.sql";
  const char *log_path = "/tmp/accel_pg_vis_bg.log";
  ::unlink(staged_path);
  ::unlink(go_path);
  ::unlink(done_path);

  Expect(RunSQL(tools, "SELECT accel_postgres_toy_build('vis_key', 100)"),
         "initial build failed");
  auto [ok_e1, epoch1_str] = QuerySQL(tools, "SELECT accel_postgres_toy_current_epoch()");
  Expect(ok_e1, "first epoch read failed");
  auto epoch1 = static_cast<uint64_t>(std::stoull(epoch1_str));
  Expect(epoch1 > 0, "initial epoch must be > 0");

  auto [ok_r1, val1] = QuerySQL(tools, "SELECT accel_postgres_toy_read()");
  Expect(ok_r1 && val1 == "100", "initial read should show 100");

  // File-synced concurrent writer (avoid sleep races).
  {
    FILE *sf = std::fopen(sql_path, "w");
    Expect(sf != nullptr, "failed to write visibility writer SQL");
    std::fprintf(sf,
                 "BEGIN;\n"
                 "SELECT accel_postgres_toy_build('vis_key', 200);\n"
                 "\\! touch %s\n"
                 "\\! while [ ! -f %s ]; do sleep 0.05; done\n"
                 "COMMIT;\n"
                 "\\! touch %s\n",
                 staged_path, go_path, done_path);
    std::fclose(sf);
  }
  std::string bg =
      "PGPASSWORD='" + tools.password + "' '" + tools.psql.string() +
      "' -h '" + tools.host + "' -p " + std::to_string(tools.port) +
      " -U '" + tools.user + "' -d '" + tools.database +
      "' -v ON_ERROR_STOP=1 -f '" + sql_path + "' >'" + log_path + "' 2>&1 &";
  (void)std::system(bg.c_str());

  for (int i = 0; i < 200 && ::access(staged_path, F_OK) != 0; ++i) {
    usleep(50 * 1000);
  }
  Expect(::access(staged_path, F_OK) == 0,
         "background writer never signaled staged (see /tmp/accel_pg_vis_bg.log)");

  auto [ok_e1b, epoch1b_str] = QuerySQL(tools, "SELECT accel_postgres_toy_current_epoch()");
  Expect(ok_e1b, "epoch read after uncommitted failed");
  auto epoch1b = static_cast<uint64_t>(std::stoull(epoch1b_str));
  Expect(epoch1b == epoch1, "epoch must NOT change for uncommitted write");

  auto [ok_r2, val2] = QuerySQL(tools, "SELECT accel_postgres_toy_read()");
  Expect(ok_r2 && val2 == "100",
         "uncommitted write must not be visible to concurrent reader");

  {
    FILE *f = std::fopen(go_path, "w");
    Expect(f != nullptr, "failed to create go sync file");
    std::fclose(f);
  }
  for (int i = 0; i < 200 && ::access(done_path, F_OK) != 0; ++i) {
    usleep(50 * 1000);
  }
  Expect(::access(done_path, F_OK) == 0,
         "background writer never committed (see /tmp/accel_pg_vis_bg.log)");

  auto [ok_e2, epoch2_str] = QuerySQL(tools, "SELECT accel_postgres_toy_current_epoch()");
  Expect(ok_e2, "epoch read after commit failed");
  auto epoch2 = static_cast<uint64_t>(std::stoull(epoch2_str));
  Expect(epoch2 > epoch1, "epoch must advance after commit");

  auto [ok_r3, val3] = QuerySQL(tools, "SELECT accel_postgres_toy_read()");
  Expect(ok_r3 && val3 == "200", "committed value 200 must be visible");

  ::unlink(staged_path);
  ::unlink(go_path);
  ::unlink(done_path);
  std::cout << "  visibility: PASS (uncommitted invisible, epoch advances after commit)\n";
}

// ----------------------------------------------------------------
// 5) StateStore round-trip
//
// Proves: Build/Stage -> TryPublish -> CheckValidity -> Read -> Invalidate,
// with value asserts and fail-closed after Invalidate.
// ----------------------------------------------------------------
void TestStateStoreRoundTrip(const Tools &tools) {
  Expect(RunSQL(tools, "SELECT accel_postgres_reset_observer()"), "reset failed");

  // Phase 1: Build and publish.
  Expect(RunSQL(tools, "SELECT accel_postgres_toy_build('rt_key', 777)"),
         "build failed");
  auto [ok_epoch0, epoch0_str] = QuerySQL(tools, "SELECT accel_postgres_toy_current_epoch()");
  Expect(ok_epoch0, "epoch read failed");
  uint64_t epoch0 = static_cast<uint64_t>(std::stoull(epoch0_str));
  Expect(epoch0 > 0, "epoch must be > 0 after build");

  // Phase 2: CheckValidity — use the current epoch as the visibility.
  // Format: "code,version" (e.g. "0,1" for Valid).
  auto [ok_cv0, cv0] = QuerySQL(tools,
      "SELECT accel_postgres_toy_check_validity('rt_key', " + epoch0_str + ")");
  Expect(ok_cv0, "check_validity query failed");
  // ValidityCode::Valid = 0
  int cv0_code = std::stoi(cv0.substr(0, cv0.find(',')));
  Expect(cv0_code == 0, "CheckValidity should be Valid after publish");

  // Key-scoped validity: unknown key must not report Valid.
  auto [ok_cv_miss, cv_miss] = QuerySQL(tools,
      "SELECT accel_postgres_toy_check_validity('no_such_key', " + epoch0_str + ")");
  Expect(ok_cv_miss, "check_validity miss query failed");
  int cv_miss_code = std::stoi(cv_miss.substr(0, cv_miss.find(',')));
  Expect(cv_miss_code != 0,
         "CheckValidity for unknown key must not be Valid");

  // Phase 3: Read back value.
  auto [ok_r1, val1] = QuerySQL(tools, "SELECT accel_postgres_toy_read()");
  Expect(ok_r1 && val1 == "777", "read back wrong: expected 777");

  // Phase 4: Invalidate.
  Expect(RunSQL(tools, "SELECT accel_postgres_toy_invalidate()"),
         "invalidate failed");

  // Phase 5: After invalidation, CheckValidity should NOT be Valid.
  // Use same epoch to test the Invalidated/Missing code, not epoch mismatch.
  auto [ok_cv1, cv1] = QuerySQL(tools,
      "SELECT accel_postgres_toy_check_validity('rt_key', " + std::to_string(epoch0) + ")");
  Expect(ok_cv1, "check_validity after invalidate failed");
  int cv1_code = std::stoi(cv1.substr(0, cv1.find(',')));
  Expect(cv1_code != 0,
         "CheckValidity must NOT be Valid after Invalidate");

  // Phase 6: Read should fail closed (no partitions available).
  // The post-invalidation store should have no valid partitions for a reader
  // at or below the committed epoch.
  auto [ok_r2, val2] = QuerySQL(tools, "SELECT accel_postgres_toy_read()");
  if (ok_r2) {
    // toy_read returns NULL (empty string from -t -A psql) when no partition.
    Expect(val2.empty(),
           "read after invalidate should return empty (no published state)");
  } else {
    // Read failure is also acceptable (NULL result from SQL on empty store).
  }

  std::cout << "  store_roundtrip: PASS (build, check_validity, invalidate, fail-closed read)\n";
}

} // anonymous namespace

int main() {
  // fail_closed: DiscoverTools must succeed or Fail
  auto tools = DiscoverTools();
  if (!tools.ok) Fail(tools.error.c_str());

  std::cout << "engine_contract[postgres] starting...\n";
  std::cout << "  host=" << tools.host << " port=" << tools.port << "\n";

  // Register all extension helper functions.
  RegisterHelpers(tools);

  TestNativeScanContract(tools);
  TestMutationCaptureContract(tools);
  TestTxnLifecycleContract(tools);
  TestVisibilityContract(tools);
  TestStateStoreRoundTrip(tools);

  std::cout << "engine_contract[postgres] PASS\n";
  return 0;
}
