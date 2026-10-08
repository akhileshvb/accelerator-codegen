// End-to-end differential test for the SEC-EDGAR Q4 aux module on real DuckDB.
//
//   test_sec_edgar_q4_duckdb_differential synthetic [seeds] [steps] [parallel]
//   test_sec_edgar_q4_duckdb_differential file <db_copy> [ops] [parallel]
//
// synthetic: random in-memory tables (NULL keys, boundary sics, duplicate pre
//   rows, ...), random multi-mutation transactions over num/sub/tag/pre
//   (INSERT/DELETE/UPDATE incl. key and filter columns that move rows in and
//   out of the result), commits and rollbacks.  After every transaction the aux
//   output must equal the plain DuckDB execution of benchmarks/sec_edgar/
//   queries/q4.sql.
// file: the same invariants on a (writable copy of a) real SEC-EDGAR database
//   with hand-picked, high-impact writes.
//
// Q4 has no template parameters: every constant in q4.sql is a literal, so
// the module state is parameter independent.  The data-domain edge values of
// those literals (sic 3999/4000/4999/5000, uom USD/usd/EUR, stmt EQ/NULL,
// abstract 0/1/NULL, tlabel NULL) are covered by the generator.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "accelerator_codegen/harness/differential.hpp"
#include "accelerator_codegen/stateful/core/aux_registry.hpp"
#include "accelerator_codegen/stateful/duckdb/duckdb_aux_backend.hpp"
#include "accelerator_codegen/stateful/duckdb/duckdb_state_store.hpp"
#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"
#include "sec_edgar/q4/duckdb/aux/duckdb_scan.hpp"
#include "sec_edgar/q4/duckdb/aux/module.hpp"
#ifdef ACCEL_ENABLE_PARALLEL_AUX
#include "sec_edgar/q4/duckdb/aux/parallel/module.hpp"
#endif
#include "test_support/sec_edgar_q4_txn_gen.hpp"

namespace core = accelerator_codegen::aux_state::core;
namespace aas = accelerator_codegen::aux_state;
namespace engine = accelerator_codegen::aux_state::engine;
namespace harness = accelerator_codegen::aux_state::harness;
using accelerator_codegen::aux_state::query::SecEdgarQ4Config;
using accelerator_codegen::aux_state::query::SecEdgarQ4DuckDbScan;
using accelerator_codegen::aux_state::query::SecEdgarQ4Module;

