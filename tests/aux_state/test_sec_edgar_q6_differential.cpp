// Randomized differential test for the SEC EDGAR Q6 aux module.
//
// Drives the real module through the real coordinator / backend / state store
// against a DuckDB database, applying INSERT / DELETE / UPDATE on every base
// table the query reads (num, sub, pre), including updates of join keys and
// filter columns, multi-table transactions, aborted transactions and bulk
// batches, and compares the aux answer with plain DuckDB SQL after each step.
//
// Q6 is parameterized by (fy, stmt, uom) (defaults 2023, 'IS', 'USD'). After
// every step the default answer AND several random parameter triples (the
// benchmark domain plus edges: NULL, unknown, look-alike values) are compared
// with plain DuckDB SQL and with a state built from scratch. The DATA is
// randomized around the parameters too: neighbouring fiscal years, NULL fy,
// look-alike units ('usd', 'USD ', 'EUR'), look-alike statements ('is', 'BS'),
// NULL/empty labels and names, exact ties, duplicates and orphan keys.
//
// usage: test_sec_edgar_q6_differential [synthetic|synthetic-large|<db_path>]
//        [seed] [ops] [parallel]
// A db_path must be a writable scratch copy (rows are modified and the file is
// not restored). Run from the repository root (cost model path is relative).

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "accelerator_codegen/harness/cost_calibration.hpp"
#include "accelerator_codegen/harness/differential.hpp"
#include "accelerator_codegen/stateful/core/aux_calibration.hpp"
#include "accelerator_codegen/stateful/duckdb/duckdb_aux_backend.hpp"
#include "accelerator_codegen/stateful/duckdb/duckdb_state_store.hpp"
#include "catalog/query_catalog.hpp"
#include "sec_edgar/q6/duckdb/aux/duckdb_scan.hpp"
#include "sec_edgar/q6/duckdb/aux/module.hpp"
#include "sec_edgar/q6/duckdb/aux/parallel/module.hpp"

namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace engine = accelerator_codegen::aux_state::engine;
namespace q = accelerator_codegen::aux_state::query;
namespace harness = accelerator_codegen::aux_state::harness;

using core::AuxRow;
using core::AuxValue;
using core::MaintenanceAction;
using core::MutationBatch;
using core::MutationKind;
using Rng = std::mt19937_64;

void Require(bool ok, const std::string &message) {
  if (!ok) {
    throw std::runtime_error(message);
  }
}

struct ActionLog {
  MaintenanceAction last = MaintenanceAction::NoOp;
  bool staged = false;
};

template <typename Base>
class RecordingModule final : public Base {
public:
  RecordingModule(q::SecEdgarQ6Config config, q::SecEdgarQ6InitialScan *scan,
                  std::shared_ptr<ActionLog> log)
      : Base(config, scan), log_(std::move(log)) {}
  core::StagedMaintenanceOutcome
  StageMaintenance(core::AuxEngineServices &services,
                   const core::InvalidationPlan &plan,
                   core::AuxTxnBuffer &buffer) override {
    auto outcome = Base::StageMaintenance(services, plan, buffer);
    log_->last = outcome.action;
    log_->staged = true;
    return outcome;
  }

private:
  std::shared_ptr<ActionLog> log_;
};

struct CollectSink final : core::AuxRowSink {
  void Begin(const core::OutputSchema &schema) override {
    Require(schema.columns.size() == 6, "schema width");
  }
  void WriteRow(const AuxRow &row) override { rows.push_back(row); }
  void End() override {}
  std::vector<AuxRow> rows;
};

struct CollectBuilder final : core::StateBuilder {
  void Write(core::StateImage image) override { images.push_back(std::move(image)); }
  void Finish() override { finished = true; }
  std::vector<core::StateImage> images;
  bool finished = false;
};

// A fresh build per step is only affordable on small data (override with
// ACCEL_Q6_FRESH_CHECK=0/1).
bool g_fresh_check = true;
// Random parameter triples compared per step (ACCEL_Q6_PARAM_CHECKS).
int g_param_checks = 3;

// ---------------------------------------------------------------------------
// Harness: DuckDB + coordinator + backend + state store + module.
// ---------------------------------------------------------------------------
class Harness {
public:
  Harness(duckdb::Connection &connection, bool parallel_variant)
      : connection_(connection),
        entry_(engine::LookupSharedQuery("sec_edgar", 6)),
        store_(parallel_variant ? q::SecEdgarQ6ParallelModule::kId
                                : q::SecEdgarQ6Module::kId),
        backend_(
            coordinator_, store_,
            [](const ::duckdb::TableMutationEvent &) { return MutationBatch{}; },
            [] { return core::VisibilityToken{}; },
            [] {
              return core::LoadAuxCostModelJson(harness::SharedCostModelConfigPath(
                  engine::LookupSharedQuery(engine::SharedWorkloadKind::SecEdgar, 6)));
            },
            [] { return 10.0; }),
        scan_(connection), log_(std::make_shared<ActionLog>()) {
    if (parallel_variant) {
      module_ = std::make_shared<RecordingModule<q::SecEdgarQ6ParallelModule>>(
          q::SecEdgarQ6Config{1, 2, 3}, &scan_, log_);
    } else {
      module_ = std::make_shared<RecordingModule<q::SecEdgarQ6Module>>(
          q::SecEdgarQ6Config{1, 2, 3}, &scan_, log_);
    }
    coordinator_.Register(module_);
    backend_.SetExecutionContext(connection.context.get());
  }

  void Build() {
    CollectBuilder builder;
    module_->Build(backend_, builder);
    Require(builder.finished, "build did not finish");
    store_.Install(std::move(builder.images), module_->Id());
  }

  std::vector<AuxRow> Execute(const core::QueryArguments &args = {}) {
    CollectSink sink;
    module_->Execute(backend_, args, sink);
    return std::move(sink.rows);
  }

