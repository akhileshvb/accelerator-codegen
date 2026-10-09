// Randomized differential test for the SEC-EDGAR Q3 aux accelerator against
// plain DuckDB SQL.
//
//  * Real DuckDB tables with the production DDL (in-memory, synthetic data).
//  * Every step executes real SQL (INSERT / DELETE / UPDATE on `num` and
//    `sub`, single-row and bulk, join-key / filter-column updates, NULLs and
//    edge values), then feeds the matching mutation batch to the runtime's
//    coordinator and compares the accelerator against the reference SQL
//    (`entry.base_sql`).
//  * Q3 is parameterized as accel_sec_edgar_q3_aux(fy BIGINT, uom VARCHAR)
//    (defaults 2022 / 'USD'). Every verification compares the defaults and
//    several random (fy, uom) drawn from the domain {2021..2024} x {USD,
//    shares, pure, CAD, CNY, EUR} plus edge values (2019, 2025, 0, NULL
//    arguments, unknown / case-variant / empty uom) against the reference SQL
//    with the parameters substituted, interleaved with random writes. The
//    data (fy, uom, name, cik, value, adsh format) is drawn at random too.
//
// Usage: test_sec_edgar_q3_differential [iterations_per_seed] [seeds]
//        [real_db_copy]   (optional: also run a short write sequence on a
//                           writable copy of the real SF3 database)
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "accelerator_codegen/harness/differential.hpp"
#include "accelerator_codegen/runtime/duckdb/duckdb_runtime.hpp"
#include "accelerator_codegen/runtime/duckdb/register.hpp"
#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"

