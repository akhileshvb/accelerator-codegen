// PostgreSQL TPC-H differential harness.
// Builds: test_postgres_tpch_differential
//
// Modes (ACCEL_PG_DIFF_MODE):
//   read (default) — ordered SQL vs accelerator/aux match (hard fail)
//   write_maint    — read match, then mutation + maintenance + re-match
// Env: TPCH_PG_DB, ACCEL_PG_DIFF_KIND=base|aux, ACCEL_WORKLOAD_KIND

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "backends/postgres/tests/harness/postgres_tpch_harness.hpp"

namespace {

using accel::postgres::Tools;
using namespace accel::postgres::tpch_harness;
using accelerator_codegen::aux_state::engine::postgres::LookupPostgresQuery;
using accelerator_codegen::aux_state::engine::postgres::PostgresSharedQueryCatalogEntry;

void RunReadDifferential(const Tools &tools, int query_num, bool prefer_aux) {
  const auto &entry = LookupPostgresQuery("tpch", query_num);
  const std::string ref_sql = ConcreteBaseSql(query_num, entry);
  const std::string accel_sql = ConcreteAccelSql(query_num, entry, prefer_aux);

  std::cerr << "differential(read): tpch Q" << query_num
            << (prefer_aux ? " aux" : " base") << '\n';
  std::cerr << "  reference: " << ref_sql << '\n';
  std::cerr << "  accel: " << accel_sql << '\n';

  RequireQueryMatch(tools, query_num, prefer_aux, "read");
  std::cerr << "PASS read differential Q" << query_num << '\n';
}

using accel::tpch_params::WriteProbe;

WriteProbe ResolveWriteProbe(const Tools &tools, int query_num, int candidate_offset) {
  return accel::tpch_params::ResolveWriteProbe(
      query_num, candidate_offset, [&](const std::string &sql) { return QuerySQL(tools, sql); });
}

std::string SectionAfter(const std::string &blob, const std::string &marker) {
  const auto pos = blob.find(marker);
  Require(pos != std::string::npos, "missing marker " + marker);
  auto start = pos + marker.size();
  while (start < blob.size() && (blob[start] == '\n' || blob[start] == '\r')) {
    ++start;
  }
  const auto next = blob.find("===", start);
  if (next == std::string::npos) {
    return blob.substr(start);
  }
  return blob.substr(start, next - start);
}

bool ProbeChangesBaseFingerprint(const Tools &tools, const std::string &base_sql,
                                 const std::string &update_sql) {
  // Must be one backend session: separate psql -c calls do not share a txn.
  std::ostringstream script;
  script << "SELECT '===BEFORE===' AS m;\n"
         << base_sql << ";\n"
         << "BEGIN;\n"
         << update_sql << ";\n"
         << "SELECT '===AFTER===' AS m;\n"
         << base_sql << ";\n"
         << "ROLLBACK;\n";
  const std::string out = RunPsqlScript(tools, script.str());
  return TrimLines(SectionAfter(out, "===BEFORE===")) !=
         TrimLines(SectionAfter(out, "===AFTER==="));
}

void RunWriteMaintDifferential(const Tools &tools, int query_num, bool prefer_aux,
                               const std::string &force_action) {
  Require(prefer_aux, "write_maint requires aux (ACCEL_PG_DIFF_KIND=aux)");
  RunReadDifferential(tools, query_num, prefer_aux);
  EnsureMutationTriggers(tools);

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

  // Commits shipped to the accelerators' shard workers, before and after the write.
  const std::string stats = "SELECT accel_shard_stats();\n";
  std::ostringstream script;
  script << "LOAD '" << tools.extension_so << "';\n"
         << "SELECT '===WARM===' AS m;\n"
         << accel_sql << ";\n"
         // A cold read answers from SQL while the build runs: wait for the state.
         << "DO $$ BEGIN FOR i IN 1..1200 LOOP "
            "EXIT WHEN substring(accel_shard_stats() from 'synced=([0-9]+)')::bigint >= 1; "
            "PERFORM pg_sleep(0.05); END LOOP; END $$;\n";
  // The accelerator's worker exists once it was read: force from here on.
  script << "SELECT accel_set_force_maintenance_action("
         << SqlLiteral(force_action) << ");\n"
         << "SELECT '===MUT_BEFORE===' AS m;\n"
         << stats << probe.update_sql << ";\n"
         // The read answers from the state once it can serve (or from SQL when
         // waiting costs more); the worker applies the commit and publishes its
         // decision either way: wait for that.
         << "SELECT '===ACCEL===' AS m;\n"
         << accel_sql << ";\n"
         << "DO $$ DECLARE s text; BEGIN FOR i IN 1..600 LOOP s := accel_shard_stats(); "
            "EXIT WHEN substring(s from 'applied=([0-9]+)')::bigint >= substring(s from 'commits=([0-9]+)')::bigint; "
            "PERFORM pg_sleep(0.05); END LOOP; END $$;\n"
         << "SELECT pg_sleep(0.5);\n"
         << "SELECT '===MUT_AFTER===' AS m;\n"
         << stats
         << "SELECT '===ACTION===' AS m;\n"
         << "SELECT accel_get_last_maintenance_action();\n"
         << "SELECT '===REF===' AS m;\n"
         << ref_sql << ";\n"
         << "SELECT '===DONE===' AS m;\n"
         << probe.restore_sql << ";\n"
         << "SELECT accel_set_force_maintenance_action('');\n";

  const std::string session = RunPsqlScript(tools, script.str());
  const auto mut_before = TrimLines(SectionAfter(session, "===MUT_BEFORE==="));
  const auto mut_after = TrimLines(SectionAfter(session, "===MUT_AFTER==="));
  const auto action_out = TrimLines(SectionAfter(session, "===ACTION==="));
  const auto accel_out = SectionAfter(session, "===ACCEL===");
  const auto ref_out = SectionAfter(session, "===REF===");

  auto commits_of = [](const std::string &text) {
    const auto pos = text.find("commits=");
    if (pos == std::string::npos)
      throw std::runtime_error("failed to parse shard stats [" + text + "]");
    return std::stoi(text.substr(pos + 8));
  };
  const int before_n = commits_of(mut_before);
  const int after_n = commits_of(mut_after);
  Require(after_n > before_n,
          "no commit reached the shard worker (before=" + std::to_string(before_n) +
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
            << " (invalidating write + shipped commits " << before_n << "->" << after_n
            << " + aux rematch action=[" << action << "])\n";
}

std::vector<std::string> MaintenanceStrats() {
  const char *raw = std::getenv("ACCEL_PG_DIFF_MAINT_STRATS");
  const char *skip = std::getenv("ACCEL_PG_DIFF_SKIP_MAINT_STRATS");
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
    std::cerr << "postgres differential: " << tools.host << ":" << tools.port
              << " db=" << tools.database << '\n';

    const bool prefer_aux = PreferAux(query_id);
    const char *mode = std::getenv("ACCEL_PG_DIFF_MODE");
    const std::string mode_s = mode ? mode : "read";

    if (mode_s == "write_maint") {
      RunWriteMixAndStrats(tools, query_id, prefer_aux);
    } else {
      RunReadDifferential(tools, query_id, prefer_aux);
      if (prefer_aux && (!mode || mode_s == "read")) {
        const char *skip = std::getenv("ACCEL_PG_DIFF_SKIP_WRITE_MAINT");
        if (!(skip && std::string(skip) == "1")) {
          RunWriteMixAndStrats(tools, query_id, prefer_aux);
        }
      }
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "test_postgres_tpch_differential failed: " << error.what() << '\n';
    return 1;
  }
}
