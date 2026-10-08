#pragma once
// Randomized transaction generator for the SEC-EDGAR Q4 aux tests.
//
// Maintains a small in-memory model of num/sub/tag/pre (physical column
// layout) and emits, for every mutation, both the aux MutationBatch and the
// equivalent SQL.  The mock-engine test evaluates a naive oracle on the model;
// the DuckDB test executes the SQL and compares against q4.sql itself.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_types.hpp"

namespace q4gen {

using accelerator_codegen::aux_state::core::AuxRow;
using accelerator_codegen::aux_state::core::AuxType;
using accelerator_codegen::aux_state::core::AuxValue;
using accelerator_codegen::aux_state::core::MutationBatch;
using accelerator_codegen::aux_state::core::MutationKind;

inline constexpr uint64_t kNum = 1, kSub = 2, kTag = 3, kPre = 4;

struct NumRow {
  std::optional<std::string> adsh, tag, version, uom;
  std::optional<double> value;
  int ddate = 0;
  int64_t rid = 0;
};
struct SubRow {
  std::string adsh;
  std::optional<int32_t> cik, sic;
  std::string name;
};
struct TagRow {
  std::string tag, version;
  std::optional<int32_t> abstract;
  std::optional<std::string> tlabel;
};
struct PreRow {
  std::optional<std::string> adsh, tag, version, stmt;
  int line = 0;
  int64_t rid = 0;
};

struct Db {
  std::vector<NumRow> num;
  std::vector<SubRow> sub;
  std::vector<TagRow> tag;
  std::vector<PreRow> pre;
};

// ------------------------------------------------------------ conversions
inline AuxValue S(const std::optional<std::string> &v) {
  return v ? AuxValue::Of(AuxType::String, *v) : AuxValue::Null(AuxType::String);
}
inline AuxValue I(const std::optional<int32_t> &v) {
  return v ? AuxValue::Of(AuxType::Int64, static_cast<int64_t>(*v)) : AuxValue::Null(AuxType::Int64);
}
inline AuxValue D(const std::optional<double> &v) {
  return v ? AuxValue::Of(AuxType::Double, *v) : AuxValue::Null(AuxType::Double);
}
inline AuxRow ToRow(const NumRow &r) {
  return {{S(r.adsh), S(r.tag), S(r.version), I(r.ddate), I(4), S(r.uom), S(std::string("")), D(r.value)}};
}
inline AuxRow ToRow(const SubRow &r) { return {{S(r.adsh), I(r.cik), S(r.name), I(r.sic)}}; }
inline AuxRow ToRow(const TagRow &r) {
  return {{S(r.tag), S(r.version), I(0), I(r.abstract), S(std::string("d")), S(std::string("i")),
           S(std::string("c")), S(r.tlabel)}};
}
inline AuxRow ToRow(const PreRow &r) {
  return {{S(r.adsh), I(1), I(r.line), S(r.stmt), I(0), S(std::string("R")), S(r.tag), S(r.version)}};
}

// -------------------------------------------------------------- SQL text
inline std::string Q(const std::string &s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') out.push_back('\'');
    out.push_back(c);
  }
  return out + "'";
}
inline std::string Q(const std::optional<std::string> &s) { return s ? Q(*s) : "NULL"; }
inline std::string Q(const std::optional<int32_t> &v) { return v ? std::to_string(*v) : "NULL"; }
inline std::string Q(const std::optional<double> &v) {
  if (!v) return "NULL";
  if (std::isnan(*v)) return "CAST('NaN' AS DOUBLE)";
  if (std::isinf(*v)) return *v > 0 ? "CAST('Infinity' AS DOUBLE)" : "CAST('-Infinity' AS DOUBLE)";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.17g", *v);
  std::string s = buf;
  if (s.find_first_of(".eE") == std::string::npos) s += ".0";
  return "CAST(" + s + " AS DOUBLE)";
}