namespace {

namespace catalog = accelerator_codegen::aux_state::engine;
namespace harness = accelerator_codegen::aux_state::harness;
namespace core = accelerator_codegen::aux_state::core;
using catalog::duckdb::DuckdbRuntime;

constexpr core::TableId kNumTable = 1;
constexpr core::TableId kSubTable = 2;

void Require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::string g_last_sql;

void Exec(duckdb::Connection &c, const std::string &sql) {
  g_last_sql = sql;
  auto r = c.Query(sql);
  Require(r && !r->HasError(), (r ? r->GetError() : "null result") + " :: " + sql);
}

// ---- model rows ----------------------------------------------------------------

struct NumM {
  std::string adsh;
  bool adsh_null = false;
  std::string tag;
  std::string uom = "USD";
  bool uom_null = false;
  bool value_null = false;
  double value = 0.0;
};

struct SubM {
  std::string adsh;
  std::string name;
  bool name_null = false;
  int cik = 0;
  bool cik_null = false;
  int fy = 2022;
  bool fy_null = false;
};

std::string Q(const std::string &s) {
  std::string out = "'";
  for (char c : s) {
    out += c;
    if (c == '\'') {
      out += '\'';
    }
  }
  return out + "'";
}

std::string D(double v) {
  if (std::isnan(v)) {
    return "'nan'::DOUBLE";
  }
  if (std::isinf(v)) {
    return v > 0 ? "'inf'::DOUBLE" : "'-inf'::DOUBLE";
  }
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.17g::DOUBLE", v);
  return buf;
}

std::string NumValues(const NumM &n) {
  return "(" + (n.adsh_null ? std::string("NULL") : Q(n.adsh)) + "," + Q(n.tag) +
         ",'v1',20220331,4," + (n.uom_null ? std::string("NULL") : Q(n.uom)) +
         ",''," + (n.value_null ? std::string("NULL") : D(n.value)) + ")";
}

std::string SubValues(const SubM &s) {
  return "(" + Q(s.adsh) + "," + (s.cik_null ? std::string("NULL") : std::to_string(s.cik)) +
         "," + (s.name_null ? std::string("NULL") : Q(s.name)) + "," +
         (s.fy_null ? std::string("NULL") : std::to_string(s.fy)) + ")";
}

core::AuxValue Str(const std::string &v) {
  return core::AuxValue::Of(core::AuxType::String, v);
}
core::AuxValue Int(int64_t v) { return core::AuxValue::Of(core::AuxType::Int64, v); }

core::AuxRow NumRowOf(const NumM &m) {
  core::AuxRow row;
  row.values = {m.adsh_null ? core::AuxValue::Null(core::AuxType::String) : Str(m.adsh),
                Str(m.tag),
                Str("v1"),
                Int(20220331),
                Int(4),
                m.uom_null ? core::AuxValue::Null(core::AuxType::String) : Str(m.uom),
                Str(""),
                m.value_null ? core::AuxValue::Null(core::AuxType::Double)
                             : core::AuxValue::Of(core::AuxType::Double, m.value)};
  return row;
}

core::AuxRow SubRowOf(const SubM &m) {
  core::AuxRow row;
  row.values = {Str(m.adsh),
                m.name_null ? core::AuxValue::Null(core::AuxType::String) : Str(m.name),
                m.cik_null ? core::AuxValue::Null(core::AuxType::Int64) : Int(m.cik),
                m.fy_null ? core::AuxValue::Null(core::AuxType::Int64) : Int(m.fy)};
  return row;
}

// ---- generator -----------------------------------------------------------------

struct Gen {
  explicit Gen(uint32_t seed) : rng(seed) {
    for (int i = 0; i < 40; ++i) {
      char buf[32];
      std::snprintf(buf, sizeof buf, "%010d-22-%06d", 7000 + i % 17, i);
      adsh_pool.emplace_back(buf);
    }
    adsh_pool.push_back("short");
    adsh_pool.push_back("0000000000-00-000000");
    adsh_pool.push_back("0000000000-00-00000x");
    adsh_pool.push_back("it's-quoted");
    for (int i = 0; i < 14; ++i) {
      names.push_back("Company " + std::to_string(i));
    }
    names.push_back("");
    names.push_back("\xE6\x97\xA5\xE6\x9C\xAC\xE6\xA0\xAA\xE5\xBC\x8F\xE4\xBC\x9A\xE7\xA4\xBE");
    names.push_back(std::string(300, 'x'));
    ciks = {0, 1, -1, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
            std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::min()};
  }
  int Int(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); }
  template <typename T> const T &Pick(const std::vector<T> &v) {
    return v[Int(0, static_cast<int>(v.size()) - 1)];
  }
  double Value() {
    double v = std::uniform_real_distribution<double>(-3000.0, 12000.0)(rng);
    switch (Int(0, 40)) {
    case 0: return 0.0;
    case 1: return -0.0;
    case 2: return 1e-9 * v;
    case 3: return v * 1e5;
    case 4: return std::numeric_limits<double>::infinity() * (Int(0, 1) ? 1 : -1);
    case 5: return std::numeric_limits<double>::quiet_NaN();
    case 6: return -std::fabs(v);
    default: return v;
    }
  }
  NumM RandomNum() {
    NumM n;
    n.adsh = Pick(adsh_pool);
    n.adsh_null = Int(0, 50) == 0;
    n.tag = "T" + std::to_string(next_tag++);
    static const std::vector<std::string> uoms = {"USD", "USD", "USD", "USD", "usd",
                                                  "EUR", "pure", "shares", "", "CAD",
                                                  "CNY", "shares", "pure"};
    n.uom = Pick(uoms);
    n.uom_null = Int(0, 40) == 0;
    n.value_null = Int(0, 12) == 0;
    n.value = Value();
    return n;
  }
  SubM RandomSub(const std::string &adsh) {
    SubM s;
    s.adsh = adsh;
    s.name = Pick(names);
    s.name_null = Int(0, 30) == 0;
    s.cik = Pick(ciks);
    s.cik_null = Int(0, 40) == 0;
    static const std::vector<int> fys = {2022, 2022, 2022, 2022, 2021, 2023, 2024,
                                         2012, 2025, 2021, 2023};
    s.fy = Pick(fys);
    s.fy_null = Int(0, 30) == 0;
    return s;
  }
  std::mt19937 rng;
  std::vector<std::string> adsh_pool, names;
  std::vector<int> ciks;
  int next_tag = 1;
};

// ---- fixture ---------------------------------------------------------------------

struct Fixture {
  Fixture(duckdb::Connection &connection, const catalog::SharedQueryCatalogEntry &entry)
      : connection(connection), entry(entry), runtime(connection, entry, 10.0) {}