  struct Params {
    bool fy_null = false;
    int64_t fy = 2023;
    bool stmt_null = false;
    std::string stmt = "IS";
    bool uom_null = false;
    std::string uom = "USD";
    core::QueryArguments Args() const {
      core::QueryArguments a;
      a.values.push_back(fy_null ? AuxValue::Null(core::AuxType::Int64)
                                 : AuxValue::Of(core::AuxType::Int64, fy));
      a.values.push_back(stmt_null ? AuxValue::Null(core::AuxType::String)
                                   : AuxValue::Of(core::AuxType::String, stmt));
      a.values.push_back(uom_null ? AuxValue::Null(core::AuxType::String)
                                  : AuxValue::Of(core::AuxType::String, uom));
      return a;
    }
    static std::string Quote(const std::string &v) {
      std::string out = "'";
      for (char c : v) out += c == '\'' ? std::string("''") : std::string(1, c);
      return out + "'";
    }
    std::string Sql() const {
      return "SELECT s.name, p.stmt, n.tag, p.plabel, SUM(n.value) AS total_value, COUNT(*) AS cnt "
             "FROM num n JOIN sub s ON n.adsh = s.adsh "
             "JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version "
             "WHERE n.uom = " + (uom_null ? std::string("NULL") : Quote(uom)) +
             " AND p.stmt = " + (stmt_null ? std::string("NULL") : Quote(stmt)) +
             " AND s.fy = " + (fy_null ? std::string("NULL") : std::to_string(fy)) +
             " AND n.value IS NOT NULL GROUP BY s.name, p.stmt, n.tag, p.plabel "
             "ORDER BY total_value DESC, s.name, n.tag, p.plabel LIMIT 200";
    }
    std::string Text() const {
      return "(" + (fy_null ? std::string("NULL") : std::to_string(fy)) + "," +
             (stmt_null ? std::string("NULL") : stmt) + "," + (uom_null ? std::string("NULL") : uom) + ")";
    }
  };
  std::mt19937_64 param_rng_{0x5EED};
  Params RandomParams() {
    static const int64_t fys[] = {2021, 2022, 2023, 2023, 2024, 2019, 2025, 0};
    static const char *stmts[] = {"BS", "IS", "IS", "CF", "EQ", "CI", "ZZ", "is"};
    static const char *uoms[] = {"USD", "USD", "shares", "pure", "CAD", "CNY", "EUR", "zz", "usd"};
    Params p;
    auto pick = [&](size_t n) { return static_cast<size_t>(param_rng_() % n); };
    if (pick(14) == 0) p.fy_null = true; else p.fy = fys[pick(sizeof fys / sizeof *fys)];
    if (pick(14) == 0) p.stmt_null = true; else p.stmt = stmts[pick(sizeof stmts / sizeof *stmts)];
    if (pick(14) == 0) p.uom_null = true; else p.uom = uoms[pick(sizeof uoms / sizeof *uoms)];
    return p;
  }

  void CheckAgainstBase(const std::string &context) {
    Require(module_->CheckValidity(backend_, {}),
            "aux state not valid after " + context);
    std::string at = "default";
    try {
      std::vector<Params> params{Params{}};
      for (int i = 0; i < g_param_checks; ++i) params.push_back(RandomParams());
      std::shared_ptr<const q::SecEdgarQ6State> fresh;
      if (g_fresh_check) {
        fresh = std::static_pointer_cast<q::SecEdgarQ6Module>(module_)->BuildState();
      }
      for (const auto &p : params) {
        at = p.Text();
        const auto got = Execute(p.Args());
        if (fresh) CheckAgainstFresh(*fresh, p, got);
        harness::CompareAuxRowsToSql(connection_, entry_, p.Sql(), got);
      }
    } catch (const std::exception &e) {
      throw std::runtime_error(std::string(e.what()) + " [params " + at + " after " + context + "]");
    }
  }

  // The incrementally maintained state must answer exactly like a state built
  // from scratch over the same tables (same groups, same bits).
  void CheckAgainstFresh(const q::SecEdgarQ6State &state, const Params &p,
                         const std::vector<AuxRow> &got) {
    if (p.fy_null || p.stmt_null || p.uom_null) {
      Require(got.empty(), "a NULL parameter must give an empty result");
      return;
    }
    const auto fresh = q::SecEdgarQ6TopRows(state, p.fy, p.stmt, p.uom);
    Require(got.size() == fresh.size(), "fresh build row count " + std::to_string(fresh.size()) +
                                            " vs maintained " + std::to_string(got.size()));
    for (std::size_t i = 0; i < fresh.size(); ++i) {
      const auto &r = got[i].values;
      const bool name_ok = fresh[i].name_is_null ? r[0].is_null
                                                 : (!r[0].is_null && std::get<std::string>(r[0].payload) == fresh[i].name);
      const bool plabel_ok = fresh[i].plabel_is_null ? r[3].is_null
                                                     : (!r[3].is_null && std::get<std::string>(r[3].payload) == fresh[i].plabel);
      Require(name_ok && plabel_ok && std::get<std::string>(r[2].payload) == fresh[i].tag &&
                  std::get<std::string>(r[1].payload) == p.stmt &&
                  std::get<int64_t>(r[5].payload) == fresh[i].cnt,
              "maintained state differs from a fresh build at row " + std::to_string(i) + " fresh=(" +
                  fresh[i].name + "," + fresh[i].tag + "," + fresh[i].plabel + "," + std::to_string(fresh[i].cnt) +
                  "," + std::to_string(fresh[i].total_value) + ") got=(" +
                  (r[0].is_null ? "NULL" : std::get<std::string>(r[0].payload)) + "," +
                  std::get<std::string>(r[2].payload) + "," +
                  (r[3].is_null ? "NULL" : std::get<std::string>(r[3].payload)) + "," +
                  std::to_string(std::get<int64_t>(r[5].payload)) + "," +
                  std::to_string(std::get<double>(r[4].payload)) + ")");
    }
  }

  void Begin() {
    log_->staged = false;
    connection_.BeginTransaction();
  }
  // Delivers a mutation as the engine's capture does: the rows carry only
  // the columns the module declares (mutation_attnums), in that order, and
  // updated_columns index the projected row.
  void Capture(const std::vector<MutationBatch> &mutations) {
    const auto deps = module_->Dependencies();
    for (const auto &m : mutations) {
      const core::TableDependency *dep = nullptr;
      for (const auto &d : deps) {
        if (d.table == m.table) dep = &d;
      }
      Require(dep != nullptr, "mutation for an unwatched table");
      MutationBatch projected;
      projected.table = m.table;
      projected.kind = m.kind;
      for (const auto physical : m.updated_columns) {
        for (std::size_t j = 0; j < dep->mutation_attnums.size(); ++j) {
          if (dep->mutation_attnums[j] == static_cast<int32_t>(physical)) {
            projected.updated_columns.push_back(static_cast<core::ColumnId>(j));
          }
        }
      }
      const auto project = [&](const AuxRow &full) {
        AuxRow out;
        for (const auto attnum : dep->mutation_attnums) {
          out.values.push_back(full.values.at(static_cast<std::size_t>(attnum)));
        }
        return out;
      };
      for (const auto &r : m.old_rows) projected.old_rows.push_back(project(r));
      for (const auto &r : m.new_rows) projected.new_rows.push_back(project(r));
      backend_.CaptureMutation(projected);
    }
  }
  void Finish(bool commit) {
    if (commit) {
      coordinator_.PreCommit();
      connection_.Commit();
      coordinator_.Commit();
    } else {
      coordinator_.Abort();
      connection_.Rollback();
    }
  }
  MaintenanceAction LastAction() const {
    return log_->staged ? log_->last : MaintenanceAction::NoOp;
  }
  uint64_t FootprintBytes() const {
    uint64_t total = 0;
    const auto snapshot = store_.Read({});
    for (const auto &image : snapshot.partitions) {
      if (const auto *typed = image.As<q::SecEdgarQ6StateImage>()) {
        if (typed->state) total += typed->state->Bytes();
      }
    }
    return total;
  }

private:
  duckdb::Connection &connection_;
  const engine::SharedQueryCatalogEntry &entry_;
  engine::duckdb::DuckDBStateStore store_;
  core::AuxTransactionCoordinator coordinator_;
  engine::duckdb::DuckDBAuxBackend backend_;
  q::SecEdgarQ6DuckdbScan scan_;
  std::shared_ptr<ActionLog> log_;
  std::shared_ptr<core::AuxQueryModule> module_;
};