inline const char *kDdl[] = {
    "CREATE TABLE num (adsh VARCHAR, tag VARCHAR, version VARCHAR, ddate INTEGER, qtrs INTEGER, "
    "uom VARCHAR, coreg VARCHAR, value DOUBLE, footnote VARCHAR, rid BIGINT)",
    "CREATE TABLE sub (adsh VARCHAR NOT NULL PRIMARY KEY, cik INTEGER, name VARCHAR, sic INTEGER, "
    "countryba VARCHAR)",
    "CREATE TABLE tag (tag VARCHAR NOT NULL, version VARCHAR NOT NULL, custom INTEGER, "
    "abstract INTEGER, datatype VARCHAR, iord VARCHAR, crdr VARCHAR, tlabel VARCHAR, doc VARCHAR, "
    "PRIMARY KEY (tag, version))",
    "CREATE TABLE pre (adsh VARCHAR, report INTEGER, line INTEGER, stmt VARCHAR, inpth INTEGER, "
    "rfile VARCHAR, tag VARCHAR, version VARCHAR, plabel VARCHAR, negating INTEGER, rid BIGINT)"};

inline std::string InsertSql(const NumRow &r) {
  return "INSERT INTO num VALUES (" + Q(r.adsh) + "," + Q(r.tag) + "," + Q(r.version) + "," +
         std::to_string(r.ddate) + ",4," + Q(r.uom) + ",''," + Q(r.value) + ",NULL," +
         std::to_string(r.rid) + ")";
}
inline std::string InsertSql(const SubRow &r) {
  return "INSERT INTO sub VALUES (" + Q(r.adsh) + "," + Q(r.cik) + "," + Q(r.name) + "," + Q(r.sic) +
         ",'US')";
}
inline std::string InsertSql(const TagRow &r) {
  return "INSERT INTO tag VALUES (" + Q(r.tag) + "," + Q(r.version) + ",0," + Q(r.abstract) +
         ",'d','i','c'," + Q(r.tlabel) + ",NULL)";
}
inline std::string InsertSql(const PreRow &r) {
  return "INSERT INTO pre VALUES (" + Q(r.adsh) + ",1," + std::to_string(r.line) + "," + Q(r.stmt) +
         ",0,'R'," + Q(r.tag) + "," + Q(r.version) + ",NULL,0," + std::to_string(r.rid) + ")";
}

// ----------------------------------------------------------------- oracle
struct OutRow {
  int32_t sic;
  std::optional<std::string> tlabel;
  int64_t companies;
  long double total;
  long double avg;
};

inline std::vector<OutRow> Oracle(const Db &db) {
  std::map<std::string, const SubRow *> subs;
  for (const auto &s : db.sub) subs[s.adsh] = &s;
  std::map<std::pair<std::string, std::string>, const TagRow *> tags;
  for (const auto &t : db.tag) tags[{t.tag, t.version}] = &t;
  std::map<std::tuple<std::string, std::string, std::string>, int> pre_eq;
  for (const auto &p : db.pre) {
    if (p.adsh && p.tag && p.version && p.stmt && *p.stmt == "EQ") ++pre_eq[{*p.adsh, *p.tag, *p.version}];
  }
  struct Acc {
    __float128 sum = 0; // quad precision: exact enough for 1e12-scale cancellation
    int64_t count = 0;
    std::set<int32_t> ciks;
  };
  std::map<std::pair<int32_t, std::optional<std::string>>, Acc> groups;
  for (const auto &n : db.num) {
    if (!n.adsh || !n.tag || !n.version || !n.uom || !n.value || *n.uom != "USD") continue;
    const auto sit = subs.find(*n.adsh);
    if (sit == subs.end()) continue;
    const SubRow &s = *sit->second;
    if (!s.sic || *s.sic < 4000 || *s.sic > 4999) continue;
    const auto tit = tags.find({*n.tag, *n.version});
    if (tit == tags.end()) continue;
    const TagRow &t = *tit->second;
    if (!t.abstract || *t.abstract != 0) continue;
    const auto pit = pre_eq.find({*n.adsh, *n.tag, *n.version});
    if (pit == pre_eq.end()) continue;
    auto &acc = groups[{*s.sic, t.tlabel}];
    acc.sum += static_cast<__float128>(*n.value) * pit->second;
    acc.count += pit->second;
    if (s.cik) acc.ciks.insert(*s.cik);
  }
  std::vector<OutRow> out;
  for (const auto &[key, acc] : groups) {
    if (acc.ciks.size() < 2) continue;
    out.push_back({key.first, key.second, static_cast<int64_t>(acc.ciks.size()),
                   static_cast<long double>(acc.sum), static_cast<long double>(acc.sum / acc.count)});
  }
  std::sort(out.begin(), out.end(), [](const OutRow &a, const OutRow &b) {
    const bool an = std::isnan(a.total), bn = std::isnan(b.total);
    if (an != bn) return an; // NaN sorts as the largest value (DuckDB)
    if (!an && a.total != b.total) return a.total > b.total;
    if (a.sic != b.sic) return a.sic < b.sic;
    return a.tlabel < b.tlabel; // NULL first (matches the module)
  });
  if (out.size() > 500) out.resize(500);
  return out;
}

