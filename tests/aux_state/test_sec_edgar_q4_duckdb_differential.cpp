// End-to-end differential test for the SEC-EDGAR Q4 aux module on real DuckDB.
//
//   test_sec_edgar_q4_duckdb_differential synthetic [seeds] [steps] [parallel]
//   test_sec_edgar_q4_duckdb_differential file <db_copy> [ops] [parallel]
//   test_sec_edgar_q4_duckdb_differential sweep <db_copy> [random_ranges] [parallel]
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
// Q4 takes (sic_lo, sic_hi, stmt, uom): the state keeps every (sic, tlabel,
// stmt, uom) group, so after every transaction the default read AND three
// random-parameter reads (SIC divisions, random/inverted/edge ranges, NULLs,
// stmt/uom incl. unknown values) are compared with plain SQL.  The data-domain
// edge values (sic 3999/4000/4999/5000, uom USD/usd/EUR/shares, stmt EQ/NULL,
// abstract 0/1/NULL, tlabel NULL) are covered by the generator.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <map>
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
#include "sec_edgar/q4/common/aux/module.hpp"
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
    state_bytes = 0;
    for (const auto &image : builder.images) {
      if (const auto *typed = image.As<accelerator_codegen::aux_state::query::SecEdgarQ4StateImage>()) {
        state_bytes += accelerator_codegen::aux_state::query::SecEdgarQ4ResidentBytes(*typed->state, typed->kind);
      }
    }
    store.Install(std::move(builder.images), accelerator);
  }
  uint64_t state_bytes = 0;

  std::vector<core::AuxRow> Execute() {
    Require(module->CheckValidity(backend, {}), "state must be valid (served incrementally)");
    MaterializeSink sink;
    module->Execute(backend, {}, sink);
    return std::move(sink.rows);
  }

  // The parameterized read against plain SQL with the same parameters.
  void ComparePerParams(const q4gen::Params &params, const std::string &what, int step) {
    Require(module->CheckValidity(backend, {}), "state must be valid (served incrementally)");
    MaterializeSink sink;
    module->Execute(backend, q4gen::ToQueryArgs(params), sink);
    try {
      harness::CompareAuxRowsToSql(con, *entry, q4gen::BaseSql(params), sink.rows);
    } catch (const std::exception &e) {
      std::cerr << "FAIL step " << step << " (" << what << ") params(" << q4gen::ArgsSql(params)
                << "): " << e.what() << '\n';
      std::abort();
    }
    param_rows += sink.rows.size();
    ++param_checks;
  }
  size_t param_rows = 0;
  int param_checks = 0;

  void Compare(const std::string &what, int step) {
    const auto rows = Execute();
    try {
      harness::CompareAuxRowsToSql(con, *entry, q4gen::BaseSql(q4gen::Params{}), rows);
    } catch (const std::exception &e) {
      std::cerr << "FAIL step " << step << " (" << what << "): " << e.what() << '\n';
      Dump(rows, q4gen::BaseSql(q4gen::Params{}));
      std::abort();
    }
  }

  void Dump(const std::vector<core::AuxRow> &rows, const std::string &sql) {
    auto fmt = [](const core::AuxRow &r) {
      std::string out;
      for (const auto &v : r.values) {
        if (v.is_null) out += "NULL|";
        else if (auto *i = std::get_if<int64_t>(&v.payload)) out += std::to_string(*i) + "|";
        else if (auto *d = std::get_if<double>(&v.payload)) out += std::to_string(*d) + "|";
        else out += std::get<std::string>(v.payload) + "|";
      }
      return out;
    };
    const auto want = Fetch(con, sql);
    for (size_t i = 0; i < 8 && (i < rows.size() || i < want.size()); ++i) {
      std::cerr << "  accel: " << (i < rows.size() ? fmt(rows[i]) : "-") << "\n  base:  "
                << (i < want.size() ? fmt(want[i]) : "-") << '\n';
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
    cfg.wide_keys = seed % 2 == 1;                  // negative / huge cik values
    cfg.nonfinite = !limit_case && seed % 3 != 0;   // inf, NaN, 1e300, subnormals
    q4gen::Model model(static_cast<uint64_t>(seed) * 104729, cfg);
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    Exec(con, "PRAGMA threads=4");
    for (const auto &sql : model.Init()) Exec(con, sql);

    Env env(con, parallel);
    env.Build();
    env.Compare("initial", 0);
    for (int k = 0; k < 12; ++k) env.ComparePerParams(q4gen::RandomParams(model), "initial", 0);
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
      for (int k = 0; k < 3; ++k) {
        env.ComparePerParams(q4gen::RandomParams(model), rollback ? "rollback" : model.last_what, step);
      }
    }
    total_rebuilds += rebuilt;
    std::cerr << "synthetic seed " << seed << (parallel ? " (parallel)" : "") << ": " << steps
              << " txns ok (param_checks=" << env.param_checks << " avg_param_rows="
              << env.param_rows / std::max(1, env.param_checks) << " avg_result_rows=" << result_rows / steps << " incremental=" << incremental << " rebuilds=" << rebuilt << ")\n";
    Require(rebuilt <= (cfg.nonfinite ? steps / 2 : steps / 20), "too many rebuilds");
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

q4gen::Params g_round_params; // parameters of the current file-mode round (non-default)

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
  env.ComparePerParams(g_round_params, op.name, step);
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
  // Round r exercises another (stmt, uom, sic range) than the defaults.
  struct Cell {
    const char *stmt, *uom;
    int lo, hi;
  };
  static const Cell kCells[] = {{"EQ", "USD", 4000, 4999}, {"BS", "USD", 1000, 3999},
                                {"CF", "USD", 5000, 9999}, {"IS", "shares", 100, 9999},
                                {"EQ", "shares", 4000, 4999}, {"BS", "USD", 4000, 4999}};
  for (int round = 0; round < ops; ++round) {
    const Cell &cell = kCells[round % 6];
    const std::string st = std::string("'") + cell.stmt + "'", um = std::string("'") + cell.uom + "'";
    const std::string in_range = "BETWEEN " + std::to_string(cell.lo) + " AND " + std::to_string(cell.hi);
    g_round_params.lo = cell.lo;
    g_round_params.hi = cell.hi;
    g_round_params.stmt = std::string(cell.stmt);
    g_round_params.uom = std::string(cell.uom);
    std::cerr << "round " << round << ": " << cell.stmt << "/" << cell.uom << " sic " << cell.lo << ".."
              << cell.hi << "\n";
    // A qualifying num row (cohort sic, uom, bridge key present).
    const auto num = pick(
        "SELECT n.rowid, n.adsh, n.tag, n.version FROM num n JOIN pre p ON n.adsh=p.adsh AND "
        "n.tag=p.tag AND n.version=p.version AND p.stmt=" + st + " JOIN sub s ON s.adsh=n.adsh AND "
        "s.sic " + in_range + " WHERE n.uom=" + um + " AND n.value IS NOT NULL USING SAMPLE reservoir(200 ROWS) REPEATABLE (" + std::to_string(round * 4 + 1) + ")");
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
    const auto sub = pick("SELECT adsh FROM sub WHERE sic " + in_range + " USING SAMPLE reservoir(50 ROWS) REPEATABLE (" + std::to_string(round * 4 + 2) + ")");
    const std::string sw = " FROM sub WHERE adsh = " + q(s(sub, 0));
    ApplyOp(env, {"sub sic out of range", 2, core::MutationKind::Update, {3}, "SELECT *" + sw,
                  {"UPDATE sub SET sic = " + std::to_string(cell.hi + 1) + " WHERE adsh = " + q(s(sub, 0))}, "SELECT *" + sw}, ++step);
    ApplyOp(env, {"sub sic back in range", 2, core::MutationKind::Update, {3}, "SELECT *" + sw,
                  {"UPDATE sub SET sic = " + std::to_string(cell.hi) + " WHERE adsh = " + q(s(sub, 0))}, "SELECT *" + sw}, ++step);
    ApplyOp(env, {"sub cik change", 2, core::MutationKind::Update, {1}, "SELECT *" + sw,
                  {"UPDATE sub SET cik = cik + 1000000 WHERE adsh = " + q(s(sub, 0))}, "SELECT *" + sw}, ++step);
    const auto sub_row = SqlRow(con, "SELECT *" + sw);
    ApplyOp(env, {"sub delete", 2, core::MutationKind::Delete, {}, "SELECT *" + sw,
                  {"DELETE FROM sub WHERE adsh = " + q(s(sub, 0))}, ""}, ++step);
    ApplyOp(env, {"sub insert", 2, core::MutationKind::Insert, {}, "",
                  {"INSERT INTO sub VALUES " + sub_row}, "SELECT *" + sw}, ++step);

    // tag abstract flip / tlabel change on a tag used by the cohort
    const auto tag = pick(
        "SELECT p.tag, p.version FROM pre p JOIN sub s ON s.adsh=p.adsh AND s.sic " + in_range + " "
        "JOIN tag t ON t.tag=p.tag AND t.version=p.version AND t.abstract=0 WHERE p.stmt=" + st + " USING SAMPLE reservoir(100 ROWS) REPEATABLE (" + std::to_string(round * 4 + 3) + ")");
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
    const auto pre = pick("SELECT p.rowid FROM pre p JOIN sub s ON s.adsh=p.adsh AND s.sic " + in_range + " "
                          "WHERE p.stmt=" + st + " USING SAMPLE reservoir(100 ROWS) REPEATABLE (" + std::to_string(round * 4 + 3) + ")");
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

// Order-insensitive comparison for real data: groups whose totals agree only to
// floating-point rounding (e.g. the same EPS values summed in another order)
// may swap places in plain SQL, so rows are matched by (sic, tlabel).  When
// LIMIT 500 truncates, groups at the cut may differ only if their totals tie
// with the last returned row.
std::string CompareLoose(const std::vector<core::AuxRow> &want, const std::vector<core::AuxRow> &got) {
  using Key = std::pair<int64_t, std::string>;
  const auto key_of = [](const core::AuxRow &r) {
    return Key{std::get<int64_t>(r.values[0].payload),
               r.values[1].is_null ? std::string("\x01NULL") : std::get<std::string>(r.values[1].payload)};
  };
  const auto num = [](const core::AuxValue &v) {
    if (const auto *i = std::get_if<int64_t>(&v.payload)) return static_cast<double>(*i);
    return std::get<double>(v.payload);
  };
  const auto close = [](double a, double b) {
    if (std::isnan(a) || std::isnan(b)) return std::isnan(a) && std::isnan(b);
    if (std::isinf(a) || std::isinf(b)) return a == b;
    return std::fabs(a - b) <= 1e-9 * std::max({1.0, std::fabs(a), std::fabs(b)});
  };
  std::map<Key, const core::AuxRow *> w, g;
  for (const auto &r : want) w[key_of(r)] = &r;
  for (const auto &r : got) g[key_of(r)] = &r;
  if (want.size() != got.size()) {
    return "row count want=" + std::to_string(want.size()) + " got=" + std::to_string(got.size());
  }
  const bool truncated = want.size() == 500;
  const double cut = want.empty() ? 0.0 : num(want.back().values[4]);
  for (const auto &[k, row] : w) {
    const auto it = g.find(k);
    if (it == g.end()) {
      if (truncated && close(num(row->values[4]), cut)) continue;
      return "missing group sic=" + std::to_string(k.first) + " tlabel=" + k.second;
    }
    const auto &a = *row, &b = *it->second;
    if (std::get<std::string>(a.values[2].payload) != std::get<std::string>(b.values[2].payload) ||
        num(a.values[3]) != num(b.values[3]) || !close(num(a.values[4]), num(b.values[4])) ||
        !close(num(a.values[5]), num(b.values[5]))) {
      return "value mismatch sic=" + std::to_string(k.first) + " tlabel=" + k.second;
    }
  }
  return "";
}

// ------------------------------------------------------------ SF3 sweep
// Parameter sweep on a (writable copy of a) real database.  The oracle is the
// 4-way join of q4.sql materialized once without the three parameter
// predicates (`j`); every combination is then plain SQL over `j`, which is the
// same query semantics at a fraction of the cost.  A sample of combinations is
// also compared with the unmaterialized q4.sql.
int RunSweep(const std::string &path, int random_ranges, bool parallel) {
  duckdb::DuckDB db(path.c_str());
  duckdb::Connection con(db);
  Exec(con, "PRAGMA threads=12");
  Env env(con, parallel);
  auto t0 = Clock::now();
  env.Build();
  std::cerr << "build: " << Ms(t0) << " ms, resident state " << env.state_bytes / 1048576.0 << " MiB\n";
  t0 = Clock::now();
  Exec(con, "CREATE TEMP TABLE j AS SELECT s.sic AS sic, t.tlabel AS tlabel, p.stmt AS stmt, "
            "n.uom AS uom, s.cik AS cik, n.value AS value FROM num n JOIN sub s ON n.adsh = s.adsh "
            "JOIN tag t ON n.tag = t.tag AND n.version = t.version "
            "JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version "
            "WHERE n.value IS NOT NULL AND t.abstract = 0");
  std::cerr << "materialized join: " << Ms(t0) << " ms\n";
  const auto oracle_sql = [](const q4gen::Params &p) {
    if (!p.lo || !p.hi || !p.stmt || !p.uom) return q4gen::BaseSql(p); // NULL -> trivially empty
    return std::string("SELECT sic, tlabel, stmt, COUNT(DISTINCT cik) AS num_companies, "
                       "SUM(value) AS total_value, AVG(value) AS avg_value FROM j WHERE uom = ") +
           q4gen::Q(*p.uom) + " AND stmt = " + q4gen::Q(*p.stmt) + " AND sic BETWEEN " +
           std::to_string(*p.lo) + " AND " + std::to_string(*p.hi) +
           " GROUP BY sic, tlabel, stmt HAVING COUNT(DISTINCT cik) >= 2 "
           "ORDER BY total_value DESC, sic, tlabel NULLS FIRST LIMIT 500";
  };
  env.Compare("default vs q4.sql", 0);

  static const int kDiv[10][2] = {{100, 999},   {1000, 1499}, {1500, 1799}, {2000, 3999},
                                  {4000, 4999}, {5000, 5199}, {5200, 5999}, {6000, 6799},
                                  {7000, 8999}, {9000, 9999}};
  static const char *kStmts[] = {"BS", "IS", "CF", "EQ", "CI", "ZZ"};
  static const char *kUoms[] = {"USD", "shares", "pure", "CAD", "CNY", "EUR"};
  int checked = 0, nonempty = 0;
  double aux_ms = 0, sql_ms = 0, max_aux_ms = 0;
  const auto check = [&](const q4gen::Params &p, bool vs_real) {
    MaterializeSink sink;
    auto a0 = Clock::now();
    env.module->Execute(env.backend, q4gen::ToQueryArgs(p), sink);
    const double am = Ms(a0);
    aux_ms += am;
    max_aux_ms = std::max(max_aux_ms, am);
    auto s0 = Clock::now();
    try {
      const auto want = Fetch(con, oracle_sql(p));
      sql_ms += Ms(s0);
      std::string diff = CompareLoose(want, sink.rows);
      if (diff.empty() && vs_real) diff = CompareLoose(Fetch(con, q4gen::BaseSql(p)), sink.rows);
      if (!diff.empty()) throw std::runtime_error(diff);
    } catch (const std::exception &e) {
      std::cerr << "MISMATCH params(" << q4gen::ArgsSql(p) << "): " << e.what() << '\n';
      std::exit(1);
    }
    ++checked;
    nonempty += !sink.rows.empty();
  };
  int idx = 0;
  for (const auto &d : kDiv) {
    for (const char *st : kStmts) {
      for (const char *um : kUoms) {
        q4gen::Params p;
        p.lo = d[0];
        p.hi = d[1];
        p.stmt = std::string(st);
        p.uom = std::string(um);
        check(p, idx++ % 40 == 0);
      }
    }
  }
  std::mt19937_64 rng(7);
  for (int k = 0; k < random_ranges; ++k) {
    q4gen::Params p;
    int64_t a = static_cast<int64_t>(rng() % 10000), b = static_cast<int64_t>(rng() % 10000);
    if (k % 7 != 0 && a > b) std::swap(a, b); // some inverted
    p.lo = a;
    p.hi = b;
    p.stmt = std::string(kStmts[rng() % 5]);
    p.uom = std::string(kUoms[rng() % 6]);
    check(p, k % 25 == 0);
  }
  // Edge values / NULLs.
  const int64_t edges[] = {INT32_MIN, -1, 0, 3999, 4000, 4999, 5000, INT32_MAX};
  for (const int64_t lo : edges) {
    for (const int64_t hi : edges) {
      q4gen::Params p;
      p.lo = lo;
      p.hi = hi;
      check(p, false);
    }
  }
  {
    q4gen::Params p;
    p.lo.reset();
    check(p, false);
    p = q4gen::Params{};
    p.stmt.reset();
    check(p, false);
    p = q4gen::Params{};
    p.uom.reset();
    check(p, false);
  }
  // Wide ranges on the biggest groups, also against the real q4.sql.
  for (const char *st : {"BS", "CF", "IS"}) {
    q4gen::Params p;
    p.lo = INT32_MIN;
    p.hi = INT32_MAX;
    p.stmt = std::string(st);
    check(p, true);
  }
  std::cerr << "sweep ok: " << checked << " combinations (" << nonempty << " non-empty), aux avg "
            << aux_ms / checked << " ms (max " << max_aux_ms << " ms), join-table SQL avg "
            << sql_ms / checked << " ms\n";
  return 0;
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
    } else if (mode == "sweep") {
      Require(argc > 2, "usage: sweep <db_copy> [random_ranges] [parallel]");
      RunSweep(argv[2], argc > 3 && std::isdigit(argv[3][0]) ? std::atoi(argv[3]) : 120, parallel);
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