// ---------------------------------------------------------------------------
// SQL helpers
// ---------------------------------------------------------------------------
using Value = duckdb::Value;
using Row = std::vector<Value>;

std::string Literal(const Value &v) {
  if (v.IsNull()) {
    return "NULL";
  }
  switch (v.type().id()) {
  case duckdb::LogicalTypeId::VARCHAR: {
    std::string s = v.ToString();
    std::string out = "'";
    for (char c : s) {
      if (c == '\'') out += "''";
      else out += c;
    }
    return out + "'";
  }
  case duckdb::LogicalTypeId::DOUBLE: {
    char buf[64];
    const double d = v.GetValue<double>();
    std::snprintf(buf, sizeof buf, "%.17g", d);
    return std::string(buf) + "::DOUBLE";
  }
  default:
    return v.ToString();
  }
}

AuxValue ToAux(const Value &v) {
  switch (v.type().id()) {
  case duckdb::LogicalTypeId::VARCHAR:
    return v.IsNull() ? AuxValue::Null(core::AuxType::String)
                      : AuxValue::Of(core::AuxType::String, v.ToString());
  case duckdb::LogicalTypeId::DOUBLE:
    return v.IsNull() ? AuxValue::Null(core::AuxType::Double)
                      : AuxValue::Of(core::AuxType::Double, v.GetValue<double>());
  default:
    return v.IsNull() ? AuxValue::Null(core::AuxType::Int64)
                      : AuxValue::Of(core::AuxType::Int64, v.GetValue<int64_t>());
  }
}

AuxRow ToAuxRow(const Row &row) {
  AuxRow out;
  for (const auto &v : row) {
    out.values.push_back(ToAux(v));
  }
  return out;
}

struct Table {
  const char *name;
  core::TableId id;
  std::vector<std::string> columns;
  std::vector<duckdb::LogicalType> types;
};

// ---------------------------------------------------------------------------
// Data model / generators
// ---------------------------------------------------------------------------
struct Scale {
  int names, adsh, tags, versions, plabels, pre_per_adsh, num_rows;
  int bulk;
};

class Db {
public:
  Db(duckdb::Connection &con, uint64_t seed, bool real, Scale scale)
      : con(con), rng(seed), real(real), scale(scale) {}

  duckdb::Connection &con;
  Rng rng;
  bool real;
  Scale scale;
  Table num{"num", 1, {}, {}};
  Table sub{"sub", 2, {}, {}};
  Table pre{"pre", 3, {}, {}};
  std::vector<std::string> names, tags, versions, plabels;
  int fresh_counter = 0;

  uint64_t Rand(uint64_t n) { return n == 0 ? 0 : rng() % n; }
  bool Chance(double p) { return std::uniform_real_distribution<>(0, 1)(rng) < p; }
  template <typename T> const T &Pick(const std::vector<T> &v) { return v[Rand(v.size())]; }

  std::unique_ptr<duckdb::MaterializedQueryResult> Query(const std::string &sql) {
    auto r = con.Query(sql);
    Require(r && !r->HasError(), "SQL failed: " + sql + " : " + (r ? r->GetError() : ""));
    return r;
  }
  std::vector<Row> Rows(const std::string &sql) {
    auto r = Query(sql);
    std::vector<Row> rows;
    for (duckdb::idx_t i = 0; i < r->RowCount(); ++i) {
      Row row;
      for (duckdb::idx_t c = 0; c < r->ColumnCount(); ++c) {
        row.push_back(r->GetValue(c, i));
      }
      rows.push_back(std::move(row));
    }
    return rows;
  }
  void LoadSchema(Table &t) {
    auto rows = Rows(std::string("SELECT name, type FROM pragma_table_info('") + t.name + "')");
    t.columns.clear();
    t.types.clear();
    for (auto &r : rows) {
      t.columns.push_back(r[0].ToString());
      const auto type = r[1].ToString();
      t.types.push_back(type == "VARCHAR" ? duckdb::LogicalType::VARCHAR
                        : type == "DOUBLE" ? duckdb::LogicalType::DOUBLE
                                           : duckdb::LogicalType::INTEGER);
    }
  }
  int ColumnIndex(const Table &t, const std::string &c) {
    for (size_t i = 0; i < t.columns.size(); ++i)
      if (t.columns[i] == c) return static_cast<int>(i);
    throw std::runtime_error("no column " + c);
  }

