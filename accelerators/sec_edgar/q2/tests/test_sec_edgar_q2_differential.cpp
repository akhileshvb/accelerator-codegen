// Differential tests for the SEC-EDGAR Q2 aux module against plain DuckDB SQL.
//
//  * group top-2 algebra vs a brute-force multiset model (property test)
//  * state codec round trip
//  * randomized writes on every column the query reads (num: INSERT / DELETE /
//    UPDATE of adsh, tag, uom, value and an irrelevant column; sub: INSERT /
//    DELETE / UPDATE of fy, name), single- and multi-statement transactions,
//    rollbacks, interleaved with reads for randomly drawn (fy, uom)
//    parameters including edge values (NULL, unknown uom, quote characters,
//    int32 extremes, NaN / +-inf / -0.0 / duplicate values, uncovered uoms)
//  * the DuckDB table-function entry points accel_sec_edgar_q2_aux[_parallel]
//
// Usage: test_sec_edgar_q2_differential [seed] [steps]   (seed also from
//        $ACCEL_TEST_SEED; every failure prints the replay command line)
// Env:   ACCEL_FORCE_MAINTENANCE_ACTION=ApplyDelta|RebuildAll|InvalidateAndFallback|NoOp
//        forces the maintenance decision so every fallback path is exercised.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <tuple>

#include <unistd.h>

#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "backends/duckdb/api/register.hpp"
#include "substrate/aux/core/aux_calibration.hpp"
#include "substrate/aux/core/aux_registry.hpp"
#include "backends/duckdb/engine/duckdb_aux_backend.hpp"
#include "backends/duckdb/state/duckdb_state_store.hpp"
#include "substrate/aux/maintenance/cost_model.hpp"
#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"
#include "sec_edgar/q2/cost_model.hpp"
#include "sec_edgar/q2/duckdb/aux/duckdb_scan.hpp"
#include "sec_edgar/q2/duckdb/aux/module.hpp"
#include "sec_edgar/q2/duckdb/aux/parallel/module.hpp"
#include "sec_edgar/q2/duckdb/base/dictionary_slot_cache.hpp"

namespace core = accelerator_codegen::aux_state::core;
namespace engine = accelerator_codegen::aux_state::engine;
using namespace accelerator_codegen::aux_state::query;

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

// Replay context printed with every failure ("seed=N steps=M").
std::string g_replay;

[[noreturn]] void Fail(const std::string &message) {
  std::cerr << "FAIL: " << message << std::endl;
  std::cerr << "replay: test_sec_edgar_q2_differential " << g_replay << std::endl;
  std::abort();
}

void Require(bool ok, const std::string &message) {
  if (!ok) {
    Fail(message);
  }
}

// ------------------------------------------------------------------ model

struct Multiset {
  std::map<int64_t, uint32_t> counts; // key -> rows
};

void CheckGroupAgainstModel(const Q2Group &g, const Multiset &m) {
  if (m.counts.empty()) {
    Require(g.c0 == 0, "empty model but group has rows");
    return;
  }
  auto it = m.counts.rbegin();
  Require(g.c0 > 0 && g.k0 == it->first && g.c0 == it->second,
          "group max / count mismatch");
  ++it;
  if (g.c1 > 0) {
    Require(it != m.counts.rend() && g.k1 == it->first && g.c1 == it->second,
            "group second value mismatch");
    ++it;
  }
  // trunc may stay set after unlisted rows are deleted (conservative), but
  // must be set whenever unlisted rows exist.
  Require(!(it != m.counts.rend() && !g.trunc), "group trunc flag missing");
}

void TestGroupAlgebra(std::mt19937_64 &rng) {
  for (int iter = 0; iter < 4000; ++iter) {
    Q2Group g;
    Multiset m;
    for (int step = 0; step < 60; ++step) {
      const int64_t key = static_cast<int64_t>(rng() % 6) - 2; // heavy ties
      const bool insert = m.counts.empty() || rng() % 100 < 55 ||
                          m.counts.find(key) == m.counts.end();
      if (insert) {
        Q2GroupInsert(g, key);
        ++m.counts[key];
      } else {
        const Q2Removal r = Q2GroupRemove(g, key);
        Require(r != Q2Removal::Inconsistent, "valid removal flagged inconsistent");
        if (--m.counts[key] == 0) {
          m.counts.erase(key);
        }
        if (r == Q2Removal::Lost) {
          // The module re-reads the group: emulate by rebuilding from the model.
          Q2Group fresh;
          fresh.adsh = g.adsh;
          fresh.tag = g.tag;
          for (const auto &[k, c] : m.counts) {
            Q2GroupInsert(fresh, k, c);
          }
          g = fresh;
        }
      }
      CheckGroupAgainstModel(g, m);
    }
  }
  // removing a value that is not in the group must be flagged
  Q2Group g;
  Q2GroupInsert(g, 5);
  Require(Q2GroupRemove(g, 7) == Q2Removal::Inconsistent, "remove above max");
  Require(Q2GroupRemove(g, 3) == Q2Removal::Inconsistent, "remove absent value");
  Require(Q2ValueKey(-0.0) == Q2ValueKey(0.0), "-0 == 0");
  Require(Q2ValueKey(std::nan("")) > Q2ValueKey(kInf), "NaN > inf");
  Require(Q2ValueKey(-1.0) < Q2ValueKey(-0.5) && Q2ValueKey(-0.5) < Q2ValueKey(0.0) &&
              Q2ValueKey(0.0) < Q2ValueKey(0.5) && Q2ValueKey(-kInf) < Q2ValueKey(-1e300),
          "key order");
  for (const double v : {0.0, 1.5, -2.25, 1e300, -1e-300, kInf, -kInf}) {
    Require(Q2KeyValue(Q2ValueKey(v)) == v, "key round trip");
  }
}

// -------------------------------------------------------------- DB helpers

std::string Quote(const std::string &s) {
  std::string out = "'";
  for (const char c : s) {
    out += c == '\'' ? std::string("''") : std::string(1, c);
  }
  return out + "'";
}

std::string DoubleSql(double v) {
  if (std::isnan(v)) {
    return "'nan'::DOUBLE";
  }
  if (std::isinf(v)) {
    return v > 0 ? "'inf'::DOUBLE" : "'-inf'::DOUBLE";
  }
  if (v == 0.0 && std::signbit(v)) {
    return "'-0'::DOUBLE";
  }
  std::ostringstream os;
  os.precision(17);
  os << v;
  return os.str() + "::DOUBLE";
}

void Exec(duckdb::Connection &con, const std::string &sql) {
  auto r = con.Query(sql);
  if (!r || r->HasError()) {
    Fail("SQL failed: " + sql + " -> " + (r ? r->GetError() : "null"));
  }
}

bool TryExec(duckdb::Connection &con, const std::string &sql) {
  auto r = con.Query(sql);
  return r && !r->HasError();
}