namespace {

using Clock = std::chrono::steady_clock;

double Ms(Clock::time_point since) {
  return std::chrono::duration<double, std::milli>(Clock::now() - since).count();
}

void Require(bool ok, const std::string &what) {
  if (!ok) {
    std::cerr << "FAIL: " << what << '\n';
    std::abort();
  }
}

void Exec(duckdb::Connection &con, const std::string &sql) {
  auto r = con.Query(sql);
  Require(r && !r->HasError(), sql + " -> " + (r ? r->GetError() : "null result"));
}

core::AuxValue ToAux(const duckdb::Value &v) {
  using duckdb::LogicalTypeId;
  if (v.IsNull()) {
    switch (v.type().id()) {
    case LogicalTypeId::DOUBLE:
      return core::AuxValue::Null(core::AuxType::Double);
    case LogicalTypeId::INTEGER:
    case LogicalTypeId::BIGINT:
      return core::AuxValue::Null(core::AuxType::Int64);
    default:
      return core::AuxValue::Null(core::AuxType::String);
    }
  }
  switch (v.type().id()) {
  case LogicalTypeId::DOUBLE:
    return core::AuxValue::Of(core::AuxType::Double, v.GetValue<double>());
  case LogicalTypeId::INTEGER:
  case LogicalTypeId::BIGINT:
    return core::AuxValue::Of(core::AuxType::Int64, v.GetValue<int64_t>());
  default:
    return core::AuxValue::Of(core::AuxType::String, v.ToString());
  }
}

std::vector<core::AuxRow> Fetch(duckdb::Connection &con, const std::string &sql, bool drop_first = false) {
  auto r = con.Query(sql);
  Require(r && !r->HasError(), sql + " -> " + (r ? r->GetError() : "null"));
  std::vector<core::AuxRow> rows;
  for (duckdb::idx_t i = 0; i < r->RowCount(); ++i) {
    core::AuxRow row;
    for (duckdb::idx_t c = drop_first ? 1 : 0; c < r->ColumnCount(); ++c) {
      row.values.push_back(ToAux(r->GetValue(c, i)));
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

struct MaterializeSink final : core::AuxRowSink {
  void Begin(const core::OutputSchema &) override {}
  void WriteRow(const core::AuxRow &row) override { rows.push_back(row); }
  void End() override {}
  std::vector<core::AuxRow> rows;
};

struct Builder final : core::StateBuilder {
  void Write(core::StateImage image) override { images.push_back(std::move(image)); }
  void Finish() override { finished = true; }
  std::vector<core::StateImage> images;
  bool finished = false;
};

class CountingModule : public SecEdgarQ4Module {
public:
  using SecEdgarQ4Module::SecEdgarQ4Module;
  void Build(core::AuxEngineServices &s, core::StateBuilder &b) override {
    ++builds;
    SecEdgarQ4Module::Build(s, b);
  }
  int builds = 0;
};

#ifdef ACCEL_ENABLE_PARALLEL_AUX
class CountingParallelModule : public accelerator_codegen::aux_state::query::SecEdgarQ4ParallelModule {
public:
  using SecEdgarQ4ParallelModule::SecEdgarQ4ParallelModule;
  void Build(core::AuxEngineServices &s, core::StateBuilder &b) override {
    ++builds;
    SecEdgarQ4ParallelModule::Build(s, b);
  }
  int builds = 0;
};
#endif

struct Env {
  Env(duckdb::Connection &connection, bool parallel)
      : con(connection),
        accelerator(parallel ? core::SecEdgarQ4Layout::kParallelAccelerator
                             : core::SecEdgarQ4Layout::kAccelerator),
        store(accelerator), scan(connection),
        backend(coordinator, store, [](const duckdb::TableMutationEvent &) { return core::MutationBatch{}; },
                [] { return core::VisibilityToken{}; },
                [] { return aas::maintenance::CostModelParams{}; }, [] { return 10.0; }),
        entry(&engine::LookupSharedQuery("sec_edgar", 4)) {
    backend.SetExecutionContext(connection.context.get());
    const SecEdgarQ4Config cfg{1, 2, 3, 4};
#ifdef ACCEL_ENABLE_PARALLEL_AUX
    if (parallel) {
      auto m = std::make_shared<CountingParallelModule>(cfg, &scan);
      builds = &m->builds;
      module = m;
    }
#endif
    if (!module) {
      auto m = std::make_shared<CountingModule>(cfg, &scan);
      builds = &m->builds;
      module = m;
    }
    coordinator.Register(module);
  }

  void Build() {
    Builder builder;
    module->Build(backend, builder);
    Require(builder.finished, "build did not finish");
    store.Install(std::move(builder.images), accelerator);
  }

  std::vector<core::AuxRow> Execute() {
    Require(module->CheckValidity(backend, {}), "state must be valid (served incrementally)");
    MaterializeSink sink;
    module->Execute(backend, {}, sink);
    return std::move(sink.rows);
  }

  void Compare(const std::string &what, int step) {
    const auto rows = Execute();
    try {
      harness::CompareAuxRowsToBase(con, *entry, rows);
    } catch (const std::exception &e) {
      std::cerr << "FAIL step " << step << " (" << what << "): " << e.what() << '\n';
      std::abort();
    }
  }

  duckdb::Connection &con;
  std::string accelerator;
  engine::duckdb::DuckDBStateStore store;
  SecEdgarQ4DuckDbScan scan;
  core::AuxTransactionCoordinator coordinator;
  engine::duckdb::DuckDBAuxBackend backend;
  const engine::SharedQueryCatalogEntry *entry;
  std::shared_ptr<core::AuxQueryModule> module;
  int *builds = nullptr;
};

// ------------------------------------------------------------- synthetic
int RunSynthetic(int seeds, int steps, bool parallel) {
  int total_rebuilds = 0;
  for (int seed = 1; seed <= seeds + 1; ++seed) {
    const bool limit_case = seed == seeds + 1;
    auto cfg = q4gen::MakeConfig(seed, limit_case);
    cfg.big_values = false;   // keep cancellation noise below the 1e-6 comparison
    cfg.unique_sums = true;   // no exact ties => ORDER BY is deterministic
    q4gen::Model model(static_cast<uint64_t>(seed) * 104729, cfg);
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    Exec(con, "PRAGMA threads=4");
    for (const auto &sql : model.Init()) Exec(con, sql);

    Env env(con, parallel);
    env.Build();
    env.Compare("initial", 0);
    if (limit_case) {
      Require(env.Execute().size() == 500, "LIMIT 500 must truncate");
    }
    int incremental = 0, rebuilt = 0;
    size_t result_rows = 0;
    for (int step = 1; step <= steps; ++step) {
      const q4gen::Db snapshot = model.db;
      const int64_t rid = model.next_rid;
      const bool rollback = model.Coin(0.08);
      const int builds_before = *env.builds;
      auto txn = model.NextTxn();
      con.BeginTransaction();
      for (const auto &m : txn) {
        for (const auto &sql : m.sql) Exec(con, sql);
        env.backend.CaptureMutation(m.batch);
      }
      env.coordinator.PreCommit(); // stage inside the open SQL transaction
      if (rollback) {
        con.Rollback();
        env.coordinator.Abort();
        model.db = snapshot;
        model.next_rid = rid;
      } else {
        con.Commit();
        env.coordinator.Commit();
      }
      (*env.builds > builds_before ? rebuilt : incremental)++;
      env.Compare(rollback ? "rollback" : model.last_what, step);
      result_rows += env.Execute().size();
    }
    total_rebuilds += rebuilt;
    std::cerr << "synthetic seed " << seed << (parallel ? " (parallel)" : "") << ": " << steps
              << " txns ok (avg_result_rows=" << result_rows / steps << " incremental=" << incremental << " rebuilds=" << rebuilt << ")\n";
    Require(rebuilt <= steps / 20, "too many rebuilds");
  }
  return total_rebuilds;
}

// ------------------------------------------------------------- real data
std::string SqlRow(duckdb::Connection &con, const std::string &select) {
  auto r = con.Query(select);
  Require(r && !r->HasError(), select);
  Require(r->RowCount() == 1, "expected one row: " + select);
  std::string out = "(";
  for (duckdb::idx_t c = 0; c < r->ColumnCount(); ++c) {
    if (c) out += ",";
    out += r->GetValue(c, 0).ToSQLString();
  }
  return out + ")";
}

struct WriteOp {
  std::string name;
  core::TableId table;
  core::MutationKind kind;
  std::vector<core::ColumnId> updated;
  std::string old_select; // "SELECT * ..." for the pre-image (may be empty)
  std::vector<std::string> sql;
  std::string new_select; // post-image (may be empty)
};

void ApplyOp(Env &env, const WriteOp &op, int step) {
  auto &con = env.con;
  core::MutationBatch batch;
  batch.table = op.table;
  batch.kind = op.kind;
  batch.updated_columns = op.updated;
  if (!op.old_select.empty()) batch.old_rows = Fetch(con, op.old_select);
  con.BeginTransaction();
  for (const auto &sql : op.sql) Exec(con, sql);
  if (!op.new_select.empty()) batch.new_rows = Fetch(con, op.new_select);
  env.backend.CaptureMutation(batch);
  env.coordinator.PreCommit();
  con.Commit();
  env.coordinator.Commit();
  const auto t0 = Clock::now();
  env.Compare(op.name, step);
  std::cerr << "  " << op.name << ": ok (verify " << Ms(t0) << " ms)\n";
}

int RunFile(const std::string &path, int ops, bool parallel) {
  duckdb::DuckDB db(path.c_str());
  duckdb::Connection con(db);
  Exec(con, "PRAGMA threads=8");
  Env env(con, parallel);
  auto t0 = Clock::now();
  env.Build();
  std::cerr << "build: " << Ms(t0) << " ms\n";
  env.Compare("initial", 0);
  std::cerr << "initial result matches DuckDB\n";

  std::mt19937_64 rng(42);
  auto pick = [&](const std::string &sql) {
    auto rows = Fetch(con, sql);
    Require(!rows.empty(), "no candidate: " + sql);
    return rows[rng() % rows.size()];
  };
  auto s = [](const core::AuxRow &r, size_t i) { return std::get<std::string>(r.values[i].payload); };
  auto q = [](const std::string &v) { return q4gen::Q(v); };

  int step = 0;
  for (int round = 0; round < ops; ++round) {
    // A qualifying num row (cohort sic, USD, bridge key present).
    const auto num = pick(
        "SELECT n.rowid, n.adsh, n.tag, n.version FROM num n JOIN pre p ON n.adsh=p.adsh AND "
        "n.tag=p.tag AND n.version=p.version AND p.stmt='EQ' JOIN sub s ON s.adsh=n.adsh AND "
        "s.sic BETWEEN 4000 AND 4999 WHERE n.uom='USD' AND n.value IS NOT NULL USING SAMPLE reservoir(200 ROWS) REPEATABLE (" + std::to_string(round * 4 + 1) + ")");
    const std::string rid = std::to_string(std::get<int64_t>(num.values[0].payload));
    const auto where = " FROM num WHERE rowid = " + rid;
    // value update
    ApplyOp(env, {"num value update", 1, core::MutationKind::Update, {7}, "SELECT *" + where,
                  {"UPDATE num SET value = value * 1.5 + 1000" + std::string(" WHERE rowid = ") + rid},
                  "SELECT *" + where}, ++step);
    // delete + reinsert of the same row
    const auto row_sql = SqlRow(con, "SELECT *" + where);
    ApplyOp(env, {"num delete", 1, core::MutationKind::Delete, {}, "SELECT *" + where,
                  {"DELETE FROM num WHERE rowid = " + rid}, ""}, ++step);
    ApplyOp(env, {"num insert", 1, core::MutationKind::Insert, {},
                  "", {"INSERT INTO num VALUES " + row_sql},
                  "SELECT * FROM num WHERE rowid = (SELECT max(rowid) FROM num)"}, ++step);
    ApplyOp(env, {"num uom -> EUR (leaves result)", 1, core::MutationKind::Update, {5},
                  "SELECT * FROM num WHERE rowid = (SELECT max(rowid) FROM num)",
                  {"UPDATE num SET uom = 'EUR' WHERE rowid = (SELECT max(rowid) FROM num)"},
                  "SELECT * FROM num WHERE rowid = (SELECT max(rowid) FROM num)"}, ++step);

    // sub moves into / out of the sic range
    const auto sub = pick("SELECT adsh FROM sub WHERE sic BETWEEN 4000 AND 4999 USING SAMPLE reservoir(50 ROWS) REPEATABLE (" + std::to_string(round * 4 + 2) + ")");
    const std::string sw = " FROM sub WHERE adsh = " + q(s(sub, 0));
    ApplyOp(env, {"sub sic out of range", 2, core::MutationKind::Update, {3}, "SELECT *" + sw,
                  {"UPDATE sub SET sic = 5000 WHERE adsh = " + q(s(sub, 0))}, "SELECT *" + sw}, ++step);
    ApplyOp(env, {"sub sic back in range", 2, core::MutationKind::Update, {3}, "SELECT *" + sw,
                  {"UPDATE sub SET sic = 4999 WHERE adsh = " + q(s(sub, 0))}, "SELECT *" + sw}, ++step);
    ApplyOp(env, {"sub cik change", 2, core::MutationKind::Update, {1}, "SELECT *" + sw,
                  {"UPDATE sub SET cik = cik + 1000000 WHERE adsh = " + q(s(sub, 0))}, "SELECT *" + sw}, ++step);
    const auto sub_row = SqlRow(con, "SELECT *" + sw);
    ApplyOp(env, {"sub delete", 2, core::MutationKind::Delete, {}, "SELECT *" + sw,
                  {"DELETE FROM sub WHERE adsh = " + q(s(sub, 0))}, ""}, ++step);
    ApplyOp(env, {"sub insert", 2, core::MutationKind::Insert, {}, "",
                  {"INSERT INTO sub VALUES " + sub_row}, "SELECT *" + sw}, ++step);

    // tag abstract flip / tlabel change on a tag used by the cohort
    const auto tag = pick(
        "SELECT p.tag, p.version FROM pre p JOIN sub s ON s.adsh=p.adsh AND s.sic BETWEEN 4000 AND 4999 "
        "JOIN tag t ON t.tag=p.tag AND t.version=p.version AND t.abstract=0 WHERE p.stmt='EQ' USING SAMPLE reservoir(100 ROWS) REPEATABLE (" + std::to_string(round * 4 + 3) + ")");
    const std::string tw = " FROM tag WHERE tag = " + q(s(tag, 0)) + " AND version = " + q(s(tag, 1));
    ApplyOp(env, {"tag abstract flip", 3, core::MutationKind::Update, {3}, "SELECT *" + tw,
                  {"UPDATE tag SET abstract = 1" + std::string(" WHERE tag = ") + q(s(tag, 0)) +
                   " AND version = " + q(s(tag, 1))}, "SELECT *" + tw}, ++step);
    ApplyOp(env, {"tag abstract restore", 3, core::MutationKind::Update, {3}, "SELECT *" + tw,
                  {"UPDATE tag SET abstract = 0 WHERE tag = " + q(s(tag, 0)) + " AND version = " + q(s(tag, 1))},
                  "SELECT *" + tw}, ++step);
    ApplyOp(env, {"tag tlabel change", 3, core::MutationKind::Update, {7}, "SELECT *" + tw,
                  {"UPDATE tag SET tlabel = 'relabelled' WHERE tag = " + q(s(tag, 0)) +
                   " AND version = " + q(s(tag, 1))}, "SELECT *" + tw}, ++step);

    // pre: duplicate an EQ row (multiplicity 2), then drop one copy
    const auto pre = pick("SELECT p.rowid FROM pre p JOIN sub s ON s.adsh=p.adsh AND s.sic BETWEEN 4000 AND 4999 "
                          "WHERE p.stmt='EQ' USING SAMPLE reservoir(100 ROWS) REPEATABLE (" + std::to_string(round * 4 + 3) + ")");
    const std::string prid = std::to_string(std::get<int64_t>(pre.values[0].payload));
    const auto pre_row = SqlRow(con, "SELECT * FROM pre WHERE rowid = " + prid);
    const auto pre_key = Fetch(con, "SELECT adsh, tag, version FROM pre WHERE rowid = " + prid)[0];
    (void)pre_key;
    const std::string pkw = " FROM pre WHERE rowid = (SELECT max(rowid) FROM pre)";
    ApplyOp(env, {"pre insert (dup key)", 4, core::MutationKind::Insert, {}, "",
                  {"INSERT INTO pre VALUES " + pre_row}, "SELECT *" + pkw}, ++step);
    ApplyOp(env, {"pre stmt change", 4, core::MutationKind::Update, {3}, "SELECT * FROM pre WHERE rowid = " + prid,
                  {"UPDATE pre SET stmt = 'IS' WHERE rowid = " + prid}, "SELECT * FROM pre WHERE rowid = " + prid},
            ++step);
    ApplyOp(env, {"pre delete", 4, core::MutationKind::Delete, {}, "SELECT * FROM pre WHERE rowid = " + prid,
                  {"DELETE FROM pre WHERE rowid = " + prid}, ""}, ++step);
  }
  std::cerr << "file differential ok, rebuilds=" << (*env.builds - 1) << '\n';
  return *env.builds - 1;
}

} // namespace

int main(int argc, char **argv) {
  const std::string mode = argc > 1 ? argv[1] : "synthetic";
  bool parallel = false;
  for (int i = 2; i < argc; ++i) parallel |= std::string(argv[i]) == "parallel";
  try {
    if (mode == "synthetic") {
      const int seeds = argc > 2 && std::isdigit(argv[2][0]) ? std::atoi(argv[2]) : 4;
      const int steps = argc > 3 && std::isdigit(argv[3][0]) ? std::atoi(argv[3]) : 250;
      RunSynthetic(seeds, steps, parallel);
    } else if (mode == "file") {
      Require(argc > 2, "usage: file <db_copy> [ops] [parallel]");
      const int ops = argc > 3 && std::isdigit(argv[3][0]) ? std::atoi(argv[3]) : 3;
      RunFile(argv[2], ops, parallel);
    } else if (mode == "harness") {
      // The generic shared harness (probe-based incremental + adversarial writes).
      const auto &entry = engine::LookupSharedQuery("sec_edgar", 4);
      harness::RunDifferentialCheck(argv[2], entry);
      harness::RunIncrementalDifferentialCheck(argv[2], entry);
      harness::RunAdversarialWriteDifferentialCheck(argv[2], entry, 5, 1, false);
    } else {
      Require(false, "unknown mode " + mode);
    }
  } catch (const std::exception &e) {
    std::cerr << "test_sec_edgar_q4_duckdb_differential failed: " << e.what() << '\n';
    return 1;
  }
  std::cout << "test_sec_edgar_q4_duckdb_differential " << mode << " passed\n";
  return 0;
}