  // Random value generators ------------------------------------------------
  Value NumValue() {
    if (Chance(0.07)) return Value(duckdb::LogicalType::DOUBLE);
    if (Chance(0.12)) {
      // dyadic rationals: every sum stays exact, so twin groups (same rows,
      // different plabel) tie exactly in both engines.
      return Value::DOUBLE(static_cast<double>(static_cast<int64_t>(Rand(6400000)) - 64000) / 64.0);
    }
    static const double kExact[] = {0, 1, 2, 3, 5, 10, 20, 100, 1000, 5000, 1e6,
                                    -1, -5, -100, 0.5, 0.25, 2.5, 1e9, 1e12, -1e9};
    return Value::DOUBLE(kExact[Rand(sizeof kExact / sizeof *kExact)]);
  }
  Value NumUom() {
    static const char *k[] = {"USD", "USD", "USD", "USD", "USD", "EUR", "usd", "USD ", "", "CAD",
                              "shares", "shares", "pure", "CNY"};
    if (Chance(0.04)) return Value(duckdb::LogicalType::VARCHAR);
    return Value(k[Rand(sizeof k / sizeof *k)]);
  }
  Value NullableStr(const std::vector<std::string> &pool, double null_p) {
    if (Chance(null_p)) return Value(duckdb::LogicalType::VARCHAR);
    return Value(Pick(pool));
  }
  Value Fy() {
    static const int k[] = {2023, 2023, 2023, 2023, 2022, 2024, 2023, 2021, 2025, 2022, 2024, 2019, 0};
    if (Chance(0.04)) return Value(duckdb::LogicalType::INTEGER);
    return Value::INTEGER(k[Rand(sizeof k / sizeof *k)]);
  }
  Value Stmt() {
    static const char *k[] = {"IS", "IS", "IS", "IS", "BS", "CF", "is", "IS ", "EQ", "CI"};
    if (Chance(0.04)) return Value(duckdb::LogicalType::VARCHAR);
    return Value(k[Rand(sizeof k / sizeof *k)]);
  }
  std::string AdshName(int i) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "0001193125-23-%06d", i);
    return buf;
  }
  // existing sub adsh (any fy) / orphan
  Value SomeAdsh(const std::vector<std::string> &adshs) {
    if (Chance(0.04)) return Value("ORPHAN-" + std::to_string(Rand(5)));
    return Value(Pick(adshs));
  }

  void MakeData() {
    for (int i = 0; i < scale.names; ++i) {
      static const char *special[] = {"", "acme", "ACME", "Acme", "Ünï Corp", "A B", "a", "A", "zeta \"Q\""};
      if (i < 9) names.push_back(special[i]);
      else names.push_back("Name " + std::to_string(i));
    }
    for (int i = 0; i < scale.tags; ++i) {
      if (i == 0) tags.push_back(std::string(200, 'T'));
      else if (i == 1) tags.push_back("Revenues");
      else if (i == 2) tags.push_back("");
      else tags.push_back("Tag" + std::to_string(i));
    }
    static const char *vs[] = {"us-gaap/2022", "us-gaap/2023", "dei/2023", "", "ifrs/2021"};
    for (int i = 0; i < scale.versions; ++i) versions.push_back(vs[i % 5]);
    for (int i = 0; i < scale.plabels; ++i) {
      if (i == 0) plabels.push_back("");
      else if (i == 1) plabels.push_back("Total revenues");
      else plabels.push_back("Label " + std::to_string(i) + (i % 3 == 0 ? " é" : ""));
    }

    Query("CREATE TABLE num(adsh VARCHAR, tag VARCHAR, version VARCHAR, ddate INTEGER, qtrs INTEGER, uom VARCHAR, coreg VARCHAR, value DOUBLE, footnote VARCHAR)");
    Query("CREATE TABLE sub(adsh VARCHAR NOT NULL PRIMARY KEY, cik INTEGER, name VARCHAR, sic INTEGER, countryba VARCHAR, stprba VARCHAR, cityba VARCHAR, countryinc VARCHAR, form VARCHAR, period INTEGER, fy INTEGER, fp VARCHAR, filed INTEGER, accepted VARCHAR, prevrpt INTEGER, nciks INTEGER, afs VARCHAR, wksi INTEGER, fye VARCHAR, instance VARCHAR)");
    Query("CREATE TABLE pre(adsh VARCHAR, report INTEGER, line INTEGER, stmt VARCHAR, inpth INTEGER, rfile VARCHAR, tag VARCHAR, version VARCHAR, plabel VARCHAR, negating INTEGER)");
    LoadSchema(num);
    LoadSchema(sub);
    LoadSchema(pre);

    std::vector<std::string> adshs;
    {
      std::ostringstream sql;
      sql << "INSERT INTO sub(adsh, cik, name, sic, fy) VALUES ";
      for (int i = 0; i < scale.adsh; ++i) {
        adshs.push_back(AdshName(i));
        if (i) sql << ",";
        sql << "(" << Literal(Value(adshs.back())) << "," << (1000 + i) << ","
            << Literal(NullableStr(names, 0.03)) << "," << (i % 50) << ","
            << Literal(Fy()) << ")";
      }
      Query(sql.str());
    }
    {
      std::ostringstream sql;
      sql << "INSERT INTO pre(adsh, report, line, stmt, tag, version, plabel) VALUES ";
      bool first = true;
      for (int a = 0; a < scale.adsh; ++a) {
        for (int k = 0; k < scale.pre_per_adsh; ++k) {
          if (!first) sql << ",";
          first = false;
          sql << "(" << Literal(Value(adshs[a])) << "," << k % 3 << "," << k << ","
              << Literal(Stmt()) << "," << Literal(Value(Pick(tags))) << ","
              << Literal(Value(Pick(versions))) << "," << Literal(NullableStr(plabels, 0.06))
              << ")";
        }
      }
      // exact duplicates (multiplicity) and extra labels for the same key
      for (int d = 0; d < scale.adsh / 2 + 1; ++d) {
        sql << ",(" << Literal(Value(Pick(adshs))) << ",1,99,'IS'," << Literal(Value(tags[1 % tags.size()]))
            << "," << Literal(Value(versions[0])) << "," << Literal(Value(plabels[1 % plabels.size()])) << ")";
      }
      Query(sql.str());
    }
    {
      // num rows mostly hit existing pre keys.
      auto pre_rows = Rows("SELECT adsh, tag, version FROM pre");
      for (int base = 0; base < scale.num_rows; base += 2000) {
        std::ostringstream sql;
        sql << "INSERT INTO num VALUES ";
        const int end = std::min(scale.num_rows, base + 2000);
        for (int i = base; i < end; ++i) {
          Row key;
          if (Chance(0.8) && !pre_rows.empty()) key = pre_rows[Rand(pre_rows.size())];
          else key = {SomeAdsh(adshs), Value(Pick(tags)), Value(Pick(versions))};
          if (i != base) sql << ",";
          sql << "(" << Literal(key[0]) << "," << Literal(key[1]) << "," << Literal(key[2])
              << "," << (20230101 + static_cast<int>(Rand(300))) << ",4," << Literal(NumUom())
              << ",NULL," << Literal(NumValue()) << ",NULL)";
        }
        Query(sql.str());
      }
    }
  }

  // Existing-row picking ----------------------------------------------------
  std::vector<int64_t> PickRowids(const Table &t, const std::string &where, size_t k) {
    const int64_t window = real ? 20000 : 1000000000;
    for (int attempt = 0; attempt < 12; ++attempt) {
      int64_t lo = 0;
      if (real) {
        const int64_t hi = max_rowid_cache[t.name];
        lo = static_cast<int64_t>(Rand(static_cast<uint64_t>(std::max<int64_t>(1, hi))));
      }
      auto rows = Rows(std::string("SELECT rowid FROM ") + t.name + " WHERE rowid >= " +
                       std::to_string(lo) + " AND rowid < " + std::to_string(lo + window) +
                       (where.empty() ? "" : " AND " + where) + " LIMIT " +
                       std::to_string(real ? k : 100000));
      if (rows.empty()) continue;
      // random subset for synthetic mode
      std::vector<int64_t> ids;
      for (auto &r : rows) ids.push_back(r[0].GetValue<int64_t>());
      if (!real) {
        std::shuffle(ids.begin(), ids.end(), rng);
      }
      if (ids.size() > k) ids.resize(k);
      return ids;
    }
    return {};
  }
  std::map<std::string, int64_t> max_rowid_cache;
  void RefreshRowidCache() {
    for (auto *t : {&num, &sub, &pre}) {
      auto r = Rows(std::string("SELECT coalesce(max(rowid), 0) FROM ") + t->name);
      max_rowid_cache[t->name] = r[0][0].GetValue<int64_t>() + 1000;
    }
  }

  std::string IdList(const std::vector<int64_t> &ids) {
    std::string s;
    for (size_t i = 0; i < ids.size(); ++i) {
      if (i) s += ",";
      s += std::to_string(ids[i]);
    }
    return s;
  }
  std::vector<Row> RowsByIds(const Table &t, const std::vector<int64_t> &ids) {
    // preserve the order of `ids`
    std::vector<Row> out;
    for (auto id : ids) {
      auto r = Rows(std::string("SELECT * FROM ") + t.name + " WHERE rowid = " + std::to_string(id));
      Require(r.size() == 1, "row vanished");
      out.push_back(std::move(r[0]));
    }
    return out;
  }

  // Generic row-generic update on table by rowid ----------------------------
  MutationBatch UpdateRows(const Table &t, const std::vector<int64_t> &ids,
                           const std::vector<std::pair<std::string, Value>> &sets) {
    MutationBatch m;
    m.table = t.id;
    m.kind = MutationKind::Update;
    auto before = RowsByIds(t, ids);
    std::string assign;
    for (size_t i = 0; i < sets.size(); ++i) {
      if (i) assign += ", ";
      assign += sets[i].first + " = " + Literal(sets[i].second);
      m.updated_columns.push_back(static_cast<core::ColumnId>(ColumnIndex(t, sets[i].first)));
    }
    Query(std::string("UPDATE ") + t.name + " SET " + assign + " WHERE rowid IN (" + IdList(ids) + ")");
    auto after = RowsByIds(t, ids);
    for (auto &r : before) m.old_rows.push_back(ToAuxRow(r));
    for (auto &r : after) m.new_rows.push_back(ToAuxRow(r));
    return m;
  }
  MutationBatch DeleteRows(const Table &t, const std::vector<int64_t> &ids) {
    MutationBatch m;
    m.table = t.id;
    m.kind = MutationKind::Delete;
    auto before = RowsByIds(t, ids);
    Query(std::string("DELETE FROM ") + t.name + " WHERE rowid IN (" + IdList(ids) + ")");
    for (auto &r : before) m.old_rows.push_back(ToAuxRow(r));
    return m;
  }
  MutationBatch InsertRows(const Table &t, const std::vector<Row> &rows) {
    MutationBatch m;
    m.table = t.id;
    m.kind = MutationKind::Insert;
    std::ostringstream sql;
    sql << "INSERT INTO " << t.name << " VALUES ";
    for (size_t i = 0; i < rows.size(); ++i) {
      if (i) sql << ",";
      sql << "(";
      for (size_t c = 0; c < rows[i].size(); ++c) {
        if (c) sql << ",";
        sql << Literal(rows[i][c]);
      }
      sql << ")";
      m.new_rows.push_back(ToAuxRow(rows[i]));
    }
    Query(sql.str());
    return m;
  }

  // Predicates that find rows that matter / do not matter -------------------
  std::string CohortAdshPredicate() { return "adsh IN (SELECT adsh FROM sub WHERE fy = 2023)"; }

  std::vector<std::string> SubAdshes() {
    std::vector<std::string> out;
    for (auto &r : Rows("SELECT adsh FROM sub ORDER BY adsh LIMIT 100000")) out.push_back(r[0].ToString());
    return out;
  }
  std::vector<std::string> CohortSubAdshes() {
    std::vector<std::string> out;
    for (auto &r : Rows("SELECT adsh FROM sub WHERE fy = 2023 ORDER BY adsh LIMIT 100000")) out.push_back(r[0].ToString());
    return out;
  }

  Row NewNumRow() {
    // mostly keys of existing IS pre rows in the cohort
    Row key;
    auto hits = Rows("SELECT adsh, tag, version FROM pre WHERE stmt = 'IS' AND " + CohortAdshPredicate() +
                     " AND rowid >= " + std::to_string(real ? Rand(static_cast<uint64_t>(max_rowid_cache["pre"])) : 0) +
                     " LIMIT 50");
    if (Chance(0.75) && !hits.empty()) {
      key = hits[Rand(hits.size())];
    } else {
      auto adshs = SubAdshes();
      key = {adshs.empty() ? Value("X") : SomeAdsh(adshs), Value(Pick(tags)), Value(Pick(versions))};
    }
    return {key[0], key[1], key[2], Value::INTEGER(20230101 + static_cast<int>(Rand(300))),
            Value::INTEGER(4), NumUom(), Value(duckdb::LogicalType::VARCHAR), NumValue(),
            Value(duckdb::LogicalType::VARCHAR)};
  }
};