  // base_sql with `s.fy = 2022` / `n.uom = 'USD'` replaced by the parameters.
  std::string ParamSql(const std::string &fy_sql, const std::string &uom_sql) const {
    std::string sql = entry.base_sql;
    const auto replace_all = [&](const std::string &from, const std::string &to) {
      std::size_t at = 0;
      int count = 0;
      while ((at = sql.find(from, at)) != std::string::npos) {
        sql.replace(at, from.size(), to);
        at += to.size();
        ++count;
      }
      Require(count == 1, "base_sql shape changed: " + from);
    };
    replace_all("s.fy = 2022", "s.fy = " + fy_sql);
    replace_all("s2.fy = 2022", "s2.fy = " + fy_sql);
    replace_all("n.uom = 'USD'", "n.uom = " + uom_sql);
    replace_all("n2.uom = 'USD'", "n2.uom = " + uom_sql);
    return sql;
  }

  // ORDER BY total DESC leaves the order of equal totals (e.g. several groups
  // at +inf) unspecified in SQL, so rows are compared as ordered totals plus,
  // within each run of equal totals, the multiset of (name, cik) keys.
  void CompareParamRows(const std::string &sql, const std::vector<core::AuxRow> &got) {
    auto result = connection.Query(sql);
    Require(result && !result->HasError(), result ? result->GetError() : "reference failed");
    Require(result->RowCount() == got.size(),
            "row count sql=" + std::to_string(result->RowCount()) +
                " aux=" + std::to_string(got.size()));
    const auto same_total = [](double x, double y) {
      if (std::isnan(x) || std::isnan(y)) {
        return std::isnan(x) && std::isnan(y);
      }
      if (std::isinf(x) || std::isinf(y)) {
        return x == y;
      }
      return std::fabs(x - y) <= 1e-9 * std::max(1.0, std::fabs(x));
    };
    const auto sql_key = [&](duckdb::idx_t r) {
      const auto name = result->GetValue(0, r), cik = result->GetValue(1, r);
      return (name.IsNull() ? std::string("\x01NULL") : name.ToString()) + "|" +
             (cik.IsNull() ? std::string("NULL") : cik.ToString());
    };
    const auto aux_key = [&](std::size_t r) {
      const auto &v = got[r].values;
      return (v[0].is_null ? std::string("\x01NULL") : std::get<std::string>(v[0].payload)) + "|" +
             (v[1].is_null ? std::string("NULL") : std::to_string(std::get<int64_t>(v[1].payload)));
    };
    for (duckdb::idx_t r = 0; r < result->RowCount();) {
      duckdb::idx_t end = r + 1;
      const double total = result->GetValue(2, r).GetValue<double>();
      for (; end < result->RowCount() &&
             same_total(total, result->GetValue(2, end).GetValue<double>());
           ++end) {
      }
      std::vector<std::string> want_keys, got_keys;
      for (duckdb::idx_t i = r; i < end; ++i) {
        Require(same_total(result->GetValue(2, i).GetValue<double>(),
                           std::get<double>(got[i].values[2].payload)),
                "total mismatch at row " + std::to_string(i));
        want_keys.push_back(sql_key(i));
        got_keys.push_back(aux_key(i));
      }
      std::sort(want_keys.begin(), want_keys.end());
      std::sort(got_keys.begin(), got_keys.end());
      Require(want_keys == got_keys, "group mismatch in rows " + std::to_string(r) + ".." +
                                         std::to_string(end));
      r = end;
    }
  }

  void VerifyParams(Gen &gen) {
    static const std::vector<int64_t> fys = {2021, 2022, 2022, 2023, 2024, 2019, 2025, 0,
                                             -1, 2147483647};
    static const std::vector<std::string> uoms = {"USD", "USD", "shares", "pure", "CAD",
                                                  "CNY", "EUR", "nonexistent", "usd", "",
                                                  "USD "};
    const bool fy_null = gen.Int(0, 14) == 0;
    const bool uom_null = gen.Int(0, 14) == 0;
    const int64_t fy = gen.Pick(fys);
    const std::string uom = gen.Pick(uoms);
    core::QueryArguments args;
    args.values.push_back(fy_null ? core::AuxValue::Null(core::AuxType::Int64) : Int(fy));
    args.values.push_back(uom_null ? core::AuxValue::Null(core::AuxType::String) : Str(uom));
    const std::string sql = ParamSql(fy_null ? "NULL" : std::to_string(fy),
                                     uom_null ? "NULL" : Q(uom));
    try {
      auto rows = runtime.ExecuteCollect(args);
      param_nonempty += rows.empty() ? 0 : 1;
      CompareParamRows(sql, rows);
    } catch (const std::exception &e) {
      throw std::runtime_error("params fy=" + (fy_null ? std::string("NULL") : std::to_string(fy)) +
                               " uom=" + (uom_null ? std::string("NULL") : Q(uom)) + ": " +
                               e.what());
    }
    ++param_verifications;
  }