core::AuxValue ToAux(const duckdb::Value &v) {
  using core::AuxType;
  using core::AuxValue;
  switch (v.type().id()) {
  case duckdb::LogicalTypeId::VARCHAR:
    return v.IsNull() ? AuxValue::Null(AuxType::String)
                      : AuxValue::Of(AuxType::String, v.ToString());
  case duckdb::LogicalTypeId::DOUBLE:
    return v.IsNull() ? AuxValue::Null(AuxType::Double)
                      : AuxValue::Of(AuxType::Double, v.GetValue<double>());
  default:
    return v.IsNull() ? AuxValue::Null(AuxType::Int64)
                      : AuxValue::Of(AuxType::Int64, v.GetValue<int64_t>());
  }
}

std::vector<core::AuxRow> FetchRows(duckdb::Connection &con, const std::string &sql) {
  auto r = con.Query(sql);
  if (!r || r->HasError()) {
    Fail("SQL failed: " + sql + " -> " + (r ? r->GetError() : "null"));
  }
  std::vector<core::AuxRow> rows;
  for (duckdb::idx_t i = 0; i < r->RowCount(); ++i) {
    core::AuxRow row;
    for (duckdb::idx_t c = 0; c < r->ColumnCount(); ++c) {
      row.values.push_back(ToAux(r->GetValue(c, i)));
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

constexpr const char *kSchema =
    "CREATE TABLE sub (adsh VARCHAR(20) PRIMARY KEY, cik INTEGER, name VARCHAR(150), "
    "sic INTEGER, countryba VARCHAR(2), stprba VARCHAR(2), cityba VARCHAR(30), "
    "countryinc VARCHAR(3), form VARCHAR(10), period INTEGER, fy INTEGER, fp VARCHAR(2), "
    "filed INTEGER, accepted VARCHAR(24), prevrpt INTEGER, nciks INTEGER, afs VARCHAR(5), "
    "wksi INTEGER, fye VARCHAR(4), instance VARCHAR(64));"
    "CREATE TABLE num (adsh VARCHAR(20), tag VARCHAR(256), version VARCHAR(20), "
    "ddate INTEGER, qtrs INTEGER, uom VARCHAR(20), coreg VARCHAR(256), value DOUBLE, "
    "footnote VARCHAR(1024), FOREIGN KEY (adsh) REFERENCES sub(adsh));";

const std::vector<double> kValues = {0.0,  0.0,  -0.0, 1.0, 1.0,  1.0,   2.5,  2.5,
                                     -3.0, 7.0,  7.0,  100.0, 1e18, 1e18,
                                     std::nan(""), kInf, -kInf, 0.25, 3.0};

struct Gen {
  std::mt19937_64 rng;
  std::vector<std::string> adshs;     // all sub.adsh ever created
  std::vector<std::string> tags;
  std::vector<std::string> uoms;
  int next_adsh = 0;

  explicit Gen(uint64_t seed) : rng(seed) {
    tags = {"Assets", "Liabilities", "Revenue", "tag'quote", "EPS", "Shares",
            "A", "B", "ProfitLoss", std::string(200, 'x')};
    uoms = {"pure", "pure", "pure", "pure", "USD", "USD", "shares", "EUR", "x'y",
            "Pure", "pure "};
  }
  size_t Pick(size_t n) { return static_cast<size_t>(rng() % n); }
  // ORDER BY expression drawing a pseudo-random, seed-determined sample (USING
  // SAMPLE is not reproducible, so a failing seed could not be replayed).
  std::string SampleOrder(const std::string &columns) {
    return "hash(" + columns + ", " + std::to_string(rng() % 1000000007) + "::BIGINT)";
  }
  bool Chance(int percent) { return static_cast<int>(rng() % 100) < percent; }
  double Value() { return kValues[Pick(kValues.size())]; }
  std::string Uom() { return uoms[Pick(uoms.size())]; }
  std::string Tag() { return tags[Pick(tags.size())]; }
  std::string NewAdsh() {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%010d-22-%06d", 1000000 + static_cast<int>(rng() % 50),
                  next_adsh++);
    return buf;
  }
  std::string FySql() {
    static const char *fys[] = {"2022", "2022", "2022", "2021", "2023", "2012", "NULL"};
    return fys[Pick(7)];
  }
  std::string NameSql() {
    static const char *names[] = {"'Acme'", "'Beta Corp'", "'Acme'", "'zeta'",
                                  "'a\"b''c'", "NULL", "'Delta'", "''"};
    return names[Pick(8)];
  }
};

void InsertSub(duckdb::Connection &con, Gen &g, const std::string &adsh) {
  Exec(con, "INSERT INTO sub (adsh, cik, name, fy) VALUES (" + Quote(adsh) + ", 1, " +
                g.NameSql() + ", " + g.FySql() + ")");
}

// Row layout of the mutation batch = physical column order.
std::vector<core::AuxRow> NumRowsById(duckdb::Connection &con, const std::string &where) {
  return FetchRows(con, "SELECT adsh, tag, version, ddate, qtrs, uom, coreg, value, "
                        "footnote FROM num WHERE " + where + " ORDER BY rowid");
}

std::string QuerySql(const std::string &fy_sql, const std::string &uom_sql) {
  return "SELECT s.name, n.tag, n.value FROM num n JOIN sub s ON n.adsh = s.adsh "
         "JOIN (SELECT adsh, tag, MAX(value) AS max_value FROM num WHERE uom = " +
         uom_sql +
         " AND value IS NOT NULL GROUP BY adsh, tag) m ON n.adsh = m.adsh AND "
         "n.tag = m.tag AND n.value = m.max_value WHERE n.uom = " +
         uom_sql + " AND s.fy = " + fy_sql +
         " AND n.value IS NOT NULL ORDER BY n.value DESC, s.name, n.tag LIMIT 100";
}

bool SameDouble(double a, double b) {
  if (std::isnan(a) || std::isnan(b)) {
    return std::isnan(a) && std::isnan(b);
  }
  return a == b;
}

// ----------------------------------------------------------------- harness

class Sink final : public core::AuxRowSink {
public:
  void Begin(const core::OutputSchema &schema) override {
    Require(schema.columns.size() == 3, "output schema width");
  }
  void WriteRow(const core::AuxRow &row) override { rows.push_back(row); }
  void End() override { ended = true; }
  std::vector<core::AuxRow> rows;
  bool ended = false;
};

class Builder final : public core::StateBuilder {
public:
  void Write(core::StateImage image) override { images.push_back(std::move(image)); }
  void Finish() override { finished = true; }
  std::vector<core::StateImage> images;
  bool finished = false;
};

struct Harness {
  duckdb::DuckDB db{nullptr};
  duckdb::Connection con{db};
  std::unique_ptr<engine::duckdb::DuckDBStateStore> store;
  core::AuxTransactionCoordinator coordinator;
  std::unique_ptr<SecEdgarQ2DuckdbScan> scan;
  std::shared_ptr<SecEdgarQ2Module> module;
  std::unique_ptr<engine::duckdb::DuckDBAuxBackend> backend;
  bool parallel = false;
  uint64_t reads = 0;

  explicit Harness(bool parallel_p) : parallel(parallel_p) {
    Exec(con, kSchema);
    // The repository cost model (run from the repository root).
    const char *path = std::getenv("ACCEL_Q2_COST_MODEL");
    const auto cost = core::LoadAuxCostModelJson(
        path ? path : core::AuxCostModelDefaultPath(core::kSecEdgarQ2CostModel));
    store = std::make_unique<engine::duckdb::DuckDBStateStore>(
        parallel ? SecEdgarQ2ParallelModule::kId : SecEdgarQ2Module::kId);
    scan = std::make_unique<SecEdgarQ2DuckdbScan>(con);
    // Test knobs: Q2_TEST_OVERLAY_LIMIT forces compactions on small data,
    // Q2_TEST_MAX_UOM_ROWS=N is the opt-out limit (default: every unit of
    // measure is materialised; N=10000 sends the large unit "USD" to SQL).
    SecEdgarQ2Config cfg{1, 2};
    if (const char *v = std::getenv("Q2_TEST_OVERLAY_LIMIT")) {
      cfg.overlay_limit = std::strtoull(v, nullptr, 10);
    }
    if (const char *v = std::getenv("Q2_TEST_MAX_UOM_ROWS")) {
      cfg.max_uom_rows = std::strtoull(v, nullptr, 10);
    }
    if (const char *v = std::getenv("Q2_TEST_BUILD_THREADS")) {
      cfg.build_threads = static_cast<unsigned>(std::strtoul(v, nullptr, 10));
    } else if (parallel) {
      cfg.build_threads = 4; // exercise the parallel build
    }
    if (parallel) {
      module = std::make_shared<SecEdgarQ2ParallelModule>(cfg, scan.get());
    } else {
      module = std::make_shared<SecEdgarQ2Module>(cfg, scan.get());
    }
    backend = std::make_unique<engine::duckdb::DuckDBAuxBackend>(
        coordinator, *store,
        [](const duckdb::TableMutationEvent &) { return core::MutationBatch{}; },
        [] { return core::VisibilityToken{}; }, [cost] { return cost; },
        [] { return 10.0; });
    coordinator.Register(module);
  }

  void Build() {
    Builder builder;
    module->Build(*backend, builder);
    Require(builder.finished, "build finished");
    store->Install(std::move(builder.images),
                   parallel ? SecEdgarQ2ParallelModule::kId : SecEdgarQ2Module::kId);
  }

  void Capture(const core::MutationBatch &batch) { backend->CaptureMutation(batch); }

  void Commit() {
    coordinator.PreCommit();
    con.Commit();
    coordinator.Commit();
  }

  std::vector<core::AuxRow> Run(const core::QueryArguments &args) {
    Sink sink;
    module->Execute(*backend, args, sink);
    Require(sink.ended, "sink ended");
    ++reads;
    return sink.rows;
  }

  // Compares the aux answer for (fy, uom) with plain SQL.
  void Verify(const std::string &label, bool fy_null, int64_t fy, bool uom_null,
              const std::string &uom, bool use_defaults = false) {
    core::QueryArguments args;
    std::string fy_sql = std::to_string(fy), uom_sql = Quote(uom);
    if (use_defaults) {
      fy_sql = "2022";
      uom_sql = "'pure'";
    } else {
      args.values.push_back(fy_null ? core::AuxValue::Null(core::AuxType::Int64)
                                    : core::AuxValue::Of(core::AuxType::Int64, fy));
      args.values.push_back(uom_null ? core::AuxValue::Null(core::AuxType::String)
                                     : core::AuxValue::Of(core::AuxType::String, uom));
      if (fy_null) {
        fy_sql = "NULL";
      }
      if (uom_null) {
        uom_sql = "NULL";
      }
    }
    const auto got = Run(args);
    auto expect = con.Query(QuerySql(fy_sql, uom_sql));
    Require(expect && !expect->HasError(), "reference query failed");
    const std::string ctx = label + " fy=" + fy_sql + " uom=" + uom_sql;
    const auto dump = [&](const std::string &what) {
      std::ostringstream os;
      os << what << "\n  aux rows / sql rows:";
      for (size_t i = 0; i < std::max<size_t>(got.size(), expect->RowCount()); ++i) {
        os << "\n  " << i << ": ";
        if (i < got.size()) {
          const auto &r = got[i].values;
          os << (r[0].is_null ? std::string("NULL") : std::get<std::string>(r[0].payload))
             << " | " << std::get<std::string>(r[1].payload).substr(0, 24) << " | "
             << std::get<double>(r[2].payload);
        } else {
          os << "-";
        }
        os << "   ||   ";
        if (i < expect->RowCount()) {
          os << expect->GetValue(0, i).ToString() << " | "
             << expect->GetValue(1, i).ToString().substr(0, 24) << " | "
             << expect->GetValue(2, i).ToString();
        } else {
          os << "-";
        }
      }
      return os.str();
    };
    if (got.size() != expect->RowCount()) {
      Fail(dump(ctx + ": row count aux=" + std::to_string(got.size()) +
                " sql=" + std::to_string(expect->RowCount())));
    }
    for (duckdb::idx_t i = 0; i < expect->RowCount(); ++i) {
      const auto ev = expect->GetValue(0, i);
      const auto &gv = got[i].values[0];
      if (ev.IsNull() != gv.is_null) {
        Fail(dump(ctx + ": name nullness row " + std::to_string(i)));
      }
      if (!ev.IsNull() && ev.ToString() != std::get<std::string>(gv.payload)) {
        Fail(dump(ctx + ": name row " + std::to_string(i)));
      }
      if (expect->GetValue(1, i).ToString() != std::get<std::string>(got[i].values[1].payload)) {
        Fail(dump(ctx + ": tag row " + std::to_string(i)));
      }
      if (!SameDouble(expect->GetValue(2, i).GetValue<double>(),
                      std::get<double>(got[i].values[2].payload))) {
        Fail(dump(ctx + ": value row " + std::to_string(i)));
      }
    }
  }

  void VerifyRandom(Gen &g, const std::string &label, int draws) {
    Verify(label, false, 2022, false, "pure", true);
    for (int d = 0; d < draws; ++d) {
      static const int64_t fys[] = {2022, 2022, 2022, 2021, 2023, 2012, 0, -1,
                                    2147483647, -2147483647 - 1, 1999};
      const bool fy_null = g.Chance(3);
      const bool uom_null = g.Chance(3);
      std::string uom;
      if (g.Chance(8)) {
        uom = "nonexistent";
      } else if (g.Chance(5)) {
        uom = "";
      } else {
        uom = g.Uom();
      }
      Verify(label, fy_null, fys[g.Pick(11)], uom_null, uom);
    }
  }
};

void Seed(Harness &h, Gen &g, int adsh_count, int num_rows) {
  for (int i = 0; i < adsh_count; ++i) {
    g.adshs.push_back(g.NewAdsh());
    InsertSub(h.con, g, g.adshs.back());
  }
  // Bulk: USD gets > 10000 rows so it is an uncovered ("excluded") uom.
  std::string batch;
  int pending = 0;
  const auto flush = [&] {
    if (pending != 0) {
      Exec(h.con, "INSERT INTO num VALUES " + batch);
      batch.clear();
      pending = 0;
    }
  };
  for (int i = 0; i < num_rows; ++i) {
    std::string uom = g.Uom();
    if (i % 2 == 0 && i < 24000) {
      uom = "USD";
    }
    const std::string adsh = g.Chance(2) ? "NULL" : Quote(g.adshs[g.Pick(g.adshs.size())]);
    const std::string tag = g.Chance(2) ? "NULL" : Quote(g.Tag());
    const std::string uom_sql = g.Chance(1) ? "NULL" : Quote(uom);
    const std::string value = g.Chance(5) ? "NULL" : DoubleSql(g.Value());
    if (pending != 0) {
      batch += ",";
    }
    batch += "(" + adsh + "," + tag + ",'v',20220101,4," + uom_sql + ",NULL," + value + ",NULL)";
    if (++pending == 500) {
      flush();
    }
  }
  flush();
}

// One random write transaction made of 1..3 statements.
void RandomTransaction(Harness &h, Gen &g, int &rollbacks) {
  h.con.BeginTransaction();
  const int statements = 1 + static_cast<int>(g.Pick(3));
  for (int s = 0; s < statements; ++s) {
    core::MutationBatch batch;
    const int op = static_cast<int>(g.Pick(15));
    const auto pick_where = [&]() {
      // 1..4 random existing rows by rowid
      auto r = h.con.Query("SELECT rowid FROM num ORDER BY " + g.SampleOrder("rowid") +
                           " LIMIT " + std::to_string(1 + g.Pick(4)));
      std::string ids;
      Require(r && !r->HasError(), "pick query failed: " + (r ? r->GetError() : std::string("null")));
      for (duckdb::idx_t i = 0; i < r->RowCount(); ++i) {
        ids += (ids.empty() ? "" : ",") + r->GetValue(0, i).ToString();
      }
      return ids.empty() ? std::string("rowid = -1") : "rowid IN (" + ids + ")";
    };
    switch (op) {
    case 0:
    case 1: { // INSERT num
      batch.table = 1;
      batch.kind = core::MutationKind::Insert;
      const int rows = 1 + static_cast<int>(g.Pick(4));
      std::string values;
      for (int i = 0; i < rows; ++i) {
        values += (i ? "," : "") + std::string("(") + Quote(g.adshs[g.Pick(g.adshs.size())]) +
                  "," + Quote(g.Tag()) + ",'v',20220101,4," + Quote(g.Uom()) + ",NULL," +
                  (g.Chance(5) ? "NULL" : DoubleSql(g.Value())) + ",NULL)";
      }
      const auto before = FetchRows(h.con, "SELECT count(*) FROM num");
      Exec(h.con, "INSERT INTO num VALUES " + values);
      batch.new_rows = NumRowsById(h.con, "rowid >= (SELECT max(rowid) - " +
                                              std::to_string(rows - 1) + " FROM num)");
      Require(batch.new_rows.size() == static_cast<size_t>(rows), "inserted rows are the tail");
      (void)before;
      break;
    }
    case 2:
    case 3: { // DELETE num
      batch.table = 1;
      batch.kind = core::MutationKind::Delete;
      const std::string where = pick_where();
      batch.old_rows = NumRowsById(h.con, where);
      Exec(h.con, "DELETE FROM num WHERE " + where);
      break;
    }
    case 4:
    case 5: { // UPDATE value
      batch.table = 1;
      batch.kind = core::MutationKind::Update;
      batch.updated_columns = {7};
      const std::string where = pick_where();
      batch.old_rows = NumRowsById(h.con, where);
      const std::string set =
          g.Chance(50) ? "value = " + DoubleSql(g.Value())
                       : (g.Chance(50) ? "value = value + 1" : "value = NULL");
      Exec(h.con, "UPDATE num SET " + set + " WHERE " + where);
      batch.new_rows = NumRowsById(h.con, where);
      if (g.Chance(30)) {
        batch.updated_columns.clear(); // unknown column set
      }
      break;
    }
    case 6: { // UPDATE key columns (tag / uom / adsh)
      batch.table = 1;
      batch.kind = core::MutationKind::Update;
      const std::string where = pick_where();
      batch.old_rows = NumRowsById(h.con, where);
      const int which = static_cast<int>(g.Pick(3));
      if (which == 0) {
        Exec(h.con, "UPDATE num SET tag = " + Quote(g.Tag()) + " WHERE " + where);
        batch.updated_columns = {1};
      } else if (which == 1) {
        Exec(h.con, "UPDATE num SET uom = " + Quote(g.Uom()) + " WHERE " + where);
        batch.updated_columns = {5};
      } else {
        Exec(h.con, "UPDATE num SET adsh = " + Quote(g.adshs[g.Pick(g.adshs.size())]) +
                        " WHERE " + where);
        batch.updated_columns = {0};
      }
      batch.new_rows = NumRowsById(h.con, where);
      break;
    }
    case 7: { // UPDATE irrelevant column
      batch.table = 1;
      batch.kind = core::MutationKind::Update;
      batch.updated_columns = {8};
      const std::string where = pick_where();
      batch.old_rows = NumRowsById(h.con, where);
      Exec(h.con, "UPDATE num SET footnote = 'f' WHERE " + where);
      batch.new_rows = NumRowsById(h.con, where);
      break;
    }
    case 8: { // INSERT sub (+ rows for it in the same transaction)
      batch.table = 2;
      batch.kind = core::MutationKind::Insert;
      const std::string adsh = g.NewAdsh();
      g.adshs.push_back(adsh);
      InsertSub(h.con, g, adsh);
      batch.new_rows = FetchRows(h.con, "SELECT * FROM sub WHERE adsh = " + Quote(adsh));
      h.Capture(batch);
      batch = core::MutationBatch{};
      batch.table = 1;
      batch.kind = core::MutationKind::Insert;
      std::string values;
      const int rows = 1 + static_cast<int>(g.Pick(3));
      for (int i = 0; i < rows; ++i) {
        values += (i ? "," : "") + std::string("(") + Quote(adsh) + "," + Quote(g.Tag()) +
                  ",'v',20220101,4," + Quote(g.Uom()) + ",NULL," + DoubleSql(g.Value()) + ",NULL)";
      }
      Exec(h.con, "INSERT INTO num VALUES " + values);
      batch.new_rows = NumRowsById(h.con, "adsh = " + Quote(adsh));
      break;
    }
    case 9: { // DELETE sub without children
      auto r = h.con.Query("SELECT adsh FROM sub WHERE adsh NOT IN (SELECT adsh FROM num "
                           "WHERE adsh IS NOT NULL) ORDER BY adsh LIMIT 1");
      Require(r && !r->HasError(), "pick query failed: " + (r ? r->GetError() : std::string("null")));
      if (r->RowCount() == 0) {
        continue;
      }
      const std::string adsh = r->GetValue(0, 0).ToString();
      batch.table = 2;
      batch.kind = core::MutationKind::Delete;
      batch.old_rows = FetchRows(h.con, "SELECT * FROM sub WHERE adsh = " + Quote(adsh));
      Exec(h.con, "DELETE FROM sub WHERE adsh = " + Quote(adsh));
      g.adshs.erase(std::remove(g.adshs.begin(), g.adshs.end(), adsh), g.adshs.end());
      break;
    }
    case 12: { // DELETE the top-k distinct values of one group (forces list shrink / loss)
      auto r = h.con.Query(
          "SELECT adsh, tag, uom FROM num WHERE value IS NOT NULL AND adsh IS NOT NULL AND "
          "tag IS NOT NULL AND uom IN ('pure', 'shares', 'EUR') ORDER BY " +
          g.SampleOrder("rowid") + " LIMIT 1");
      Require(r && !r->HasError(), "pick query failed: " + (r ? r->GetError() : std::string("null")));
      if (r->RowCount() == 0) {
        continue;
      }
      const std::string grp = "adsh = " + Quote(r->GetValue(0, 0).ToString()) +
                              " AND tag = " + Quote(r->GetValue(1, 0).ToString()) +
                              " AND uom = " + Quote(r->GetValue(2, 0).ToString()) +
                              " AND value IS NOT NULL";
      const std::string where =
          grp + " AND value IN (SELECT DISTINCT value FROM num WHERE " + grp +
          " ORDER BY value DESC LIMIT " + std::to_string(1 + g.Pick(4)) + ")";
      batch.table = 1;
      batch.kind = core::MutationKind::Delete;
      batch.old_rows = NumRowsById(h.con, where);
      Exec(h.con, "DELETE FROM num WHERE " + where);
      break;
    }
    case 13: { // lower the maximum of one group (update of the max rows)
      auto r = h.con.Query(
          "SELECT adsh, tag, uom, max(value) FROM num WHERE value IS NOT NULL AND adsh IS NOT "
          "NULL AND tag IS NOT NULL AND uom IN ('pure', 'shares') GROUP BY adsh, tag, uom ORDER BY " +
          g.SampleOrder("adsh, tag, uom") + " LIMIT 1");
      Require(r && !r->HasError(), "pick query failed: " + (r ? r->GetError() : std::string("null")));
      if (r->RowCount() == 0 || r->GetValue(3, 0).IsNull()) {
        continue;
      }
      const std::string where = "adsh = " + Quote(r->GetValue(0, 0).ToString()) +
                                " AND tag = " + Quote(r->GetValue(1, 0).ToString()) +
                                " AND uom = " + Quote(r->GetValue(2, 0).ToString()) +
                                " AND value = " + DoubleSql(r->GetValue(3, 0).GetValue<double>());
      batch.table = 1;
      batch.kind = core::MutationKind::Update;
      batch.updated_columns = {7};
      // The updated rows are identified by rowid, so the old and new images
      // are paired row for row as an engine capture pairs them (matching the
      // new images by value instead mis-pairs them with pre-existing rows of
      // the group, e.g. -inf, or misses 1e18 - 1000).
      std::string ids;
      for (const auto &row : FetchRows(h.con, "SELECT rowid FROM num WHERE " + where +
                                                  " ORDER BY rowid")) {
        ids += (ids.empty() ? "" : ",") + std::to_string(std::get<int64_t>(row.values[0].payload));
      }
      const std::string by_id = ids.empty() ? std::string("rowid = -1") : "rowid IN (" + ids + ")";
      batch.old_rows = NumRowsById(h.con, by_id);
      Exec(h.con, "UPDATE num SET value = " + std::string(g.Chance(50) ? "value - 1000" : "-5") +
                      " WHERE " + by_id);
      batch.new_rows = NumRowsById(h.con, by_id);
      break;
    }
    default: { // UPDATE sub fy / name
      batch.table = 2;
      batch.kind = core::MutationKind::Update;
      const std::string adsh = g.adshs[g.Pick(g.adshs.size())];
      batch.old_rows = FetchRows(h.con, "SELECT * FROM sub WHERE adsh = " + Quote(adsh));
      const int which = static_cast<int>(g.Pick(3));
      if (which == 0) {
        Exec(h.con, "UPDATE sub SET fy = " + g.FySql() + " WHERE adsh = " + Quote(adsh));
        batch.updated_columns = {10};
      } else if (which == 1) {
        Exec(h.con, "UPDATE sub SET name = " + g.NameSql() + " WHERE adsh = " + Quote(adsh));
        batch.updated_columns = {2};
      } else {
        Exec(h.con, "UPDATE sub SET cik = cik + 1 WHERE adsh = " + Quote(adsh));
        batch.updated_columns = {1}; // irrelevant column
      }
      batch.new_rows = FetchRows(h.con, "SELECT * FROM sub WHERE adsh = " + Quote(adsh));
      break;
    }
    }
    h.Capture(batch);
  }
  if (g.Chance(8)) {
    // Roll back: DuckDB discards the writes, the coordinator drops its buffers.
    h.con.Rollback();
    h.coordinator.Abort();
    ++rollbacks;
    g.adshs.clear();
    for (const auto &row : FetchRows(h.con, "SELECT adsh FROM sub")) {
      g.adshs.push_back(std::get<std::string>(row.values[0].payload));
    }
    return;
  }
  h.Commit();
}

void RunRandomized(uint64_t seed, int steps, bool parallel) {
  Gen g(seed);
  Harness h(parallel);
  const uint64_t direct_before = SecEdgarQ2GlobalStats().direct_reads.load();
  Seed(h, g, 150, 26000);
  h.Build();
  h.VerifyRandom(g, "initial", 12);
  int rollbacks = 0;
  for (int step = 0; step < steps; ++step) {
    RandomTransaction(h, g, rollbacks);
    h.VerifyRandom(g, "step " + std::to_string(step), 3);
  }
  h.VerifyRandom(g, "final", 40);
  // Rebuilding from scratch must agree with the incrementally maintained state.
  h.Build();
  h.VerifyRandom(g, "rebuilt", 20);
  const auto &st = SecEdgarQ2GlobalStats();
  if (!std::getenv("Q2_TEST_MAX_UOM_ROWS") && st.direct_reads.load() != direct_before) {
    std::cerr << "FAIL: SQL fallback used although every uom is materialised\n";
    std::exit(1);
  }
  std::cout << "  stats: apply_delta=" << st.apply_delta << " rebuild=" << st.rebuild
            << " lost_lookups=" << st.lost_group_lookups
            << " new_adsh_lookups=" << st.new_adsh_lookups
            << " inconsistent=" << st.inconsistent_deltas << " compactions=" << st.compactions
            << " state_reads=" << st.state_reads << " direct_reads=" << st.direct_reads << "\n";
  std::cout << (parallel ? "parallel" : "single ") << " seed=" << seed << " steps=" << steps
            << " rollbacks=" << rollbacks << " reads=" << h.reads << " ok\n";
}

// A mutation stream that does not match the table (row that never existed) is
// detected and repaired by a rebuild; the answers stay equal to SQL.
void TestBogusStream(uint64_t seed) {
  Gen g(seed);
  Harness h(false);
  Seed(h, g, 30, 2000);
  h.Build();
  auto rows = FetchRows(h.con, "SELECT adsh, tag, version, ddate, qtrs, uom, coreg, value, "
                               "footnote FROM num WHERE uom = 'pure' AND adsh IS NOT NULL AND "
                               "tag IS NOT NULL AND value IS NOT NULL AND NOT EXISTS (SELECT 1 FROM num m "
                               "WHERE m.adsh = num.adsh AND m.tag = num.tag AND m.uom = 'pure' "
                               "AND m.value >= 1e30) ORDER BY rowid LIMIT 1");
  // (a group whose maximum is below 1e30, NaN counting as greatest: deleting
  // 1e30 from it is detectably inconsistent; under a NaN / +inf maximum with
  // unlisted values it would be indistinguishable from a real unlisted row)
  Require(!rows.empty(), "need a pure row");
  core::MutationBatch bogus;
  bogus.table = 1;
  bogus.kind = core::MutationKind::Delete;
  bogus.old_rows = rows;
  bogus.old_rows[0].values[7] = core::AuxValue::Of(core::AuxType::Double, 1e30); // never existed
  const uint64_t before = SecEdgarQ2GlobalStats().inconsistent_deltas;
  h.con.BeginTransaction();
  h.Capture(bogus);
  h.Commit();
  const char *forced = std::getenv("ACCEL_FORCE_MAINTENANCE_ACTION");
  if (forced == nullptr || std::string(forced) == "ApplyDelta") {
    Require(SecEdgarQ2GlobalStats().inconsistent_deltas == before + 1, "inconsistency detected");
  }
  h.VerifyRandom(g, "bogus", 20);
  std::cout << "bogus stream ok\n";
}

// Groups whose row counts exceed the base's one-byte count encoding (>= 127 /
// >= 255 identical values) survive build, deletes, inserts and compaction.
void TestLargeCounts(uint64_t seed, bool parallel) {
  Gen g(seed);
  Harness h(parallel);
  g.adshs.push_back(g.NewAdsh());
  g.adshs.push_back(g.NewAdsh());
  Exec(h.con, "INSERT INTO sub (adsh, cik, name, fy) VALUES (" + Quote(g.adshs[0]) +
                  ", 1, 'Big', 2022), (" + Quote(g.adshs[1]) + ", 1, 'Small', 2022)");
  const auto insert = [&](const std::string &adsh, const char *value, int rows) {
    std::string values;
    for (int i = 0; i < rows; ++i) {
      values += std::string(i ? "," : "") + "(" + Quote(adsh) + ",'T','v',20220101,4,'USD',NULL," +
                value + ",NULL)";
    }
    Exec(h.con, "INSERT INTO num VALUES " + values);
  };
  insert(g.adshs[0], "5.0", 300);
  insert(g.adshs[0], "3.0", 130);
  insert(g.adshs[0], "1.0", 2);
  insert(g.adshs[1], "5.0", 127);
  insert(g.adshs[1], "4.0", 126);
  h.Build();
  h.VerifyRandom(g, "large initial", 6);
  h.Verify("large 2022 USD", false, 2022, false, "USD");
  for (int round = 0; round < 6; ++round) {
    h.con.BeginTransaction();
    core::MutationBatch batch;
    batch.table = 1;
    batch.kind = (round % 2 == 0) ? core::MutationKind::Delete : core::MutationKind::Insert;
    if (batch.kind == core::MutationKind::Delete) {
      const std::string where =
          "rowid IN (SELECT rowid FROM num WHERE value = 5.0 ORDER BY " + g.SampleOrder("rowid") +
          " LIMIT " + std::to_string(60 + round * 40) + ")";
      batch.old_rows = NumRowsById(h.con, where);
      Exec(h.con, "DELETE FROM num WHERE " + where);
    } else {
      insert(g.adshs[round % 2], "5.0", 70);
      batch.new_rows = NumRowsById(h.con, "rowid >= (SELECT max(rowid) - 69 FROM num)");
    }
    h.Capture(batch);
    h.Commit();
    h.Verify("large round " + std::to_string(round), false, 2022, false, "USD");
  }
  h.Build();
  h.Verify("large rebuilt", false, 2022, false, "USD");
  std::cout << "large counts ok\n";
}

void TestCodec(uint64_t seed) {
  Gen g(seed);
  Harness h(false);
  Seed(h, g, 40, 3000);
  const auto state = h.module->BuildState();
  const auto bytes = EncodeSecEdgarQ2State(*state);
  SecEdgarQ2State decoded;
  Require(DecodeSecEdgarQ2State(bytes, decoded), "decode");
  Require(EncodeSecEdgarQ2State(decoded) == bytes, "codec round trip is stable");
  for (int i = 0; i < 20; ++i) {
    std::vector<SecEdgarQ2OutputRow> a, b;
    const std::string uom = g.Uom();
    const bool ra = FinalizeSecEdgarQ2(*state, 2022, uom, a);
    const bool rb = FinalizeSecEdgarQ2(decoded, 2022, uom, b);
    Require(ra == rb && a.size() == b.size(), "decoded state answers differently");
    for (size_t k = 0; k < a.size(); ++k) {
      Require(a[k].name == b[k].name && a[k].tag == b[k].tag &&
                  SameDouble(a[k].value, b[k].value),
              "decoded state row differs");
    }
  }
  std::vector<uint8_t> truncated(bytes.begin(), bytes.begin() + bytes.size() / 2);
  SecEdgarQ2State junk;
  Require(!DecodeSecEdgarQ2State(truncated, junk), "truncated image is rejected");
  std::cout << "codec ok (" << bytes.size() << " bytes, heap " << state->HeapBytes() << ")\n";
}

// accel_sec_edgar_q2_aux[_parallel]() / (fy, uom) through SQL.
void TestTableFunctions(uint64_t seed, bool parallel) {
  Gen g(seed);
  duckdb::DuckDB db(nullptr);
  duckdb::Connection con(db);
  accelerator_codegen::RegisterAcceleratorFunctions(con);
  Exec(con, kSchema);
  g.adshs.clear();
  for (int i = 0; i < 60; ++i) {
    g.adshs.push_back(g.NewAdsh());
    InsertSub(con, g, g.adshs.back());
  }
  for (int i = 0; i < 1500; ++i) {
    Exec(con, "INSERT INTO num VALUES (" + Quote(g.adshs[g.Pick(g.adshs.size())]) + "," +
                  Quote(g.Tag()) + ",'v',20220101,4," + Quote(g.Uom()) + ",NULL," +
                  DoubleSql(g.Value()) + ",NULL)");
  }
#ifdef ACCEL_ENABLE_PARALLEL_AUX
  const auto &entry = parallel ? engine::LookupSharedQueryParallel("sec_edgar", 2)
                               : engine::LookupSharedQuery("sec_edgar", 2);
#else
  if (parallel) {
    return;
  }
  const auto &entry = engine::LookupSharedQuery("sec_edgar", 2);
#endif
  engine::duckdb::DuckdbRuntime runtime(con, entry, 10.0);
  runtime.Build();
  const std::string fn = parallel ? "accel_sec_edgar_q2_aux_parallel" : "accel_sec_edgar_q2_aux";
  const auto compare = [&](const std::string &call, const std::string &fy,
                           const std::string &uom) {
    auto got = con.Query("SELECT * FROM " + call);
    auto want = con.Query(QuerySql(fy, uom));
    Require(got && !got->HasError(), "table function failed: " + (got ? got->GetError() : ""));
    Require(want && !want->HasError(), "reference failed");
    Require(got->RowCount() == want->RowCount(), call + ": row count");
    for (duckdb::idx_t r = 0; r < want->RowCount(); ++r) {
      for (duckdb::idx_t c = 0; c < 3; ++c) {
        const auto a = got->GetValue(c, r);
        const auto b = want->GetValue(c, r);
        Require(a.IsNull() == b.IsNull(), call + ": nullness");
        if (!a.IsNull()) {
          if (c == 2) {
            Require(SameDouble(a.GetValue<double>(), b.GetValue<double>()), call + ": value");
          } else {
            Require(a.ToString() == b.ToString(), call + ": text");
          }
        }
      }
    }
  };
  compare(fn + "()", "2022", "'pure'");
  compare(fn + "(2022, 'pure')", "2022", "'pure'");
  compare(fn + "(2021, 'USD')", "2021", "'USD'");
  compare(fn + "(2023, 'x''y')", "2023", "'x''y'");
  compare(fn + "(CAST(NULL AS INTEGER), 'pure')", "NULL", "'pure'");
  compare(fn + "(2022, NULL)", "2022", "NULL");
  // a write through the runtime keeps the table function consistent
  auto rows = FetchRows(con, "SELECT adsh, tag, version, ddate, qtrs, uom, coreg, value, "
                             "footnote FROM num WHERE uom = 'pure' AND value IS NOT NULL "
                             "ORDER BY value DESC LIMIT 1");
  if (!rows.empty() && !std::isinf(std::get<double>(rows[0].values[7].payload)) &&
      !std::isnan(std::get<double>(rows[0].values[7].payload))) {
    core::MutationBatch m;
    m.table = 1;
    m.kind = core::MutationKind::Update;
    m.updated_columns = {7};
    m.old_rows = rows;
    Exec(con, "UPDATE num SET value = value + 5 WHERE rowid = (SELECT rowid FROM num WHERE "
              "uom = 'pure' AND value IS NOT NULL AND value = " +
                  DoubleSql(std::get<double>(rows[0].values[7].payload)) + " LIMIT 1)");
    m.new_rows = {rows[0]};
    m.new_rows[0].values[7] =
        core::AuxValue::Of(core::AuxType::Double, std::get<double>(rows[0].values[7].payload) + 5);
    runtime.ApplyMutationBatch(m);
    compare(fn + "()", "2022", "'pure'");
    compare(fn + "(2023, 'pure')", "2023", "'pure'");
  }
  std::cout << "table functions " << (parallel ? "(parallel) " : "") << "ok\n";
}

// The native storage scan (dictionary / FSST / constant vectors, deleted rows,
// uncommitted rows of the open transaction) must build exactly the state the
// SQL GROUP BY path builds.  Run on a checkpointed file database so the
// string columns are dictionary-compressed.
class SqlOnlyScan final : public SecEdgarQ2DuckdbScan {
public:
  using SecEdgarQ2DuckdbScan::SecEdgarQ2DuckdbScan;
  bool ScanNumIds(SecEdgarQ2KeyMap &, const std::function<void(const SecEdgarQ2IdBatch &)> &) override {
    return false;
  }
};

using GroupMap = std::map<std::tuple<std::string, std::string, std::string>,
                          std::tuple<int64_t, int64_t, uint32_t, uint32_t, bool>>;

GroupMap Groups(const SecEdgarQ2State &s) {
  GroupMap out;
  for (uint32_t u = 0; u < s.uoms->names.size(); ++u) {
    for (const auto &g : s.LiveGroups(u)) {
      out[{std::string(s.uoms->names.Get(u)), std::string(s.adsh.Get(g.adsh)),
           std::string(s.tag.Get(g.tag))}] = {g.k0, g.c1 ? g.k1 : 0, g.c0, g.c1, g.trunc};
    }
  }
  return out;
}

void TestNativeScan(uint64_t seed) {
  const std::string path = "/tmp/accel_q2_native_scan_test_" + std::to_string(::getpid()) + ".duckdb";
  std::remove(path.c_str());
  std::remove((path + ".wal").c_str());
  {
    duckdb::DuckDB db(path);
    duckdb::Connection con(db);
    Exec(con, kSchema);
    const std::string s = std::to_string(seed % 1000);
    // 600 filings (a few non-canonical accession numbers), 4000 tags, 6 units,
    // NULLs in every key column and in value, many duplicate values.
    Exec(con, "INSERT INTO sub (adsh, cik, name, fy) SELECT CASE WHEN i % 97 = 0 THEN 'odd-' || i "
              "ELSE printf('%010d-22-%06d', 1000000 + i % 50, i) END, 1, "
              "CASE WHEN i % 13 = 0 THEN NULL ELSE 'Co ' || (i % 41) END, "
              "CASE WHEN i % 11 = 0 THEN NULL ELSE 2020 + i % 4 END FROM range(600) t(i)");
    Exec(con, "CREATE TEMP TABLE ids AS SELECT row_number() OVER () - 1 AS k, adsh FROM sub");
    Exec(con,
         "INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, coreg, value) "
         "SELECT CASE WHEN h % 251 = 0 THEN NULL ELSE ids.adsh END, "
         "CASE WHEN h % 263 = 0 THEN NULL ELSE 'Tag' || ((h // 7) % 4000) END, 'v', 20220101, 4, "
         "CASE WHEN h % 271 = 0 THEN NULL ELSE list_extract(['pure','USD','USD','shares','EUR','x''y'], "
         "((h // 3) % 6 + 1)::BIGINT) END, "
         "NULL, CASE WHEN h % 17 = 0 THEN NULL WHEN h % 5 = 0 THEN 1.0 ELSE ((h * 7919) % 1000) / 4.0 - 50 END "
         "FROM (SELECT (hash(i + " + s + ") % 1000003)::BIGINT AS h, i FROM range(300000) t(i)) r "
         "JOIN ids ON ids.k = r.h % 600 ORDER BY r.i");
    Exec(con, "CHECKPOINT");
    // Deleted rows inside compressed segments, then uncommitted local rows.
    Exec(con, "DELETE FROM num WHERE value BETWEEN 10 AND 12");
    Exec(con, "CHECKPOINT");
    auto info = con.Query("SELECT count(*) FROM pragma_storage_info('num') WHERE column_name = 'tag' "
                          "AND compression = 'Dictionary'");
    Require(info && !info->HasError() && info->GetValue(0, 0).GetValue<int64_t>() > 0,
            "test data is dictionary-compressed");
    Exec(con, "BEGIN");
    Exec(con, "INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, coreg, value) "
              "SELECT ids.adsh, 'Local' || (i % 10), 'v', 1, 1, 'pure', NULL, i "
              "FROM range(5000) t(i) JOIN ids ON ids.k = i % 600");
    Exec(con, "UPDATE num SET value = value + 1000 WHERE uom = 'EUR' AND value < 0");

    SecEdgarQ2Config cfg{1, 2};
    SecEdgarQ2DuckdbScan native(con);
    SqlOnlyScan sql(con);
    SecEdgarQ2Module mn(cfg, &native), ms(cfg, &sql);
    const auto a = mn.BuildState();
    const auto b = ms.BuildState();
    const GroupMap ga = Groups(*a), gb = Groups(*b);
    Require(ga.size() > 1000, "native scan built groups");
    Require(ga == gb, "native scan state == SQL path state (" + std::to_string(ga.size()) +
                          " vs " + std::to_string(gb.size()) + " groups)");
    for (const char *uom : {"pure", "USD", "EUR", "x'y", "shares", "none"}) {
      for (int64_t fy : {2020, 2021, 2022, 2023}) {
        std::vector<SecEdgarQ2OutputRow> ra, rb;
        Require(FinalizeSecEdgarQ2(*a, fy, uom, ra) && FinalizeSecEdgarQ2(*b, fy, uom, rb),
                "covered");
        Require(ra.size() == rb.size(), "native / SQL answers differ");
        for (size_t i = 0; i < ra.size(); ++i) {
          Require(ra[i].name == rb[i].name && ra[i].name_is_null == rb[i].name_is_null &&
                      ra[i].tag == rb[i].tag && SameDouble(ra[i].value, rb[i].value),
                  "native / SQL answer row differs");
        }
      }
    }
    // The opt-out keeps its meaning on the native path.
    SecEdgarQ2Config small = cfg;
    small.max_uom_rows = 10000;
    SecEdgarQ2Module mx(small, &native);
    const auto x = mx.BuildState();
    std::vector<SecEdgarQ2OutputRow> rx;
    Require(!FinalizeSecEdgarQ2(*x, 2022, "USD", rx), "USD is excluded by max_uom_rows");
    Require(FinalizeSecEdgarQ2(*x, 2022, "pure", rx), "default unit stays covered");
    Exec(con, "ROLLBACK");
  }
  std::remove(path.c_str());
  std::remove((path + ".wal").c_str());
  std::cout << "native scan ok\n";
}

// Regression: the base kernel's per-dictionary-index adsh cache was kept while
// the dictionary's address repeated, but a later chunk can carry other strings
// behind a recycled address (one company's row then got another's name).
// Slots may carry over only for the same reusable (id-carrying) dictionary.
void TestDictionarySlotCache() {
  using accelerator_codegen::DictionarySlotCache;
  duckdb::Vector strings(duckdb::LogicalType::VARCHAR, 4);
  auto *data = duckdb::FlatVector::GetData<duckdb::string_t>(strings);
  duckdb::SelectionVector sel(4);
  for (duckdb::idx_t i = 0; i < 4; ++i) sel.set_index(i, 3 - i);
  DictionarySlotCache<int> cache;

  // Two chunks slicing the same buffer, with other bytes the second time: the
  // dictionaries share an address and carry no id.
  data[0] = duckdb::string_t("0000000001-23-000001");
  duckdb::Vector chunk1(strings);
  chunk1.Slice(sel, 4);
  if (chunk1.GetVectorType() != duckdb::VectorType::DICTIONARY_VECTOR ||
      !duckdb::DictionaryVector::DictionaryId(chunk1).empty())
    throw std::runtime_error("dictionary slot cache: expected an id-less dictionary");
  cache.BeginChunk(chunk1);
  cache.Store(0, 1);
  data[0] = duckdb::string_t("0000000002-23-000002");
  duckdb::Vector chunk2(strings);
  chunk2.Slice(sel, 4);
  if (duckdb::DictionaryVector::Child(chunk2).GetData() != duckdb::DictionaryVector::Child(chunk1).GetData())
    throw std::runtime_error("dictionary slot cache: expected a recycled dictionary address");
  cache.BeginChunk(chunk2);
  if (cache.Find(0) != nullptr)
    throw std::runtime_error("dictionary slot cache: slot kept across a recycled id-less dictionary");

  // A reusable dictionary keeps its slots across chunks; another one does not.
  auto dict_a = duckdb::DictionaryVector::CreateReusableDictionary(duckdb::LogicalType::VARCHAR, 4);
  auto dict_b = duckdb::DictionaryVector::CreateReusableDictionary(duckdb::LogicalType::VARCHAR, 4);
  duckdb::Vector a1(duckdb::LogicalType::VARCHAR), a2(duckdb::LogicalType::VARCHAR), b1(duckdb::LogicalType::VARCHAR);
  a1.Dictionary(dict_a, sel);
  a2.Dictionary(dict_a, sel);
  b1.Dictionary(dict_b, sel);
  cache.BeginChunk(a1);
  cache.Store(2, 7);
  cache.BeginChunk(a2);
  if (cache.Find(2) == nullptr || *cache.Find(2) != 7)
    throw std::runtime_error("dictionary slot cache: slot lost within one reusable dictionary");
  cache.BeginChunk(b1);
  if (cache.Find(2) != nullptr)
    throw std::runtime_error("dictionary slot cache: slot kept across reusable dictionaries");
  std::cout << "dictionary slot cache ok\n";
}

} // namespace

int main(int argc, char **argv) {
  // Seed: argv[1], else $ACCEL_TEST_SEED, else 1 (ctest is deterministic).
  const char *seed_env = std::getenv("ACCEL_TEST_SEED");
  const uint64_t seed = argc > 1   ? std::strtoull(argv[1], nullptr, 10)
                        : seed_env ? std::strtoull(seed_env, nullptr, 10)
                                   : 1;
  const int steps = argc > 2 ? std::atoi(argv[2]) : 120;
  g_replay = std::to_string(seed) + " " + std::to_string(steps);
  std::cout << "seed=" << seed << " steps=" << steps << "\n";
  std::mt19937_64 rng(seed);
  TestDictionarySlotCache();
  TestGroupAlgebra(rng);
  std::cout << "group algebra ok\n";
  TestCodec(seed);
  TestBogusStream(seed);
  RunRandomized(seed, steps, false);
  RunRandomized(seed + 1000, steps / 2, true);
  TestLargeCounts(seed, false);
  TestLargeCounts(seed, true);
  TestTableFunctions(seed, false);
  TestTableFunctions(seed, true);
  TestNativeScan(seed);
  std::cout << "ALL OK\n";
  return 0;
}