enum class Kind { Num, Sub, Pre, Mixed, Abort };

struct Stats {
  std::map<std::string, std::map<int, int>> actions;
  int ops = 0;
};

const char *ActionName(MaintenanceAction a) {
  switch (a) {
  case MaintenanceAction::NoOp: return "NoOp";
  case MaintenanceAction::ApplyDelta: return "ApplyDelta";
  case MaintenanceAction::RebuildAll: return "RebuildAll";
  case MaintenanceAction::RebuildPartition: return "RebuildPartition";
  case MaintenanceAction::InvalidateAndRebuildOnRead: return "InvalidateAndRebuildOnRead";
  }
  return "?";
}

using OpFn = std::function<std::vector<MutationBatch>(Db &)>;

// ----- num ops ------------------------------------------------------------
std::vector<MutationBatch> OpNumInsert(Db &d) {
  const size_t n = d.Chance(0.15) ? d.scale.bulk : 1 + d.Rand(4);
  std::vector<Row> rows;
  for (size_t i = 0; i < n; ++i) rows.push_back(d.NewNumRow());
  return {d.InsertRows(d.num, rows)};
}
std::vector<MutationBatch> OpNumDelete(Db &d) {
  const bool join_hits = d.Chance(0.6);
  const size_t n = d.Chance(0.1) ? d.scale.bulk : 1 + d.Rand(4);
  auto ids = d.PickRowids(d.num, join_hits ? "uom = 'USD' AND " + d.CohortAdshPredicate() : "", n);
  if (ids.empty()) return {};
  return {d.DeleteRows(d.num, ids)};
}
std::vector<MutationBatch> OpNumUpdateValue(Db &d) {
  const size_t n = d.Chance(0.1) ? d.scale.bulk : 1 + d.Rand(3);
  auto ids = d.PickRowids(d.num, d.Chance(0.7) ? "uom = 'USD' AND " + d.CohortAdshPredicate() : "", n);
  if (ids.empty()) return {};
  return {d.UpdateRows(d.num, ids, {{"value", d.NumValue()}})};
}
std::vector<MutationBatch> OpNumUpdateUom(Db &d) {
  auto ids = d.PickRowids(d.num, "", 1 + d.Rand(3));
  if (ids.empty()) return {};
  return {d.UpdateRows(d.num, ids, {{"uom", d.NumUom()}})};
}
std::vector<MutationBatch> OpNumUpdateKey(Db &d) {
  auto ids = d.PickRowids(d.num, d.Chance(0.5) ? d.CohortAdshPredicate() : "", 1 + d.Rand(3));
  if (ids.empty()) return {};
  const int which = static_cast<int>(d.Rand(4));
  auto newrow = d.NewNumRow();
  std::vector<std::pair<std::string, Value>> sets;
  if (which == 0) sets = {{"adsh", newrow[0]}};
  else if (which == 1) sets = {{"tag", newrow[1]}};
  else if (which == 2) sets = {{"version", newrow[2]}};
  else sets = {{"adsh", newrow[0]}, {"tag", newrow[1]}, {"version", newrow[2]}, {"value", d.NumValue()}};
  return {d.UpdateRows(d.num, ids, sets)};
}
std::vector<MutationBatch> OpNumUpdateIrrelevant(Db &d) {
  auto ids = d.PickRowids(d.num, "", 1 + d.Rand(5));
  if (ids.empty()) return {};
  const int which = static_cast<int>(d.Rand(3));
  std::vector<std::pair<std::string, Value>> sets;
  if (which == 0) sets = {{"ddate", Value::INTEGER(static_cast<int>(d.Rand(99999)))}};
  else if (which == 1) sets = {{"coreg", Value("CO" + std::to_string(d.Rand(9)))}};
  else sets = {{"footnote", Value("fn")}, {"qtrs", Value::INTEGER(1)}};
  return {d.UpdateRows(d.num, ids, sets)};
}