  void Verify(const std::string &context, Gen &gen) {
    try {
      auto rows = runtime.ExecuteCollect();
      nonempty += rows.empty() ? 0 : 1;
      harness::CompareAuxRowsToBase(connection, entry, rows);
      for (int i = 0; i < 3; ++i) {
        VerifyParams(gen);
      }
    } catch (const std::exception &e) {
      throw std::runtime_error(context + ": " + e.what() + " [last sql: " +
                               g_last_sql.substr(0, 400) + "]");
    }
    ++verifications;
    // SQL table-function path (what a client really calls).
    if (verifications % 7 == 0) {
      auto base = connection.Query(entry.base_sql);
      auto aux = connection.Query(entry.duckdb_aux_sql);
      Require(base && !base->HasError(), "base");
      Require(aux && !aux->HasError(), aux ? aux->GetError() : "aux null");
      Require(base->RowCount() == aux->RowCount(),
              context + ": table function row count " + std::to_string(aux->RowCount()) +
                  " vs " + std::to_string(base->RowCount()));
      for (duckdb::idx_t r = 0; r < base->RowCount(); ++r) {
        for (duckdb::idx_t c = 0; c < base->ColumnCount(); ++c) {
          const auto a = base->GetValue(c, r);
          const auto b = aux->GetValue(c, r);
          if (a.IsNull() || b.IsNull()) {
            Require(a.IsNull() == b.IsNull(), context + ": table function null mismatch");
            continue;
          }
          if (c == 2) {
            const double x = a.GetValue<double>(), y = b.GetValue<double>();
            Require((std::isnan(x) && std::isnan(y)) ||
                        std::fabs(x - y) <= 1e-6 * std::max(1.0, std::fabs(x)),
                    context + ": table function value mismatch");
          } else {
            Require(a.ToString() == b.ToString(),
                    context + ": table function cell mismatch " + a.ToString() + " vs " +
                        b.ToString());
          }
        }
      }
    }
  }

  void Send(core::TableId table, core::MutationKind kind, std::vector<core::AuxRow> old_rows,
            std::vector<core::AuxRow> new_rows, std::vector<core::ColumnId> cols = {}) {
    core::MutationBatch batch;
    batch.table = table;
    batch.kind = kind;
    batch.old_rows = std::move(old_rows);
    batch.new_rows = std::move(new_rows);
    batch.updated_columns = std::move(cols);
    runtime.ApplyMutationBatch(batch);
  }

  duckdb::Connection &connection;
  const catalog::SharedQueryCatalogEntry &entry;
  DuckdbRuntime runtime;
  std::vector<NumM> num;
  std::map<std::string, SubM> sub;
  uint64_t verifications = 0;
  uint64_t nonempty = 0;
  uint64_t param_verifications = 0;
  uint64_t param_nonempty = 0;
};

void CreateSchema(duckdb::Connection &c) {
  Exec(c,
       "CREATE TABLE num(adsh VARCHAR, tag VARCHAR, \"version\" VARCHAR, ddate INTEGER, "
       "qtrs INTEGER, uom VARCHAR, coreg VARCHAR, \"value\" DOUBLE, footnote VARCHAR)");
  Exec(c,
       "CREATE TABLE sub(adsh VARCHAR PRIMARY KEY, cik INTEGER, \"name\" VARCHAR, sic INTEGER, "
       "countryba VARCHAR, stprba VARCHAR, cityba VARCHAR, countryinc VARCHAR, form VARCHAR, "
       "period INTEGER, fy INTEGER, fp VARCHAR, filed INTEGER, accepted VARCHAR, prevrpt INTEGER, "
       "nciks INTEGER, afs VARCHAR, wksi INTEGER, fye VARCHAR, instance VARCHAR)");
}

void InsertNumSql(duckdb::Connection &c, const std::vector<NumM> &rows) {
  if (rows.empty()) {
    return;
  }
  std::string sql = "INSERT INTO num (adsh, tag, \"version\", ddate, qtrs, uom, coreg, \"value\") VALUES ";
  for (std::size_t i = 0; i < rows.size(); ++i) {
    sql += (i ? "," : "") + NumValues(rows[i]);
  }
  Exec(c, sql);
}

