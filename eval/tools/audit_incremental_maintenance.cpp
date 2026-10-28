// Audits incremental maintenance on DuckDB: one accelerator, one write
// workload; after every commit a read, then what the runtime did for it.
//
// Usage: audit_incremental_maintenance <db> <workload> <query>...
//   workloads: rf-small (10-order RF1/RF2 chunks), rf-official (1500-order
//   chunks: 0.1% of SF1; AUDIT_RF_ORDERS sets the size, 1500 x SF is the official one), gate-mix (the slot gates' lineitem writes),
//   fuzz-all (writes on every TPC-H table), sql-mix (the differential's
//   targeted + per-table writes), sec-probes (SEC-EDGAR catalog probes, handed
//   to the accelerator by hand), sec-sql (the same probes as SQL writes the
//   engine must capture), scale-rows (the rows the policy sees, no writes), sec-mix (updates, inserts and deletes on every
//   SEC-EDGAR table)
// Every answer is checked against the base query after each commit.
// Output: one tab-separated line per commit, then one SUMMARY line.
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "catalog/query_catalog.hpp"
#include "eval/harness/differential.hpp"
#include "eval/harness/tpch_rf.hpp"
#include "eval/tests/test_support/db_paths.hpp"
#include "eval/tests/tpch_fuzz_writes.hpp"

namespace {

namespace engine = accelerator_codegen::aux_state::engine;
namespace harness = accelerator_codegen::aux_state::harness;
namespace ts = accelerator_codegen::test_support;
using engine::duckdb::DuckdbRuntime;

constexpr int kFuzzCommits = 60;

struct Totals {
  int commits = 0, applied = 0, rebuilt = 0, ignored = 0;
  uint64_t fallbacks = 0, gaps = 0;
};

class Audit {
public:
  Audit(DuckdbRuntime &runtime_p, const engine::SharedQueryCatalogEntry &entry_p, std::string workload_p)
      : runtime(runtime_p), entry(entry_p), workload(std::move(workload_p)) {}

  void CheckAnswers(duckdb::Connection &connection_p) { connection = &connection_p; }

  // `write` commits one transaction.
  template <typename Write> void Step(const std::string &name, Write &&write) {
    const auto before = runtime.Stats();
    write();
    runtime.ExecuteCollect(); // waits for any rebuild the commit caused
    const auto after = runtime.Stats();
    std::string wrong;
    if (connection) {
      try {
        harness::RequireSharedMatchesBase(*connection, runtime, entry);
      } catch (const std::exception &e) {
        wrong = std::string(e.what()).substr(0, 120);
      }
    }
    Record(name, before, after, wrong);
  }

  void Summary(double build_us) const {
    std::cout << "SUMMARY\t" << entry.workload_name << "\tQ" << entry.query_id << '\t' << workload << "\tcommits="
              << totals.commits << "\tincremental=" << totals.applied << "\tfull_rebuilds=" << totals.rebuilt
              << "\tignored=" << totals.ignored << "\tgaps=" << totals.gaps << "\tfallbacks=" << totals.fallbacks
              << "\twrong_answers=" << wrong_answers
              << "\tinitial_build_us=" << static_cast<long>(build_us) << std::endl;
  }

private:
  void Record(const std::string &name, const accelerator_codegen::aux_state::runtime::AcceleratorStats &before,
              const accelerator_codegen::aux_state::runtime::AcceleratorStats &after, const std::string &wrong) {
    const auto applied = after.commits_applied - before.commits_applied;
    const auto builds = after.builds - before.builds;
    const char *outcome = builds ? "REBUILD" : applied ? "incremental" : "ignored";
    totals.commits++;
    totals.rebuilt += builds ? 1 : 0;
    totals.applied += !builds && applied ? 1 : 0;
    totals.ignored += !builds && !applied ? 1 : 0;
    totals.gaps += after.gaps - before.gaps;
    totals.fallbacks += after.reads_fallback - before.reads_fallback;
    std::cout << entry.workload_name << "\tQ" << entry.query_id << '\t' << workload << '\t' << name << '\t' << outcome
              << "\tapplied=" << applied << "\tbuilds=" << builds
              << "\tcatch_up=" << after.catch_up_commits - before.catch_up_commits
              << "\tfallbacks=" << after.reads_fallback - before.reads_fallback
              << "\tapply_us=" << static_cast<long>(after.apply_us - before.apply_us)
              << "\trows=" << after.rows_applied - before.rows_applied;
    if (after.commits_stale > before.commits_stale) {
      std::cout << "\tstale=\"" << after.last_stale_reason << '"';
    }
    if (const auto &d = after.last_decision; d && (applied || builds)) {
      std::cout << "\taction=" << accelerator_codegen::aux_state::maintenance::MaintenanceActionName(d->staged_action)
                << "\tselected=" << accelerator_codegen::aux_state::maintenance::MaintenanceActionName(d->selected)
                << "\tparts=" << d->affected_partition_count
                << "\tpred_delta_us=" << d->predicted_apply_delta_us << "\tpred_part_us=" << d->predicted_rebuild_partition_us
                << "\tpred_all_us=" << d->predicted_rebuild_all_us;
    }
    if (!wrong.empty()) {
      wrong_answers++;
      std::cout << "\tWRONG_ANSWER=\"" << wrong << '"';
    }
    std::cout << std::endl;
  }

