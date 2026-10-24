// The test-suite format's loading and its small pieces (eval/harness/suite.hpp): no database needed.
#include <cstdlib>
#include <unistd.h>
#include <fstream>
#include <iostream>

#include "eval/harness/suite.hpp"

namespace suite = accelerator_codegen::suite;

namespace {

int failures = 0;

void Expect(bool ok, const char *what) {
  if (!ok) {
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
  }
}

void Write(const std::filesystem::path &path, const std::string &text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path) << text;
}

} // namespace

int main() {
  const auto items = suite::SplitTuple("'A, B', DATE '1995-01-01', f(1, 2), 0.05");
  Expect(items.size() == 4 && items[0] == "'A, B'" && items[2] == "f(1, 2)", "SplitTuple respects quotes and parentheses");
  Expect(suite::Project("'A', DATE '1995-01-01', DATE '1996-01-01'", 2) == "'A', DATE '1995-01-01'", "Project keeps the leading values");
  Expect(suite::Project("1", 3) == "1", "Project of a short tuple is the tuple");
  Expect(suite::IsRollbackTxn("-- rollback\nUPDATE t SET v = 0;") && suite::IsRollbackTxn("\n  -- ROLLBACK\nDELETE FROM t;"),
         "a first line `-- rollback` marks an aborted transaction");
  Expect(!suite::IsRollbackTxn("UPDATE t SET v = 0; -- rollback") && !suite::IsRollbackTxn(""), "only the first line counts");

  { const suite::Phase build("build"); }
  { const suite::Phase drift("drift"); }
  Expect(suite::PhaseFields().rfind("\"phases\":{\"build\":", 0) == 0 && suite::PhaseFields().find(",\"drift\":") != std::string::npos,
         "the phases of a run are reported in order");
  suite::DifferentialRun run;
  run.costs.push_back({"001.sql", 1, 2, 3, "RebuildAll"});
  run.costs.push_back({"002.sql", 1, 2, -1});
  Expect(run.CostFields().find("\"maint_ms\":3.000000,\"action\":\"RebuildAll\"}") != std::string::npos &&
             run.CostFields().find("\"action\":\"unknown\"") != std::string::npos,
         "each write's report carries the maintenance action, unknown when the engine does not tell");

  const auto dir = std::filesystem::temp_directory_path() / ("suite_format_" + std::to_string(::getpid()));
  Write(dir / "params.sql", "-- fixed\n'A', 1\n\n  'B', 2\n");
  Write(dir / "txns" / "002.sql", "DELETE FROM t;");
  Write(dir / "txns" / "001.sql", "INSERT INTO t VALUES (1);");
  Write(dir / "txns" / "003.sql", "-- rollback\nUPDATE t SET v = 1;");
  Write(dir / "drawn.sql", "'C', 3\n'A', 1\n");
  setenv("SUITE_FORMAT_TEST_EXTRA", (dir / "drawn.sql").c_str(), 1);
  auto spec = suite::LoadSpec(dir, "SUITE_FORMAT_TEST_REPLACE", "SUITE_FORMAT_TEST_EXTRA");
  Expect(spec.params.size() == 3 && spec.params[2] == "'C', 3", "fresh tuples are added once to the fixed ones");
  Expect(spec.txns.size() == 3 && spec.txn_names[0] == "001.sql" && spec.txn_names[2] == "003.sql", "transactions run in file order");
  suite::KeepTxns(spec, "003.sql,001.sql");
  Expect(spec.txns.size() == 2 && spec.txn_names[0] == "001.sql" && spec.txn_names[1] == "003.sql", "KeepTxns keeps the named ones in order");
  setenv("SUITE_FORMAT_TEST_REPLACE", (dir / "drawn.sql").c_str(), 1);
  Expect(suite::LoadSpec(dir, "SUITE_FORMAT_TEST_REPLACE", "SUITE_FORMAT_TEST_NONE").params.size() == 2, "a replacing file is the tuples");
  std::filesystem::remove_all(dir);
  std::cerr << (failures ? "FAILED\n" : "PASS test_suite_format\n");
  return failures ? 1 : 0;
}