void InsertSubSql(duckdb::Connection &c, const SubM &s) {
  Exec(c, "INSERT INTO sub (adsh, cik, \"name\", fy) VALUES " + SubValues(s));
}

std::string TagList(const std::vector<NumM> &rows) {
  std::string out;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    out += (i ? "," : "") + Q(rows[i].tag);
  }
  return out;
}

struct Counters {
  uint64_t single_num = 0, bulk_num = 0, sub_ops = 0;
};

void Step(Fixture &f, Gen &gen, Counters &counters) {
  auto &c = f.connection;
  switch (gen.Int(0, 13)) {
  case 0:
  case 1: { // INSERT num (1-3 rows)
    std::vector<NumM> rows;
    for (int i = 0, n = gen.Int(1, 3); i < n; ++i) {
      rows.push_back(gen.RandomNum());
    }
    InsertNumSql(c, rows);
    std::vector<core::AuxRow> aux;
    for (const auto &r : rows) {
      f.num.push_back(r);
      aux.push_back(NumRowOf(r));
    }
    f.Send(kNumTable, core::MutationKind::Insert, {}, std::move(aux));
    ++counters.single_num;
    break;
  }
  case 2:
  case 3: { // DELETE num by tag
    if (f.num.empty()) {
      break;
    }
    std::vector<NumM> gone;
    for (int i = 0, n = gen.Int(1, 2); i < n && !f.num.empty(); ++i) {
      const auto idx = static_cast<std::size_t>(gen.Int(0, static_cast<int>(f.num.size()) - 1));
      gone.push_back(f.num[idx]);
      f.num.erase(f.num.begin() + idx);
    }
    Exec(c, "DELETE FROM num WHERE tag IN (" + TagList(gone) + ")");
    std::vector<core::AuxRow> aux;
    for (const auto &r : gone) {
      aux.push_back(NumRowOf(r));
    }
    f.Send(kNumTable, core::MutationKind::Delete, std::move(aux), {});
    ++counters.single_num;
    break;
  }
  case 4:
  case 5: { // UPDATE num.value
    if (f.num.empty()) {
      break;
    }
    const auto idx = static_cast<std::size_t>(gen.Int(0, static_cast<int>(f.num.size()) - 1));
    const auto old_row = NumRowOf(f.num[idx]);
    f.num[idx].value = gen.Value();
    f.num[idx].value_null = gen.Int(0, 10) == 0;
    Exec(c, "UPDATE num SET \"value\" = " +
                (f.num[idx].value_null ? std::string("NULL") : D(f.num[idx].value)) +
                " WHERE tag = " + Q(f.num[idx].tag));
    f.Send(kNumTable, core::MutationKind::Update, {old_row}, {NumRowOf(f.num[idx])}, {7});
    ++counters.single_num;
    break;
  }
  case 6: { // UPDATE num.uom and/or adsh (filter column + join key)
    if (f.num.empty()) {
      break;
    }
    const auto idx = static_cast<std::size_t>(gen.Int(0, static_cast<int>(f.num.size()) - 1));
    const auto old_row = NumRowOf(f.num[idx]);
    auto &n = f.num[idx];
    std::vector<core::ColumnId> cols;
    std::string set;
    if (gen.Int(0, 1)) {
      static const char *to[] = {"USD", "EUR", "shares", "CAD", "pure"};
      n.uom = to[gen.Int(0, 4)];
      n.uom_null = gen.Int(0, 15) == 0;
      set += std::string("uom = ") + (n.uom_null ? "NULL" : Q(n.uom));
      cols.push_back(5);
    }
    if (set.empty() || gen.Int(0, 1)) {
      n.adsh = gen.Pick(gen.adsh_pool);
      n.adsh_null = gen.Int(0, 20) == 0;
      set += std::string(set.empty() ? "" : ", ") + "adsh = " + (n.adsh_null ? "NULL" : Q(n.adsh));
      cols.push_back(0);
    }
    Exec(c, "UPDATE num SET " + set + " WHERE tag = " + Q(n.tag));
    if (gen.Int(0, 3) == 0) {
      cols.clear();
    }
    f.Send(kNumTable, core::MutationKind::Update, {old_row}, {NumRowOf(n)}, cols);
    ++counters.single_num;
    break;
  }
  case 7: { // UPDATE of a column Q3 does not read
    if (f.num.empty()) {
      break;
    }
    const auto idx = static_cast<std::size_t>(gen.Int(0, static_cast<int>(f.num.size()) - 1));
    const auto old_row = NumRowOf(f.num[idx]);
    Exec(c, "UPDATE num SET footnote = 'edited', qtrs = qtrs WHERE tag = " + Q(f.num[idx].tag));
    f.Send(kNumTable, core::MutationKind::Update, {old_row}, {old_row}, {8});
    break;
  }
  case 8: { // bulk insert
    std::vector<NumM> rows;
    for (int i = 0, n = gen.Int(20, 400); i < n; ++i) {
      auto r = gen.RandomNum();
      if (r.value != r.value || std::isinf(r.value)) {
        r.value = 1.5; // keep bulk inserts finite; the single-row path covers NaN/inf
      }
      rows.push_back(r);
    }
    InsertNumSql(c, rows);
    std::vector<core::AuxRow> aux;
    for (const auto &r : rows) {
      f.num.push_back(r);
      aux.push_back(NumRowOf(r));
    }
    f.Send(kNumTable, core::MutationKind::Insert, {}, std::move(aux));
    ++counters.bulk_num;
    break;
  }
  case 9: { // bulk delete + bulk update by predicate
    if (f.num.size() < 50) {
      break;
    }
    const std::string pred_uom = gen.Int(0, 1) ? "EUR" : "pure";
    std::vector<NumM> gone;
    std::vector<NumM> kept;
    for (const auto &r : f.num) {
      (!r.uom_null && r.uom == pred_uom && gen.Int(0, 1) ? gone : kept).push_back(r);
    }
    if (!gone.empty()) {
      Exec(c, "DELETE FROM num WHERE tag IN (" + TagList(gone) + ")");
      f.num = kept;
      std::vector<core::AuxRow> aux;
      for (const auto &r : gone) {
        aux.push_back(NumRowOf(r));
      }
      f.Send(kNumTable, core::MutationKind::Delete, std::move(aux), {});
      ++counters.bulk_num;
    }
    // scale every USD value of one filing
    const std::string adsh = gen.Pick(gen.adsh_pool);
    std::vector<core::AuxRow> before, after;
    std::string tags;
    for (auto &r : f.num) {
      if (!r.adsh_null && r.adsh == adsh && !r.value_null && std::isfinite(r.value)) {
        before.push_back(NumRowOf(r));
        r.value *= 1.25;
        after.push_back(NumRowOf(r));
        tags += (tags.empty() ? "" : ",") + Q(r.tag);
      }
    }
    if (!before.empty()) {
      // multiplication done in SQL; replicate in the model with the same IEEE op
      Exec(c, "UPDATE num SET \"value\" = \"value\" * 1.25 WHERE tag IN (" + tags + ")");
      f.Send(kNumTable, core::MutationKind::Update, std::move(before), std::move(after), {7});
      ++counters.bulk_num;
    }
    break;
  }
  case 10: { // sub insert
    std::vector<std::string> free;
    for (const auto &a : gen.adsh_pool) {
      if (!f.sub.count(a)) {
        free.push_back(a);
      }
    }
    if (free.empty()) {
      break;
    }
    auto s = gen.RandomSub(gen.Pick(free));
    InsertSubSql(c, s);
    f.sub[s.adsh] = s;
    f.Send(kSubTable, core::MutationKind::Insert, {}, {SubRowOf(s)});
    ++counters.sub_ops;
    break;
  }
  case 11: { // sub delete
    if (f.sub.empty()) {
      break;
    }
    auto it = f.sub.begin();
    std::advance(it, gen.Int(0, static_cast<int>(f.sub.size()) - 1));
    const auto old_row = SubRowOf(it->second);
    Exec(c, "DELETE FROM sub WHERE adsh = " + Q(it->first));
    f.sub.erase(it);
    f.Send(kSubTable, core::MutationKind::Delete, {old_row}, {});
    ++counters.sub_ops;
    break;
  }
  case 12: { // sub update fy / name / cik
    if (f.sub.empty()) {
      break;
    }
    auto it = f.sub.begin();
    std::advance(it, gen.Int(0, static_cast<int>(f.sub.size()) - 1));
    const auto old_row = SubRowOf(it->second);
    const auto fresh = gen.RandomSub(it->first);
    std::vector<core::ColumnId> cols;
    std::string set;
    auto add = [&](const std::string &s) { set += (set.empty() ? "" : ", ") + s; };
    if (gen.Int(0, 1)) {
      it->second.fy = fresh.fy;
      it->second.fy_null = fresh.fy_null;
      add(std::string("fy = ") + (fresh.fy_null ? "NULL" : std::to_string(fresh.fy)));
      cols.push_back(3);
    }
    if (gen.Int(0, 1)) {
      it->second.name = fresh.name;
      it->second.name_null = fresh.name_null;
      add(std::string("\"name\" = ") + (fresh.name_null ? "NULL" : Q(fresh.name)));
      cols.push_back(1);
    }
    if (set.empty() || gen.Int(0, 1)) {
      it->second.cik = fresh.cik;
      it->second.cik_null = fresh.cik_null;
      add(std::string("cik = ") + (fresh.cik_null ? "NULL" : std::to_string(fresh.cik)));
      cols.push_back(2);
    }
    Exec(c, "UPDATE sub SET " + set + " WHERE adsh = " + Q(it->first));
    f.Send(kSubTable, core::MutationKind::Update, {old_row}, {SubRowOf(it->second)}, cols);
    ++counters.sub_ops;
    break;
  }
  case 13: { // sub primary key update
    if (f.sub.empty()) {
      break;
    }
    std::vector<std::string> free;
    for (const auto &a : gen.adsh_pool) {
      if (!f.sub.count(a)) {
        free.push_back(a);
      }
    }
    if (free.empty()) {
      break;
    }
    auto it = f.sub.begin();
    std::advance(it, gen.Int(0, static_cast<int>(f.sub.size()) - 1));
    const auto old_row = SubRowOf(it->second);
    auto moved = it->second;
    moved.adsh = gen.Pick(free);
    Exec(c, "UPDATE sub SET adsh = " + Q(moved.adsh) + " WHERE adsh = " + Q(it->first));
    f.sub.erase(it);
    f.sub[moved.adsh] = moved;
    f.Send(kSubTable, core::MutationKind::Update, {old_row}, {SubRowOf(moved)}, {0});
    ++counters.sub_ops;
    break;
  }
  }
}

