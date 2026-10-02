// MySQL TPC-H differential harness.
// Builds: test_mysql_tpch_differential
//
// Modes (ACCEL_MYSQL_DIFF_MODE):
//   read (default) — ordered SQL vs accelerator/aux match (hard fail)
//   write_maint    — read match, then mutation + maintenance + re-match
// Env: TPCH_MYSQL_DB, ACCEL_MYSQL_DIFF_KIND=base|aux, ACCEL_WORKLOAD_KIND

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "aux_state/mysql_tpch_harness.hpp"
#include "aux_state/mysql_tpch_write_probes.hpp"

namespace {

using accel::mysql::Tools;
using namespace accel::mysql::tpch_harness;
using accelerator_codegen::aux_state::engine::mysql::LookupMysqlQuery;
using accelerator_codegen::aux_state::engine::mysql::MysqlSharedQueryCatalogEntry;

void RunReadDifferential(const Tools &tools, int query_num, bool prefer_aux) {
  const auto &entry = LookupMysqlQuery("tpch", query_num);
  const std::string ref_sql = ConcreteBaseSql(query_num, entry);
  const std::string accel_sql = ConcreteAccelSql(query_num, entry, prefer_aux);

  std::cerr << "differential(read): tpch Q" << query_num
            << (prefer_aux ? " aux" : " base") << '\n';
  std::cerr << "  reference: " << ref_sql << '\n';
  std::cerr << "  accel: " << accel_sql << '\n';

  RequireQueryMatch(tools, query_num, prefer_aux, "read");
  std::cerr << "PASS read differential Q" << query_num << '\n';
}



void RunWriteMaintDifferential(const Tools &tools, int query_num, bool prefer_aux,
                               const std::string &force_action) {
  Require(prefer_aux, "write_maint requires aux (ACCEL_MYSQL_DIFF_KIND=aux)");
  RunReadDifferential(tools, query_num, prefer_aux);
  EnsureMutationTriggers(tools);

  const auto &entry = LookupMysqlQuery("tpch", query_num);
  const std::string ref_sql = ConcreteBaseSql(query_num, entry);
  const std::string accel_sql = ConcreteAccelSql(query_num, entry, /*prefer_aux=*/true);

  WriteProbe probe = PickInvalidatingWriteProbe(tools, query_num, ref_sql);

  // Separate CLI invocations still share plugin process state. Avoid one
  // multi-statement script: mysql continues after errors and BEGIN/ROLLBACK
  // is not reliable for fingerprint/observer sequencing.
  Require(QuerySQL(tools, "SELECT mysql_accel_reset_observer()").first,
          "reset observer failed");
  Require(QuerySQL(tools, "SELECT mysql_accel_set_force_maintenance_action(" +
                              SqlLiteral(force_action) + ")")
              .first,
          "set force maintenance failed");
  auto warm = QuerySQL(tools, accel_sql);
  Require(warm.first, "warm aux failed: " + accel_sql + "\n" + warm.second);

  auto mut_before_q =
      QuerySQL(tools, "SELECT mysql_accel_observer_mutation_count()");
  Require(mut_before_q.first, "mutation count before failed");
  Require(RunSQL(tools, probe.update_sql),
          "write_maint update failed: " + probe.update_sql);
  auto mut_after_q =
      QuerySQL(tools, "SELECT mysql_accel_observer_mutation_count()");
  Require(mut_after_q.first, "mutation count after failed");
  auto accel = QuerySQL(tools, accel_sql);
  Require(accel.first, "aux rematch query failed: " + accel_sql + "\n" +
                           accel.second);
  auto action_q =
      QuerySQL(tools, "SELECT mysql_accel_get_last_maintenance_action()");
  Require(action_q.first, "last maintenance action failed");
  auto ref = QuerySQL(tools, ref_sql);
  Require(ref.first, "reference rematch query failed: " + ref_sql + "\n" +
                         ref.second);
  Require(RunSQL(tools, probe.restore_sql),
          "write_maint restore failed: " + probe.restore_sql);
  QuerySQL(tools, "SELECT mysql_accel_set_force_maintenance_action('')");
  RequireQueryMatch(tools, query_num, /*prefer_aux=*/true,
                    force_action.empty() ? "post_restore"
                                         : ("post_restore_" + force_action));

  const auto mut_before = TrimLines(mut_before_q.second);
  const auto mut_after = TrimLines(mut_after_q.second);
  const auto action_out = TrimLines(action_q.second);
  const auto accel_out = accel.second;
  const auto ref_out = ref.second;

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

  std::string action = action_out;
  while (!action.empty() && (action.back() == '\n' || action.back() == '\r'))
    action.pop_back();
  auto ActionOk = [](const std::string &got, std::initializer_list<const char *> ok) {
    for (const char *a : ok) {
      if (got == a)
        return true;
    }
    return false;
  };
  // Rematch is the correctness bar. Forced actions may escalate when a module
  // has no ApplyDelta path, or when Postgres defers Rebuild* to EnsureBuilt.
  if (force_action == "ApplyDelta") {
    Require(ActionOk(action, {"ApplyDelta", "RebuildPartition", "RebuildAll",
                              "InvalidateAndRebuildOnRead"}),
            "forced ApplyDelta produced unexpected action=[" + action + "]");
  } else if (force_action == "RebuildPartition") {
    Require(ActionOk(action, {"RebuildPartition", "RebuildAll",
                              "InvalidateAndRebuildOnRead"}),
            "forced RebuildPartition produced unexpected action=[" + action + "]");
  } else if (force_action == "RebuildAll") {
    Require(ActionOk(action, {"RebuildAll", "InvalidateAndRebuildOnRead"}),
            "forced RebuildAll produced unexpected action=[" + action + "]");
  } else {
    Require(!action.empty() && action != "None",
            "policy write_maint recorded no maintenance action");
  }

  RequireOrderedMatch(ref_out, accel_out,
                      force_action.empty() ? "write_maint"
                                           : ("write_maint_" + force_action));
  std::cerr << "PASS write_maint differential Q" << query_num
            << (force_action.empty() ? "" : (" force=" + force_action))
            << " (invalidating write + mutation_count " << before_n << "->" << after_n
            << " + aux rematch action=[" << action << "])\n";
}

std::vector<std::string> MaintenanceStrats() {
  const char *raw = std::getenv("ACCEL_MYSQL_DIFF_MAINT_STRATS");
  const char *skip = std::getenv("ACCEL_MYSQL_DIFF_SKIP_MAINT_STRATS");
  if (skip && std::string(skip) == "1")
    return {};
  std::string src = raw && *raw ? raw : "ApplyDelta,RebuildPartition,RebuildAll";
  if (src == "none" || src == "None")
    return {};
  std::vector<std::string> out;
  std::stringstream stream(src);
  std::string token;
  while (std::getline(stream, token, ',')) {
    if (!token.empty())
      out.push_back(token);
  }
  return out;
}

void RunWriteMixAndStrats(const Tools &tools, int query_id, bool prefer_aux) {
  RunWriteMaintDifferential(tools, query_id, prefer_aux, "");
  const char *force_env = std::getenv("ACCEL_FORCE_MAINTENANCE_ACTION");
  if (force_env && *force_env) {
    RunWriteMaintDifferential(tools, query_id, prefer_aux, force_env);
    return;
  }
  for (const auto &strat : MaintenanceStrats()) {
    RunWriteMaintDifferential(tools, query_id, prefer_aux, strat);
  }
}

} // namespace

int main(int argc, char **argv) {
  try {
    int query_id = 1;
    if (argc >= 2) {
      query_id = std::stoi(argv[1]);
    }

    auto tools = ConnectTpch();
    std::cerr << "mysql differential: " << tools.host << ":" << tools.port
              << " db=" << tools.database << '\n';

    const bool prefer_aux = PreferAux(query_id);
    const char *mode = std::getenv("ACCEL_MYSQL_DIFF_MODE");
    const std::string mode_s = mode ? mode : "read";

    if (mode_s == "write_maint") {
      RunWriteMixAndStrats(tools, query_id, prefer_aux);
    } else {
      RunReadDifferential(tools, query_id, prefer_aux);
      if (prefer_aux && (!mode || mode_s == "read")) {
        const char *skip = std::getenv("ACCEL_MYSQL_DIFF_SKIP_WRITE_MAINT");
        if (!(skip && std::string(skip) == "1")) {
          RunWriteMixAndStrats(tools, query_id, prefer_aux);
        }
      }
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "test_mysql_tpch_differential failed: " << error.what() << '\n';
    return 1;
  }
}