// ----- sub ops ------------------------------------------------------------
std::vector<MutationBatch> SubUpdateByAdsh(Db &d, const std::vector<std::string> &adshs,
                                           const std::vector<std::pair<std::string, Value>> &sets,
                                           const std::function<std::string(const std::string &)> &rename = {}) {
  MutationBatch m;
  m.table = d.sub.id;
  m.kind = MutationKind::Update;
  std::string assign;
  for (size_t i = 0; i < sets.size(); ++i) {
    if (i) assign += ", ";
    assign += sets[i].first + " = " + Literal(sets[i].second);
    m.updated_columns.push_back(static_cast<core::ColumnId>(d.ColumnIndex(d.sub, sets[i].first)));
  }
  for (const auto &adsh : adshs) {
    auto before = d.Rows("SELECT * FROM sub WHERE adsh = " + Literal(Value(adsh)));
    Require(before.size() == 1, "sub row vanished");
    d.Query("UPDATE sub SET " + assign + " WHERE adsh = " + Literal(Value(adsh)));
    const std::string after_key = rename ? rename(adsh) : adsh;
    auto after = d.Rows("SELECT * FROM sub WHERE adsh = " + Literal(Value(after_key)));
    Require(after.size() == 1, "sub row not found after update");
    m.old_rows.push_back(ToAuxRow(before[0]));
    m.new_rows.push_back(ToAuxRow(after[0]));
  }
  return {m};
}
std::vector<std::string> PickSubs(Db &d, bool cohort, size_t k) {
  auto all = cohort ? d.CohortSubAdshes() : d.SubAdshes();
  std::shuffle(all.begin(), all.end(), d.rng);
  if (all.size() > k) all.resize(k);
  return all;
}
std::vector<MutationBatch> OpSubInsert(Db &d) {
  std::vector<Row> rows;
  const size_t n = 1 + d.Rand(3);
  for (size_t i = 0; i < n; ++i) {
    Row r(d.sub.columns.size(), Value(duckdb::LogicalType::VARCHAR));
    for (size_t c = 0; c < r.size(); ++c) {
      if (d.sub.types[c] == duckdb::LogicalType::INTEGER) r[c] = Value(duckdb::LogicalType::INTEGER);
    }
    // sometimes reuse an adsh that already has pre/num rows (orphan adopted)
    std::string adsh;
    auto orphan = d.Rows("SELECT adsh FROM pre WHERE adsh NOT IN (SELECT adsh FROM sub) LIMIT 5");
    if (!orphan.empty() && d.Chance(0.4)) adsh = orphan[d.Rand(orphan.size())][0].ToString();
    else adsh = "NEW-" + std::to_string(d.fresh_counter++);
    bool duplicate = false;
    for (const auto &prev : rows) duplicate |= prev[0].ToString() == adsh;
    if (duplicate) adsh = "NEW-" + std::to_string(d.fresh_counter++);
    r[d.ColumnIndex(d.sub, "adsh")] = Value(adsh);
    r[d.ColumnIndex(d.sub, "name")] = d.NullableStr(d.names, 0.05);
    r[d.ColumnIndex(d.sub, "fy")] = d.Fy();
    r[d.ColumnIndex(d.sub, "sic")] = Value::INTEGER(7);
    rows.push_back(r);
  }
  return {d.InsertRows(d.sub, rows)};
}
std::vector<MutationBatch> OpSubDelete(Db &d) {
  auto adshs = PickSubs(d, d.Chance(0.6), 1);
  if (adshs.empty()) return {};
  MutationBatch m;
  m.table = d.sub.id;
  m.kind = MutationKind::Delete;
  for (auto &a : adshs) {
    auto before = d.Rows("SELECT * FROM sub WHERE adsh = " + Literal(Value(a)));
    m.old_rows.push_back(ToAuxRow(before[0]));
    d.Query("DELETE FROM sub WHERE adsh = " + Literal(Value(a)));
  }
  return {m};
}
std::vector<MutationBatch> OpSubUpdateFy(Db &d) {
  auto adshs = PickSubs(d, d.Chance(0.5), 1 + d.Rand(2));
  if (adshs.empty()) return {};
  return SubUpdateByAdsh(d, adshs, {{"fy", d.Fy()}});
}
std::vector<MutationBatch> OpSubUpdateName(Db &d) {
  auto adshs = PickSubs(d, true, 1 + d.Rand(2));
  if (adshs.empty()) return {};
  return SubUpdateByAdsh(d, adshs, {{"name", d.NullableStr(d.names, 0.15)}});
}
std::vector<MutationBatch> OpSubRename(Db &d) {
  auto adshs = PickSubs(d, d.Chance(0.7), 1);
  if (adshs.empty()) return {};
  const std::string fresh = "REN-" + std::to_string(d.fresh_counter++);
  return SubUpdateByAdsh(d, adshs, {{"adsh", Value(fresh)}}, [&](const std::string &) { return fresh; });
}
std::vector<MutationBatch> OpSubUpdateIrrelevant(Db &d) {
  auto adshs = PickSubs(d, d.Chance(0.5), 1 + d.Rand(3));
  if (adshs.empty()) return {};
  return SubUpdateByAdsh(d, adshs, {{"sic", Value::INTEGER(static_cast<int>(d.Rand(9999)))}});
}