void RunSeed(uint32_t seed, int iterations) {
  duckdb::DuckDB database(nullptr);
  duckdb::Connection connection(database);
  accelerator_codegen::RegisterAcceleratorFunctions(connection);
  CreateSchema(connection);
  const auto &entry = catalog::LookupSharedQuery("sec_edgar", 3);
  Gen gen(seed);
  std::vector<NumM> initial;
  for (int i = 0; i < 400; ++i) {
    auto r = gen.RandomNum();
    if (!std::isfinite(r.value)) {
      r.value = 3.25;
    }
    initial.push_back(r);
  }
  InsertNumSql(connection, initial);
  Fixture f(connection, entry);
  f.num = initial;
  for (const auto &adsh : gen.adsh_pool) {
    if (gen.Int(0, 3) != 0) {
      auto s = gen.RandomSub(adsh);
      InsertSubSql(connection, s);
      f.sub[adsh] = s;
    }
  }
  f.runtime.Build();
  f.Verify("seed " + std::to_string(seed) + " initial", gen);
  Counters counters;
  for (int it = 0; it < iterations; ++it) {
    Step(f, gen, counters);
    f.Verify("seed " + std::to_string(seed) + " iter " + std::to_string(it), gen);
  }
  std::cerr << "seed " << seed << ": " << iterations << " steps (single num "
            << counters.single_num << ", bulk num " << counters.bulk_num << ", sub "
            << counters.sub_ops << "), " << f.verifications << " verifications (" << f.nonempty << " non-empty), " << f.param_verifications << " parameterized (" << f.param_nonempty << " non-empty), footprint "
            << f.runtime.AuxStateFootprintBytes() << " bytes\n";
}