// ------------------------------------------------------------- generator
struct Config {
  enum class Flavor { Narrow, Wide, Limit } flavor = Flavor::Narrow;
  bool big_values = true;      // include 1e12-scale values (mock test only)
  bool unique_sums = false;    // fine-grained values so group totals never tie
  bool wide_keys = false;      // cik values spread over the whole int32 range (negative, huge)
  bool nonfinite = false;      // +-inf / NaN / 1e300-scale values in num.value
  int adsh = 70, tags = 30, versions = 2, tlabels = 6, ciks = 12, num_rows = 4000, pre_rows = 900;
  std::vector<int> sics;
};

struct Mutation {
  MutationBatch batch;
  std::vector<std::string> sql;
};

struct Model {
  Db db;
  Config cfg;
  std::mt19937_64 rng;
  int64_t next_rid = 1;
  std::optional<std::string> hint_adsh, hint_tag, hint_version;

  explicit Model(uint64_t seed, Config c) : cfg(std::move(c)), rng(seed) {}

  int Int(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); }
  bool Coin(double p) { return std::uniform_real_distribution<double>(0, 1)(rng) < p; }
  // Distinct n give distinct ciks; with wide_keys they cover the int32 range.
  int32_t Cik(int n) const {
    if (!cfg.wide_keys) return 100 + n;
    return static_cast<int32_t>(static_cast<uint32_t>(n + 1) * 0x9E3779B1u);
  }
  double Value() {
    if (cfg.nonfinite && Coin(0.03)) {
      switch (Int(0, 5)) {
      case 0: return std::numeric_limits<double>::infinity();
      case 1: return -std::numeric_limits<double>::infinity();
      case 2: return std::numeric_limits<double>::quiet_NaN();
      case 3: return 1e300 * Int(1, 9);
      case 4: return -1e300 * Int(1, 9);
      default: return 5e-324 * Int(1, 99); // subnormal
      }
    }
    if (cfg.unique_sums) {
      return static_cast<double>(Int(-2000000000, 2000000000)) * 1e-6;
    }
    switch (Int(0, cfg.big_values ? 5 : 3)) {
    case 0: return Int(-2000, 2000) * 0.25;
    case 1: return Int(1, 100000) * 0.5;
    case 2: return Int(1, 1000) * 0.1;
    case 3: return Int(1, 400) * 0.25;
    case 4: return Int(-5, 5) * 1e12 + Int(0, 99) * 0.25;
    default: return 0.0;
    }
  }
  std::string Adsh(int n) const { return "0001-" + std::to_string(n); }
  std::string Tag(int n) const { return "Tag" + std::to_string(n); }
  std::string Version(int n) const { return n == 0 ? "us-gaap/2020" : "us-gaap/2021"; }
  int PickSic() { return cfg.sics[static_cast<size_t>(Int(0, static_cast<int>(cfg.sics.size()) - 1))]; }

  PreRow RandomPre() {
    PreRow p;
    p.adsh = Adsh(Int(0, cfg.adsh - 1));
    p.tag = Tag(Int(0, cfg.tags - 1));
    p.version = Version(Int(0, cfg.versions - 1));
    const int s = Int(0, 9);
    p.stmt = s < 6 ? std::optional<std::string>("EQ")
             : s < 9 ? std::optional<std::string>("BS")
                     : std::nullopt;
    p.line = Int(0, 50);
    p.rid = next_rid++;
    return p;
  }
  NumRow RandomNum() {
    NumRow n;
    n.adsh = Coin(0.01) ? std::nullopt : std::optional<std::string>(Adsh(Int(0, cfg.adsh - 1)));
    n.tag = Tag(Int(0, cfg.tags - 1));
    n.version = Version(Int(0, cfg.versions - 1));
    if (hint_adsh && Coin(0.7)) n.adsh = hint_adsh;
    if (hint_tag && Coin(0.7)) n.tag = hint_tag;
    if (hint_version && Coin(0.7)) n.version = hint_version;
    n.uom = Coin(0.2) ? std::optional<std::string>(Coin(0.5) ? "EUR" : "usd") : std::optional<std::string>("USD");
    n.value = Coin(0.04) ? std::nullopt : std::optional<double>(Value());
    n.ddate = Int(2019, 2023);
    n.rid = next_rid++;
    return n;
  }
  TagRow RandomTag(int max_tag) {
    TagRow row;
    row.tag = Tag(Int(0, max_tag));
    row.version = Version(Int(0, 1));
    row.abstract = Coin(0.07) ? std::nullopt : std::optional<int32_t>(Coin(0.15) ? 1 : 0);
    row.tlabel = Coin(0.07) ? std::nullopt
                            : std::optional<std::string>("L" + std::to_string(Int(0, cfg.tlabels + 2)));
    return row;
  }

  // Builds the initial database; returns the SQL that creates and fills it.
  std::vector<std::string> Init() {
    const bool limit = cfg.flavor == Config::Flavor::Limit;
    for (int a = 0; a < cfg.adsh; ++a) {
      SubRow s;
      s.adsh = Adsh(a);
      s.cik = Coin(0.04) ? std::nullopt : std::optional<int32_t>(Cik(Int(0, cfg.ciks - 1)));
      s.sic = Coin(0.04) ? std::nullopt : std::optional<int32_t>(PickSic());
      s.name = "n";
      if (limit) {
        s.cik = Cik(1000 + a);
        s.sic = 4000 + a / 2;
      }
      db.sub.push_back(s);
    }
    for (int t = 0; t < cfg.tags; ++t) {
      for (int v = 0; v < cfg.versions; ++v) {
        TagRow row = RandomTag(0);
        row.tag = Tag(t);
        row.version = Version(v);
        if (limit) {
          row.abstract = 0;
          row.tlabel = std::string("L0");
        }
        db.tag.push_back(row);
      }
    }
    for (int i = 0; i < cfg.pre_rows; ++i) db.pre.push_back(RandomPre());
    if (limit) {
      for (int a = 0; a < cfg.adsh; ++a) {
        for (int t = 0; t < cfg.tags; ++t) {
          PreRow p;
          p.adsh = Adsh(a);
          p.tag = Tag(t);
          p.version = Version(0);
          p.stmt = std::string("EQ");
          p.line = 1;
          p.rid = next_rid++;
          db.pre.push_back(p);
        }
      }
    }
    if (limit) {
      for (int a = 0; a < cfg.adsh; ++a) {
        for (int t = 0; t < cfg.tags; ++t) {
          NumRow n;
          n.adsh = Adsh(a);
          n.tag = Tag(t);
          n.version = Version(0);
          n.uom = std::string("USD");
          n.value = Value() + 10000.0 * (a % 97);
          n.ddate = 2020;
          n.rid = next_rid++;
          db.num.push_back(n);
        }
      }
    } else {
      for (int i = 0; i < cfg.num_rows; ++i) db.num.push_back(RandomNum());
    }
    std::vector<std::string> sql(std::begin(kDdl), std::end(kDdl));
    auto bulk = [&](auto &rows, const char *table) {
      std::string stmt;
      size_t n = 0;
      for (const auto &r : rows) {
        const std::string one = InsertSql(r);
        const auto vals = one.substr(one.find(" VALUES ") + 8);
        if (n % 200 == 0) {
          if (!stmt.empty()) sql.push_back(stmt);
          stmt = std::string("INSERT INTO ") + table + " VALUES " + vals;
        } else {
          stmt += "," + vals;
        }
        ++n;
      }
      if (!stmt.empty()) sql.push_back(stmt);
    };
    bulk(db.sub, "sub");
    bulk(db.tag, "tag");
    bulk(db.pre, "pre");
    bulk(db.num, "num");
    return sql;
  }

  // One transaction of 1..4 mutations.  Mutates `db`.
  std::vector<Mutation> NextTxn() {
    const bool limit = cfg.flavor == Config::Flavor::Limit;
    hint_adsh.reset();
    hint_tag.reset();
    hint_version.reset();
    std::vector<Mutation> out;
    const int mutations = Coin(0.55) ? 1 : Int(2, 4);
    for (int m = 0; m < mutations; ++m) {
      Mutation mu;
      int table = Int(0, 99);
      int op = Int(0, 2);
      if (limit && Coin(0.75)) { // churn values near the LIMIT cut
        table = 0;
        op = 2;
      }
      if (!Gen(table, op, mu)) continue;
      if (mu.batch.table == kPre && !mu.batch.new_rows.empty() && !mu.batch.new_rows[0].values[0].is_null) {
        hint_adsh = std::get<std::string>(mu.batch.new_rows[0].values[0].payload);
        if (!mu.batch.new_rows[0].values[6].is_null)
          hint_tag = std::get<std::string>(mu.batch.new_rows[0].values[6].payload);
        if (!mu.batch.new_rows[0].values[7].is_null)
          hint_version = std::get<std::string>(mu.batch.new_rows[0].values[7].payload);
      } else if (mu.batch.table == kSub && !mu.batch.new_rows.empty()) {
        hint_adsh = std::get<std::string>(mu.batch.new_rows[0].values[0].payload);
      } else if (mu.batch.table == kTag && !mu.batch.new_rows.empty()) {
        hint_tag = std::get<std::string>(mu.batch.new_rows[0].values[0].payload);
        hint_version = std::get<std::string>(mu.batch.new_rows[0].values[1].payload);
      }
      out.push_back(std::move(mu));
    }
    return out;
  }

  const char *last_what = "";

  bool Gen(int table, int op, Mutation &mu) {
    auto &b = mu.batch;
    if (table < 55) { // ------------------------------------------------ num
      b.table = kNum;
      last_what = "num";
      if (op == 0 || db.num.empty()) {
        b.kind = MutationKind::Insert;
        const int n = Coin(0.8) ? 1 : Int(2, 40);
        for (int i = 0; i < n; ++i) {
          db.num.push_back(RandomNum());
          b.new_rows.push_back(ToRow(db.num.back()));
          mu.sql.push_back(InsertSql(db.num.back()));
        }
      } else if (op == 1) {
        b.kind = MutationKind::Delete;
        const int n = std::min<int>(Coin(0.8) ? 1 : Int(2, 30), static_cast<int>(db.num.size()));
        for (int i = 0; i < n; ++i) {
          const size_t idx = static_cast<size_t>(Int(0, static_cast<int>(db.num.size()) - 1));
          b.old_rows.push_back(ToRow(db.num[idx]));
          mu.sql.push_back("DELETE FROM num WHERE rid = " + std::to_string(db.num[idx].rid));
          db.num.erase(db.num.begin() + static_cast<long>(idx));
        }
      } else {
        b.kind = MutationKind::Update;
        const int n = Coin(0.8) ? 1 : Int(2, 20);
        const int col = Int(0, 9);
        std::set<int64_t> used;
        for (int i = 0; i < n; ++i) {
          auto &row = db.num[static_cast<size_t>(Int(0, static_cast<int>(db.num.size()) - 1))];
          if (!used.insert(row.rid).second) continue; // one image per row and statement
          b.old_rows.push_back(ToRow(row));
          std::string set;
          if (col < 5) {
            row.value = Coin(0.1) ? std::nullopt : std::optional<double>(Value());
            b.updated_columns = {7};
            set = "value = " + Q(row.value);
          } else if (col == 5) {
            row.uom = Coin(0.5) ? "USD" : "EUR";
            b.updated_columns = {5};
            set = "uom = " + Q(row.uom);
          } else if (col == 6) {
            row.tag = Tag(Int(0, cfg.tags - 1));
            b.updated_columns = {1};
            set = "tag = " + Q(row.tag);
          } else if (col == 7) {
            row.adsh = Adsh(Int(0, cfg.adsh - 1));
            b.updated_columns = {0};
            set = "adsh = " + Q(row.adsh);
          } else if (col == 8) {
            row.version = Version(Int(0, 1));
            b.updated_columns = {2};
            set = "version = " + Q(row.version);
          } else {
            row.ddate = Int(2000, 2030); // irrelevant column
            b.updated_columns = {3};
            set = "ddate = " + std::to_string(row.ddate);
          }
          b.new_rows.push_back(ToRow(row));
          mu.sql.push_back("UPDATE num SET " + set + " WHERE rid = " + std::to_string(row.rid));
        }
      }
      return true;
    }
    if (table < 70) { // ------------------------------------------------ pre
      b.table = kPre;
      last_what = "pre";
      if (op == 0 || db.pre.empty()) {
        b.kind = MutationKind::Insert;
        db.pre.push_back(RandomPre());
        b.new_rows.push_back(ToRow(db.pre.back()));
        mu.sql.push_back(InsertSql(db.pre.back()));
      } else if (op == 1) {
        b.kind = MutationKind::Delete;
        const size_t idx = static_cast<size_t>(Int(0, static_cast<int>(db.pre.size()) - 1));
        b.old_rows.push_back(ToRow(db.pre[idx]));
        mu.sql.push_back("DELETE FROM pre WHERE rid = " + std::to_string(db.pre[idx].rid));
        db.pre.erase(db.pre.begin() + static_cast<long>(idx));
      } else {
        b.kind = MutationKind::Update;
        auto &row = db.pre[static_cast<size_t>(Int(0, static_cast<int>(db.pre.size()) - 1))];
        b.old_rows.push_back(ToRow(row));
        std::string set;
        switch (Int(0, 4)) {
        case 0:
          row.stmt = Coin(0.6) ? "EQ" : "IS";
          b.updated_columns = {3};
          set = "stmt = " + Q(row.stmt);
          break;
        case 1:
          row.tag = Tag(Int(0, cfg.tags - 1));
          b.updated_columns = {6};
          set = "tag = " + Q(row.tag);
          break;
        case 2:
          row.adsh = Adsh(Int(0, cfg.adsh - 1));
          b.updated_columns = {0};
          set = "adsh = " + Q(row.adsh);
          break;
        case 3:
          row.version = Version(Int(0, 1));
          b.updated_columns = {7};
          set = "version = " + Q(row.version);
          break;
        default:
          row.line = Int(0, 99); // irrelevant column
          b.updated_columns = {2};
          set = "line = " + std::to_string(row.line);
        }
        b.new_rows.push_back(ToRow(row));
        mu.sql.push_back("UPDATE pre SET " + set + " WHERE rid = " + std::to_string(row.rid));
      }
      return true;
    }
    if (table < 85) { // ------------------------------------------------ sub
      b.table = kSub;
      last_what = "sub";
      const auto exists = [&](const std::string &a) {
        return std::any_of(db.sub.begin(), db.sub.end(), [&](const SubRow &r) { return r.adsh == a; });
      };
      if (op == 0 || db.sub.size() < 5) {
        b.kind = MutationKind::Insert;
        SubRow s;
        s.adsh = Adsh(Int(0, cfg.adsh + 15));
        if (exists(s.adsh)) return false;
        s.cik = Coin(0.05) ? std::nullopt : std::optional<int32_t>(Cik(Int(0, cfg.ciks + 2)));
        s.sic = Coin(0.05) ? std::nullopt : std::optional<int32_t>(PickSic());
        s.name = "n";
        db.sub.push_back(s);
        b.new_rows.push_back(ToRow(s));
        mu.sql.push_back(InsertSql(s));
      } else if (op == 1) {
        b.kind = MutationKind::Delete;
        const size_t idx = static_cast<size_t>(Int(0, static_cast<int>(db.sub.size()) - 1));
        b.old_rows.push_back(ToRow(db.sub[idx]));
        mu.sql.push_back("DELETE FROM sub WHERE adsh = " + Q(db.sub[idx].adsh));
        db.sub.erase(db.sub.begin() + static_cast<long>(idx));
      } else {
        b.kind = MutationKind::Update;
        const size_t idx = static_cast<size_t>(Int(0, static_cast<int>(db.sub.size()) - 1));
        SubRow &row = db.sub[idx];
        const std::string old_adsh = row.adsh;
        SubRow before = row;
        std::string set;
        switch (Int(0, 3)) {
        case 0:
          row.sic = Coin(0.1) ? std::nullopt : std::optional<int32_t>(PickSic());
          b.updated_columns = {3};
          set = "sic = " + Q(row.sic);
          break;
        case 1:
          row.cik = Coin(0.1) ? std::nullopt : std::optional<int32_t>(Cik(Int(0, cfg.ciks + 2)));
          b.updated_columns = {1};
          set = "cik = " + Q(row.cik);
          break;
        case 2:
          row.name = "m" + std::to_string(Int(0, 9)); // irrelevant column
          b.updated_columns = {2};
          set = "name = " + Q(row.name);
          break;
        default: {
          const std::string next = Adsh(Int(cfg.adsh + 20, cfg.adsh + 40));
          if (exists(next)) return false;
          row.adsh = next;
          b.updated_columns = {0};
          set = "adsh = " + Q(row.adsh);
        }
        }
        b.old_rows.push_back(ToRow(before));
        b.new_rows.push_back(ToRow(row));
        mu.sql.push_back("UPDATE sub SET " + set + " WHERE adsh = " + Q(old_adsh));
      }
      return true;
    }
    // ---------------------------------------------------------------- tag
    b.table = kTag;
    last_what = "tag";
    const auto exists = [&](const std::string &t, const std::string &v) {
      return std::any_of(db.tag.begin(), db.tag.end(),
                         [&](const TagRow &r) { return r.tag == t && r.version == v; });
    };
    if (op == 0 || db.tag.size() < 5) {
      b.kind = MutationKind::Insert;
      TagRow row = RandomTag(cfg.tags + 5);
      if (exists(row.tag, row.version)) return false;
      db.tag.push_back(row);
      b.new_rows.push_back(ToRow(row));
      mu.sql.push_back(InsertSql(row));
    } else if (op == 1) {
      b.kind = MutationKind::Delete;
      const size_t idx = static_cast<size_t>(Int(0, static_cast<int>(db.tag.size()) - 1));
      b.old_rows.push_back(ToRow(db.tag[idx]));
      mu.sql.push_back("DELETE FROM tag WHERE tag = " + Q(db.tag[idx].tag) +
                       " AND version = " + Q(db.tag[idx].version));
      db.tag.erase(db.tag.begin() + static_cast<long>(idx));
    } else {
      b.kind = MutationKind::Update;
      auto &row = db.tag[static_cast<size_t>(Int(0, static_cast<int>(db.tag.size()) - 1))];
      b.old_rows.push_back(ToRow(row));
      std::string set;
      if (Coin(0.5)) {
        row.abstract = Coin(0.1) ? std::nullopt : std::optional<int32_t>(Coin(0.5) ? 1 : 0);
        b.updated_columns = {3};
        set = "abstract = " + Q(row.abstract);
      } else {
        row.tlabel = Coin(0.1) ? std::nullopt
                               : std::optional<std::string>("L" + std::to_string(Int(0, cfg.tlabels + 2)));
        b.updated_columns = {7};
        set = "tlabel = " + Q(row.tlabel);
      }
      b.new_rows.push_back(ToRow(row));
      mu.sql.push_back("UPDATE tag SET " + set + " WHERE tag = " + Q(row.tag) + " AND version = " + Q(row.version));
    }
    return true;
  }
};

inline Config MakeConfig(int index, bool limit_case) {
  Config c;
  const bool wide = !limit_case && index % 2 == 0;
  c.flavor = limit_case ? Config::Flavor::Limit : wide ? Config::Flavor::Wide : Config::Flavor::Narrow;
  c.adsh = limit_case ? 1400 : wide ? 400 : 70;
  c.tags = limit_case ? 2 : wide ? 60 : 30;
  c.tlabels = limit_case ? 2 : wide ? 14 : 6;
  c.ciks = limit_case ? 1400 : wide ? 60 : 12;
  c.num_rows = limit_case ? 2800 : wide ? 14000 : 4000;
  c.pre_rows = limit_case ? 0 : wide ? 6000 : 900;
  if (limit_case) {
    for (int i = 0; i < 700; ++i) c.sics.push_back(4000 + i);
  } else if (wide) {
    for (int i = 0; i < 120; ++i) c.sics.push_back(3990 + i * 9); // spans the range edges
    c.sics.insert(c.sics.end(), {4000, 4999, 5000, 3999});
  } else {
    c.sics = {3999, 4000, 4001, 4100, 4999, 5000, 4500};
  }
  return c;
}

} // namespace q4gen