// ----- pre ops ------------------------------------------------------------
std::vector<MutationBatch> OpPreInsert(Db &d) {
  std::vector<Row> rows;
  const size_t n = d.Chance(0.1) ? std::min(40, d.scale.bulk / 4 + 1) : 1 + d.Rand(3);
  auto adshs = d.Chance(0.7) ? d.CohortSubAdshes() : d.SubAdshes();
  if (adshs.empty()) return {};
  for (size_t i = 0; i < n; ++i) {
    std::string tag = d.Pick(d.tags), version = d.Pick(d.versions);
    if (d.Chance(0.6)) {
      // key of an existing num fact so the new bridge row matters
      auto hit = d.Rows("SELECT tag, version FROM num WHERE rowid >= " +
                        std::to_string(d.real ? d.Rand(static_cast<uint64_t>(d.max_rowid_cache["num"])) : 0) + " LIMIT 20");
      if (!hit.empty()) {
        auto &h = hit[d.Rand(hit.size())];
        tag = h[0].ToString();
        version = h[1].ToString();
      }
    }
    Row r(d.pre.columns.size(), Value(duckdb::LogicalType::VARCHAR));
    for (size_t c = 0; c < r.size(); ++c) {
      if (d.pre.types[c] == duckdb::LogicalType::INTEGER) r[c] = Value(duckdb::LogicalType::INTEGER);
    }
    r[d.ColumnIndex(d.pre, "adsh")] = d.SomeAdsh(adshs);
    r[d.ColumnIndex(d.pre, "stmt")] = d.Stmt();
    r[d.ColumnIndex(d.pre, "tag")] = Value(tag);
    r[d.ColumnIndex(d.pre, "version")] = Value(version);
    r[d.ColumnIndex(d.pre, "plabel")] = d.NullableStr(d.plabels, 0.08);
    r[d.ColumnIndex(d.pre, "line")] = Value::INTEGER(static_cast<int>(d.Rand(100)));
    rows.push_back(r);
  }
  return {d.InsertRows(d.pre, rows)};
}
std::vector<MutationBatch> OpPreDelete(Db &d) {
  auto ids = d.PickRowids(d.pre, d.Chance(0.7) ? "stmt = 'IS' AND " + d.CohortAdshPredicate() : "", 1 + d.Rand(3));
  if (ids.empty()) return {};
  return {d.DeleteRows(d.pre, ids)};
}
std::vector<MutationBatch> OpPreUpdateStmt(Db &d) {
  auto ids = d.PickRowids(d.pre, d.Chance(0.6) ? d.CohortAdshPredicate() : "", 1 + d.Rand(3));
  if (ids.empty()) return {};
  return {d.UpdateRows(d.pre, ids, {{"stmt", d.Stmt()}})};
}
std::vector<MutationBatch> OpPreUpdatePlabel(Db &d) {
  auto ids = d.PickRowids(d.pre, "stmt = 'IS' AND " + d.CohortAdshPredicate(), 1 + d.Rand(3));
  if (ids.empty()) return {};
  return {d.UpdateRows(d.pre, ids, {{"plabel", d.NullableStr(d.plabels, 0.15)}})};
}
std::vector<MutationBatch> OpPreUpdateKey(Db &d) {
  auto ids = d.PickRowids(d.pre, d.Chance(0.6) ? "stmt = 'IS'" : "", 1 + d.Rand(2));
  if (ids.empty()) return {};
  const int which = static_cast<int>(d.Rand(3));
  std::vector<std::pair<std::string, Value>> sets;
  if (which == 0) sets = {{"tag", Value(d.Pick(d.tags))}};
  else if (which == 1) sets = {{"version", Value(d.Pick(d.versions))}};
  else sets = {{"adsh", Value(d.Pick(d.SubAdshes()))}};
  return {d.UpdateRows(d.pre, ids, sets)};
}
std::vector<MutationBatch> OpPreUpdateIrrelevant(Db &d) {
  auto ids = d.PickRowids(d.pre, "", 1 + d.Rand(4));
  if (ids.empty()) return {};
  return {d.UpdateRows(d.pre, ids, {{"line", Value::INTEGER(static_cast<int>(d.Rand(9999)))},
                                    {"negating", Value::INTEGER(1)}})};
}

struct OpDef {
  const char *name;
  Kind kind;
  int weight_synth;
  int weight_real;
  OpFn fn;
};

} // namespace