// A few writes on a writable copy of the real database.
void RunRealDb(const std::string &path) {
  duckdb::DuckDB database(path.c_str());
  duckdb::Connection connection(database);
  accelerator_codegen::RegisterAcceleratorFunctions(connection);
  const auto &entry = catalog::LookupSharedQuery("sec_edgar", 3);
  Fixture f(connection, entry);
  f.runtime.Build();
  Gen gen(5);
  f.Verify("real db initial", gen);
  auto fetch = [&](const std::string &sql) {
    auto r = connection.Query(sql);
    Require(r && !r->HasError(), sql);
    return r;
  };
  // pick real USD fy2022 rows and existing sub rows to mutate
  auto rows = fetch(
      "SELECT n.adsh, n.tag, n.version, n.ddate, n.qtrs, n.uom, n.coreg, n.value, n.rowid "
      "FROM num n JOIN sub s ON n.adsh = s.adsh WHERE s.fy = 2022 AND n.uom = 'USD' "
      "AND n.value IS NOT NULL ORDER BY n.value DESC LIMIT 3");
  Require(rows->RowCount() == 3, "no candidate rows");
  auto row_of = [&](duckdb::idx_t r, double value) {
    core::AuxRow row;
    row.values = {Str(rows->GetValue(0, r).ToString()), Str(rows->GetValue(1, r).ToString()),
                  Str(rows->GetValue(2, r).ToString()),
                  Int(rows->GetValue(3, r).GetValue<int64_t>()),
                  Int(rows->GetValue(4, r).GetValue<int64_t>()),
                  Str(rows->GetValue(5, r).ToString()), Str(rows->GetValue(6, r).ToString()),
                  core::AuxValue::Of(core::AuxType::Double, value)};
    return row;
  };
  const double v0 = rows->GetValue(7, 0).GetValue<double>();
  const auto rowid0 = rows->GetValue(8, 0).ToString();
  // UPDATE value
  Exec(connection, "UPDATE num SET value = value * 3 WHERE rowid = " + rowid0);
  f.Send(kNumTable, core::MutationKind::Update, {row_of(0, v0)}, {row_of(0, v0 * 3)}, {7});
  f.Verify("real db update", gen);
  // DELETE
  Exec(connection, "DELETE FROM num WHERE rowid = " + rowid0);
  f.Send(kNumTable, core::MutationKind::Delete, {row_of(0, v0 * 3)}, {});
  f.Verify("real db delete", gen);
  // INSERT back
  Exec(connection, "INSERT INTO num (adsh, tag, \"version\", ddate, qtrs, uom, coreg, value) VALUES (" +
                       Q(rows->GetValue(0, 0).ToString()) + "," + Q(rows->GetValue(1, 0).ToString()) +
                       "," + Q(rows->GetValue(2, 0).ToString()) + "," +
                       rows->GetValue(3, 0).ToString() + "," + rows->GetValue(4, 0).ToString() +
                       ",'USD'," + Q(rows->GetValue(6, 0).ToString()) + "," + D(v0) + ")");
  f.Send(kNumTable, core::MutationKind::Insert, {}, {row_of(0, v0)});
  f.Verify("real db insert", gen);
  // sub: move a top filing out of fy 2022 and back
  const auto adsh = rows->GetValue(0, 1).ToString();
  auto sub = fetch("SELECT adsh, name, cik, fy FROM sub WHERE adsh = " + Q(adsh));
  auto sub_row = [&](int fy) {
    core::AuxRow row;
    row.values = {Str(sub->GetValue(0, 0).ToString()), Str(sub->GetValue(1, 0).ToString()),
                  Int(sub->GetValue(2, 0).GetValue<int64_t>()), Int(fy)};
    return row;
  };
  Exec(connection, "UPDATE sub SET fy = 2021 WHERE adsh = " + Q(adsh));
  f.Send(kSubTable, core::MutationKind::Update, {sub_row(2022)}, {sub_row(2021)}, {3});
  f.Verify("real db sub fy out", gen);
  Exec(connection, "UPDATE sub SET fy = 2022 WHERE adsh = " + Q(adsh));
  f.Send(kSubTable, core::MutationKind::Update, {sub_row(2021)}, {sub_row(2022)}, {3});
  f.Verify("real db sub fy in", gen);
  std::cerr << "real db: OK, footprint " << f.runtime.AuxStateFootprintBytes() << " bytes\n";
}

} // namespace

int main(int argc, char **argv) {
  try {
    const int iterations = argc > 1 ? std::atoi(argv[1]) : 400;
    const int seeds = argc > 2 ? std::atoi(argv[2]) : 4;
    for (int s = 1; s <= seeds; ++s) {
      RunSeed(static_cast<uint32_t>(s), iterations);
    }
    if (argc > 3) {
      RunRealDb(argv[3]);
    }
    std::cout << "test_sec_edgar_q3_differential OK" << std::endl;
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "test_sec_edgar_q3_differential FAILED: " << e.what() << std::endl;
    return 1;
  }
}