  DuckdbRuntime &runtime;
  const engine::SharedQueryCatalogEntry &entry;
  std::string workload;
  Totals totals;
  duckdb::Connection *connection = nullptr;
  int wrong_answers = 0;
};

void RunRf(duckdb::Connection &connection, DuckdbRuntime &runtime, Audit &audit, int orders, int chunks) {
  for (int seq = 0; seq < chunks; ++seq) {
    audit.Step(std::string(seq % 2 ? "rf1-insert" : "rf2-delete") + "#" + std::to_string(seq),
               [&] { harness::ApplyTpchRfChunk(connection, runtime, seq, orders); });
  }
}

void RunFuzz(duckdb::Connection &connection, DuckdbRuntime &runtime, Audit &audit, bool lineitem_only) {
  ts::TpchFuzzWrites fuzz(ts::FuzzDialect::DuckDB, 1, [&](const std::string &sql) {
    auto r = connection.Query(sql);
    if (!r || r->HasError()) {
      throw std::runtime_error(sql + ": " + (r ? r->GetError() : "null"));
    }
    auto v = r->RowCount() ? r->GetValue(0, 0) : duckdb::Value();
    return v.IsNull() ? int64_t{0} : v.GetValue<int64_t>();
  });
  for (int i = 0; i < kFuzzCommits; ++i) {
    const auto sql = lineitem_only ? fuzz.Next({1, 0, 0, 0, 0, 0, 0, 0}) : fuzz.Next();
    audit.Step(sql.substr(0, 60), [&] { runtime.ApplyWrite(sql); });
  }
}

// SEC-EDGAR tables reach the accelerator only through the probes' hand-built
// mutations, so every write is a probe (repeated: they toggle or advance).
void RunSecProbes(duckdb::Connection &connection, DuckdbRuntime &runtime, Audit &audit,
                  const engine::SharedQueryCatalogEntry &entry) {
  auto probes = engine::CollectInvalidatingSharedProbes(connection, entry, 256, 3);
  constexpr int kRounds = 3;
  for (int round = 0; round < kRounds; ++round) {
    for (std::size_t i = 0; i < probes.size(); ++i) {
      audit.Step("probe" + std::to_string(i) + "#" + std::to_string(round), [&] { runtime.Apply(probes[i]); });
    }
  }
}

void RunSecSql(duckdb::Connection &connection, DuckdbRuntime &runtime, Audit &audit,
               const engine::SharedQueryCatalogEntry &entry) {
  const auto probes = engine::CollectInvalidatingSharedProbes(connection, entry, 256, 3);
  for (std::size_t i = 0; i < probes.size(); ++i) {
    audit.Step("probe" + std::to_string(i), [&] { runtime.ApplyWrite(probes[i].update_sql); });
    audit.Step("restore" + std::to_string(i), [&] { runtime.ApplyWrite(probes[i].restore_sql); });
  }
}

// Writes of each kind on each SEC-EDGAR table; those a query does not read
// are ignored by it.
void RunSecMix(DuckdbRuntime &runtime, Audit &audit) {
  const std::vector<std::pair<std::string, std::string>> writes = {
      {"num value update", "UPDATE num SET value = value * 1.5 WHERE rowid IN (SELECT rowid FROM num USING SAMPLE 3 ROWS)"},
      {"num uom update", "UPDATE num SET uom = 'EUR' WHERE rowid IN (SELECT rowid FROM num USING SAMPLE 3 ROWS)"},
      {"num insert", "INSERT INTO num SELECT * FROM num USING SAMPLE 5 ROWS"},
      {"num delete", "DELETE FROM num WHERE rowid IN (SELECT rowid FROM num USING SAMPLE 5 ROWS)"},
      {"sub name update", "UPDATE sub SET name = name || ' x' WHERE rowid IN (SELECT rowid FROM sub USING SAMPLE 2 ROWS)"},
      {"sub fy update", "UPDATE sub SET fy = fy + 1 WHERE rowid IN (SELECT rowid FROM sub USING SAMPLE 2 ROWS)"},
      {"sub sic update", "UPDATE sub SET sic = sic + 1 WHERE rowid IN (SELECT rowid FROM sub USING SAMPLE 2 ROWS)"},
      {"sub delete", "DELETE FROM sub WHERE rowid IN (SELECT rowid FROM sub USING SAMPLE 2 ROWS)"},
      {"pre stmt update", "UPDATE pre SET stmt = 'BS' WHERE rowid IN (SELECT rowid FROM pre USING SAMPLE 3 ROWS)"},
      {"pre plabel update", "UPDATE pre SET plabel = plabel || ' x' WHERE rowid IN (SELECT rowid FROM pre USING SAMPLE 3 ROWS)"},
      {"pre insert", "INSERT INTO pre SELECT * FROM pre USING SAMPLE 5 ROWS"},
      {"pre delete", "DELETE FROM pre WHERE rowid IN (SELECT rowid FROM pre USING SAMPLE 5 ROWS)"},
      {"tag label update", "UPDATE tag SET tlabel = tlabel || ' x' WHERE rowid IN (SELECT rowid FROM tag USING SAMPLE 3 ROWS)"},
      {"tag delete", "DELETE FROM tag WHERE rowid IN (SELECT rowid FROM tag USING SAMPLE 2 ROWS)"},
  };
  for (const auto &[name, sql] : writes) {
    audit.Step(name, [&] { runtime.ApplyWrite(sql); });
  }
}

// Rows the policy sees, from the database file itself.
void PrintScaleRows(const std::string &db, const std::string &kind, int query) {
  duckdb::DBConfig config;
  config.options.access_mode = duckdb::AccessMode::READ_ONLY;
  duckdb::DuckDB database(db, &config);
  duckdb::Connection connection(database);
  const auto &entry = engine::LookupSharedQuery(kind, query);
  DuckdbRuntime runtime(connection, entry, 10.0);
  std::cout << "SCALE_ROWS\t" << entry.workload_name << "\tQ" << query << '\t' << runtime.DataScaleRows()
            << std::endl;
}

void AuditQuery(const std::string &db, const std::string &workload, const std::string &kind,
                int query) {
  if (workload == "scale-rows") {
    return PrintScaleRows(db, kind, query);
  }
  ts::ScratchDir scratch("audit_maintenance");
  const auto copy = ts::PrivateDbCopy(db, scratch);
  duckdb::DuckDB database(copy.c_str());
  duckdb::Connection connection(database);
  const auto &entry = engine::LookupSharedQuery(kind, query);
  DuckdbRuntime runtime(connection, entry, 10.0);
  const auto build_start = std::chrono::steady_clock::now();
  runtime.Build();
  const double build_us =
      std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - build_start).count();
  Audit audit(runtime, entry, workload);
  audit.CheckAnswers(connection);
  if (workload == "rf-small") {
    RunRf(connection, runtime, audit, 10, 6);
  } else if (workload == "rf-official") {
    RunRf(connection, runtime, audit, ts::EnvOrInt("AUDIT_RF_ORDERS", 1500), 2);
  } else if (workload == "gate-mix") {
    RunFuzz(connection, runtime, audit, true);
  } else if (workload == "fuzz-all") {
    RunFuzz(connection, runtime, audit, false);
  } else if (workload == "sql-mix") {
    for (const auto &write : harness::TpchSqlWrites(connection, entry, 2)) {
      audit.Step(write.name, [&] { runtime.ApplyWrite(write.sql); });
    }
  } else if (workload == "sec-mix") {
    RunSecMix(runtime, audit);
  } else if (workload == "sec-sql") {
    RunSecSql(connection, runtime, audit, entry);
  } else if (workload == "sec-probes") {
    RunSecProbes(connection, runtime, audit, entry);
  } else {
    throw std::invalid_argument("unknown workload " + workload);
  }
  audit.Summary(build_us);
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 4) {
    std::cerr << "usage: audit_incremental_maintenance <db> <workload> <query>...\n";
    return 2;
  }
  const std::string workload = argv[2];
  const std::string kind = workload.rfind("sec-", 0) == 0 ? "sec_edgar" : "tpch";
  int status = 0;
  for (int i = 3; i < argc; ++i) {
    try {
      AuditQuery(argv[1], workload, kind, std::atoi(argv[i]));
    } catch (const std::exception &e) {
      std::cout << "ERROR\tQ" << argv[i] << '\t' << workload << '\t' << e.what() << std::endl;
      status = 1;
    }
  }
  return status;
}