int main(int argc, char **argv) {
  try {
    std::string mode = argc > 1 ? argv[1] : "synthetic";
    const uint64_t seed = argc > 2 ? std::stoull(argv[2]) : 1;
    int ops = argc > 3 ? std::stoi(argv[3]) : 300;
    const bool parallel = argc > 4 && std::string(argv[4]) == "parallel";
    const bool real = mode != "synthetic" && mode != "synthetic-large";
    g_fresh_check = mode == "synthetic";
    if (const char *e = std::getenv("ACCEL_Q6_FRESH_CHECK")) g_fresh_check = e[0] == '1';
    if (const char *e = std::getenv("ACCEL_Q6_PARAM_CHECKS")) g_param_checks = std::atoi(e);

    std::unique_ptr<duckdb::DuckDB> database;
    if (real) {
      database = std::make_unique<duckdb::DuckDB>(mode.c_str());
    } else {
      database = std::make_unique<duckdb::DuckDB>(nullptr);
    }
    duckdb::Connection connection(*database);
    if (const char *t = std::getenv("ACCEL_BENCH_THREADS")) {
      connection.Query(std::string("PRAGMA threads=") + t);
    } else {
      connection.Query("PRAGMA threads=4");
    }

    Scale scale = {9, 40, 6, 3, 5, 8, 600, 200};
    if (mode == "synthetic-large") scale = {400, 3000, 60, 4, 6, 12, 40000, 4000};
    if (real) scale = {0, 0, 0, 0, 0, 0, 0, 3000};
    Db db(connection, seed, real, scale);
    if (!real) {
      db.MakeData();
    } else {
      db.LoadSchema(db.num);
      db.LoadSchema(db.sub);
      db.LoadSchema(db.pre);
      for (auto &r : db.Rows("SELECT DISTINCT tag FROM pre WHERE rowid < 200000")) db.tags.push_back(r[0].ToString());
      for (auto &r : db.Rows("SELECT DISTINCT version FROM pre WHERE rowid < 200000")) db.versions.push_back(r[0].ToString());
      for (auto &r : db.Rows("SELECT DISTINCT plabel FROM pre WHERE rowid < 200000 AND plabel IS NOT NULL LIMIT 2000")) db.plabels.push_back(r[0].ToString());
      for (auto &r : db.Rows("SELECT DISTINCT name FROM sub WHERE name IS NOT NULL LIMIT 2000")) db.names.push_back(r[0].ToString());
    }
    db.RefreshRowidCache();

    Harness harness_(connection, parallel);
    harness_.Build();
    harness_.CheckAgainstBase("initial build");
    std::cout << "mode=" << mode << " seed=" << seed << " ops=" << ops
              << (parallel ? " parallel" : "") << " state_bytes="
              << harness_.FootprintBytes() << "\n";

    const std::vector<OpDef> defs = {
        {"num_insert", Kind::Num, 14, 30, OpNumInsert},
        {"num_delete", Kind::Num, 10, 20, OpNumDelete},
        {"num_update_value", Kind::Num, 14, 30, OpNumUpdateValue},
        {"num_update_uom", Kind::Num, 5, 5, OpNumUpdateUom},
        {"num_update_key", Kind::Num, 7, 8, OpNumUpdateKey},
        {"num_update_irrelevant", Kind::Num, 3, 2, OpNumUpdateIrrelevant},
        {"sub_insert", Kind::Sub, 3, 3, OpSubInsert},
        {"sub_delete", Kind::Sub, 3, 3, OpSubDelete},
        {"sub_update_fy", Kind::Sub, 4, 3, OpSubUpdateFy},
        {"sub_update_name", Kind::Sub, 3, 3, OpSubUpdateName},
        {"sub_rename_adsh", Kind::Sub, 2, 0, OpSubRename},
        {"sub_update_irrelevant", Kind::Sub, 2, 1, OpSubUpdateIrrelevant},
        {"pre_insert", Kind::Pre, 4, 3, OpPreInsert},
        {"pre_delete", Kind::Pre, 3, 3, OpPreDelete},
        {"pre_update_stmt", Kind::Pre, 3, 3, OpPreUpdateStmt},
        {"pre_update_plabel", Kind::Pre, 3, 3, OpPreUpdatePlabel},
        {"pre_update_key", Kind::Pre, 3, 3, OpPreUpdateKey},
        {"pre_update_irrelevant", Kind::Pre, 2, 1, OpPreUpdateIrrelevant},
    };
    int total_weight = 0;
    for (const auto &def : defs) total_weight += real ? def.weight_real : def.weight_synth;
    // mixed / abort pseudo-ops
    const int mixed_weight = real ? 2 : 6;
    const int abort_weight = real ? 2 : 5;
    total_weight += mixed_weight + abort_weight;

    Stats stats;
    std::map<std::string, std::map<std::string, int>> per_op_actions;
    auto pick_def = [&]() -> const OpDef * {
      int r = static_cast<int>(db.Rand(total_weight));
      for (const auto &def : defs) {
        r -= real ? def.weight_real : def.weight_synth;
        if (r < 0) return &def;
      }
      return nullptr; // mixed / abort
    };
    // Every table is maintained incrementally: no transaction may rebuild.
    auto rebuild_allowed = [](Kind) { return false; };

    for (int i = 0; i < ops; ++i) {
      const OpDef *def = pick_def();
      std::string label;
      bool commit = true;
      Kind kind;
      std::vector<const OpDef *> chosen;
      if (def) {
        chosen = {def};
        kind = def->kind;
        label = def->name;
      } else {
        // mixed (commit) or abort (rollback): 1-4 random sub-ops in one txn
        commit = db.Chance(static_cast<double>(mixed_weight) / (mixed_weight + abort_weight));
        kind = commit ? Kind::Mixed : Kind::Abort;
        const int n = commit ? 2 + static_cast<int>(db.Rand(3)) : 1 + static_cast<int>(db.Rand(3));
        label = commit ? "mixed(" : "abort(";
        for (int k = 0; k < n; ++k) {
          const OpDef *c = nullptr;
          while (!c) {
            int r = static_cast<int>(db.Rand(total_weight - mixed_weight - abort_weight));
            for (const auto &dd : defs) {
              r -= real ? dd.weight_real : dd.weight_synth;
              if (r < 0) { c = &dd; break; }
            }
          }
          chosen.push_back(c);
          label += std::string(k ? "+" : "") + c->name;
        }
        label += ")";
      }
      harness_.Begin();
      std::vector<MutationBatch> muts;
      for (const auto *c : chosen) {
        auto m = c->fn(db);
        for (auto &x : m) muts.push_back(std::move(x));
      }
      harness_.Capture(muts);
      harness_.Finish(commit);
      if (commit) db.RefreshRowidCache();
      const auto action = harness_.LastAction();
      const std::string context = "op " + std::to_string(i) + " " + label +
                                  " action=" + ActionName(action);
      if (!rebuild_allowed(kind) && commit) {
        Require(action == MaintenanceAction::NoOp || action == MaintenanceAction::ApplyDelta,
                "transaction must be maintained incrementally (no rebuild): " + context);
      }
      if (kind == Kind::Num || kind == Kind::Sub || kind == Kind::Pre) {
        per_op_actions[label][ActionName(action)]++;
      }
      stats.actions[kind == Kind::Num ? "num" : kind == Kind::Sub ? "sub" : kind == Kind::Pre ? "pre"
                    : kind == Kind::Mixed ? "mixed" : "abort"][static_cast<int>(action)]++;
      ++stats.ops;
      harness_.CheckAgainstBase(context);
      if (i % 25 == 0 && i != 0) {
        std::cout << "  ... " << i << " ops ok\n";
        std::cout.flush();
      }
    }

    std::cout << "ops=" << stats.ops << " state_bytes=" << harness_.FootprintBytes() << "\n";
    for (const auto &[kind, counts] : stats.actions) {
      std::cout << "  " << kind << ":";
      for (const auto &[a, n] : counts) std::cout << " " << ActionName(static_cast<MaintenanceAction>(a)) << "=" << n;
      std::cout << "\n";
    }
    for (const auto &[op, counts] : per_op_actions) {
      std::cout << "    " << op << ":";
      for (const auto &[a, n] : counts) std::cout << " " << a << "=" << n;
      std::cout << "\n";
    }
    // Coverage assertions: the run must have exercised both paths.
    int delta = 0, rebuilds = 0;
    for (const auto &[kind, counts] : stats.actions) {
      for (const auto &[a, n] : counts) {
        delta += static_cast<MaintenanceAction>(a) == MaintenanceAction::ApplyDelta ? n : 0;
        rebuilds += static_cast<MaintenanceAction>(a) == MaintenanceAction::RebuildAll ? n : 0;
      }
    }
    if (ops >= 100) {
      Require(delta > 0, "no ApplyDelta exercised");
      Require(rebuilds == 0, "unexpected rebuilds");
      int sub_pre_delta = 0;
      for (const char *k : {"sub", "pre"}) {
        auto it = stats.actions.find(k);
        if (it != stats.actions.end())
          sub_pre_delta += it->second[static_cast<int>(MaintenanceAction::ApplyDelta)];
      }
      if (!real || ops >= 200) Require(sub_pre_delta > 0, "sub/pre never took the ApplyDelta path");
    }
    std::cout << "PASS test_sec_edgar_q6_differential\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL test_sec_edgar_q6_differential: " << e.what() << "\n";
    return 1;
  }
}
