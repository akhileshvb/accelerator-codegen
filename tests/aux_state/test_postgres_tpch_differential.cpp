// PostgreSQL TPC-H differential harness.
// Builds: test_postgres_tpch_differential
//
// Modes (ACCEL_PG_DIFF_MODE):
//   read (default) — ordered SQL vs accelerator/aux match (hard fail)
//   write_maint    — read match, then mutation + maintenance + re-match
// Env: TPCH_PG_DB, ACCEL_PG_DIFF_KIND=base|aux, ACCEL_WORKLOAD_KIND

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "postgres_tpch_common.hpp"

namespace {

using namespace accel::postgres;
using namespace accel::postgres::tpch_harness;
using accelerator_codegen::aux_state::engine::postgres::LookupPostgresQuery;

void RunWriteMaintDifferential(const Tools &tools, int query_num, bool prefer_aux) {
  Require(prefer_aux, "write_maint requires aux (ACCEL_PG_DIFF_KIND=aux)");
  RunReadDifferential(tools, query_num, prefer_aux);
  EnsureTransitionCaptureTriggers(tools, query_num);

  const auto &entry = LookupPostgresQuery("tpch", query_num);
  const std::string ref_sql = ConcreteBaseSql(query_num, entry);
  const std::string accel_sql = ConcreteAccelSql(query_num, entry, /*prefer_aux=*/true);

  WriteProbe probe;
  bool found = false;
  for (int cand = 0; cand < 64; ++cand) {
    try {
      probe = ResolveWriteProbe(tools, query_num, cand);
    } catch (const std::exception &) {
      break;
    }
    if (ProbeChangesBaseFingerprint(tools, ref_sql, probe.update_sql)) {
      found = true;
      break;
    }
  }
  Require(found, "invalidating probe did not change base SQL fingerprint for Q" +
                     std::to_string(query_num) +
                     " — write must impact this query's observable result");

  // One backend session: warm aux Runtime → mutate (triggers) → aux Execute must
  // observe mutation_count and rematch. Do NOT call an external rebuild helper.
  const char *force_action = std::getenv("ACCEL_FORCE_MAINTENANCE_ACTION");
  const std::string force_s = force_action ? force_action : "";
  std::ostringstream script;
  script << "LOAD '" << tools.extension_so << "';\n";
  if (!force_s.empty()) {
    script << "SELECT accel_postgres_set_force_maintenance_action("
           << SqlLiteral(force_s) << ");\n";
  } else {
    script << "SELECT accel_postgres_set_force_maintenance_action('');\n";
  }
  script << "SELECT '===WARM===' AS m;\n"
         << accel_sql << ";\n"
         << "SELECT '===MUT_BEFORE===' AS m;\n"
         << "SELECT accel_postgres_get_observer_mutation_count();\n"
         << probe.update_sql << ";\n"
         << "SELECT '===MUT_AFTER===' AS m;\n"
         << "SELECT accel_postgres_get_observer_mutation_count();\n"
         << "SELECT '===ACTION===' AS m;\n"
         << "SELECT accel_postgres_get_last_maintenance_action();\n"
         << "SELECT '===ACCEL===' AS m;\n"
         << accel_sql << ";\n"
         << "SELECT '===REF===' AS m;\n"
         << ref_sql << ";\n"
         << "SELECT '===DONE===' AS m;\n"
         << probe.restore_sql << ";\n";

  const std::string session = RunPsqlScript(tools, script.str());
  const auto mut_before = TrimLines(SectionAfter(session, "===MUT_BEFORE==="));
  const auto mut_after = TrimLines(SectionAfter(session, "===MUT_AFTER==="));
  // TrimLines keeps a trailing '\n' per line; take the first non-empty line.
  auto FirstLine = [](const std::string &s) {
    const auto lines = SplitLines(TrimLines(s));
    return lines.empty() ? std::string() : lines.front();
  };
  const auto action_s = FirstLine(SectionAfter(session, "===ACTION==="));
  const auto accel_out = SectionAfter(session, "===ACCEL===");
  const auto ref_out = SectionAfter(session, "===REF===");

  int before_n = 0;
  int after_n = 0;
  try {
    before_n = std::stoi(mut_before);
    after_n = std::stoi(mut_after);
  } catch (...) {
    throw std::runtime_error("failed to parse mutation counts before=[" + mut_before +
                             "] after=[" + mut_after + "]");
  }
  Require(after_n > before_n,
          "mutation observer did not advance (before=" + std::to_string(before_n) +
              " after=" + std::to_string(after_n) +
              ") — triggers must capture the write into aux maintenance");

  if (force_s.empty() || force_s == "ApplyDelta") {
    Require(action_s == "ApplyDelta",
            "expected ApplyDelta maintenance but got action=[" + action_s + "]" +
                (force_s.empty() ? "" : (" force=" + force_s)));
  } else if (force_s == "RebuildPartition") {
    Require(action_s == "RebuildPartition" || action_s == "RebuildAll",
            "forced RebuildPartition produced unexpected action=[" + action_s +
                "]");
  } else if (force_s == "RebuildAll") {
    Require(action_s == "RebuildAll",
            "forced RebuildAll produced unexpected action=[" + action_s + "]");
  }

  RequireOrderedMatch(ref_out, accel_out, "write_maint");
  std::cerr << "PASS write_maint differential Q" << query_num
            << " (invalidating write + mutation_count " << before_n << "->"
            << after_n << " + action=" << action_s
            << (force_s.empty() ? "" : (" force=" + force_s))
            << " + aux rematch)\n";
}


} // namespace


int main(int argc, char **argv) {
  try {
    int query_id = 3;
    if (argc >= 2) {
      query_id = std::stoi(argv[1]);
    }

    auto tools = DiscoverTools();
    Require(tools.ok, "PostgreSQL not available: " + tools.error);

    const char *tpch_db = std::getenv("TPCH_PG_DB");
    tools.database = (tpch_db && *tpch_db) ? tpch_db : "accel_tpch_test";

    std::cerr << "postgres differential: " << tools.host << ":" << tools.port
              << " db=" << tools.database << '\n';

    EnsureExtension(tools);

    // Ensure TPC-H tables exist (fail if not loaded).
    auto check = QuerySQL(tools, "SELECT COUNT(*) FROM lineitem");
    Require(check.first, "TPC-H tables missing — run setup_accel_postgres_tpch_test_db.sh");

    const bool prefer_aux = PreferAux(query_id);
    const char *mode = std::getenv("ACCEL_PG_DIFF_MODE");
    const std::string mode_s = mode ? mode : "read";

    if (mode_s == "write_maint") {
      RunWriteMaintDifferential(tools, query_id, prefer_aux);
    } else {
      RunReadDifferential(tools, query_id, prefer_aux);
      // Always also run write_maint for aux workloads unless explicitly disabled.
      if (prefer_aux && (!mode || mode_s == "read")) {
        const char *skip = std::getenv("ACCEL_PG_DIFF_SKIP_WRITE_MAINT");
        if (!(skip && std::string(skip) == "1")) {
          RunWriteMaintDifferential(tools, query_id, prefer_aux);
        }
      }
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "test_postgres_tpch_differential failed: " << error.what() << '\n';
    return 1;
  }
}
