// Randomized differential test of the Verus Q21 accelerator on DuckDB.
//
//   q21_verus_duckdb_fuzz <db-copy> <txns> <seed> <synthetic|sf1> <policy|ApplyDelta>
//
// synthetic: replaces the four Q21 tables of <db-copy> (a copy of any TPC-H
//   DuckDB file) with small adversarial data: duplicate orders / supplier /
//   nation keys (DuckDB declares no keys), lineitems of missing orders and of
//   an unknown supplier, suppliers and nations sharing names, and edge-case
//   names ('', non-ASCII, quote, '|').
// sf1: writes on the existing data of <db-copy>.
//
// Each transaction (1-4 operations on distinct rows, sometimes a 20-60 row
// batch) is applied through DuckdbRuntime::ApplyBatch with the exact captured
// row deltas: every changed row is addressed by rowid and read just before
// the transaction.  Operations cover every column Q21 reads on all four
// tables (inserts, deletes, updates, including key changes and updates of
// several columns at once, l_commitdate included).  After each
// transaction the accelerator is compared with the SQL (ordered rows) for two
// random parameters (all parameters every 25 transactions), including '' ,
// a non-matching name, and NULL.  The staged maintenance action of every
// transaction is counted; with ApplyDelta forced, anything else means the
// kernel declined.
#include "accelerator_codegen/runtime/duckdb/duckdb_runtime.hpp"
#include "accelerator_codegen/runtime/duckdb/register.hpp"
#include "accelerator_codegen/stateful/core/aux_maintenance_decision_log.hpp"
#include "accelerator_codegen/stateful/core/aux_maintenance_policy.hpp"
#include "catalog/query_catalog.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace {
namespace catalog = accelerator_codegen::aux_state::engine;
namespace core = accelerator_codegen::aux_state::core;
using catalog::duckdb::DuckdbRuntime;

void Require(bool ok, const std::string &msg) {
  if (!ok)
    throw std::runtime_error(msg);
}

std::unique_ptr<duckdb::MaterializedQueryResult> Q(duckdb::Connection &c, const std::string &sql) {
  auto r = c.Query(sql);
  Require(r && !r->HasError(), (r ? r->GetError() : "query failed") + ": " + sql);
  return r;
}

std::string Lit(const std::string &s) {
  std::string out = "'";
  for (char ch : s)
    out += ch == '\'' ? std::string("''") : std::string(1, ch);
  return out + "'";
}

std::string Date(int32_t d) { return "(DATE '1970-01-01' + " + std::to_string(d) + ")"; }

int32_t Days(const duckdb::Value &v) { return duckdb::Date::EpochDays(v.GetValue<duckdb::date_t>()); }

core::AuxValue I(int64_t x) { return core::AuxValue::Of(core::AuxType::Int64, x); }
core::AuxValue D(int32_t d) { return core::AuxValue::Of(core::AuxType::Date, core::AuxDate{d}); }
core::AuxValue S(const std::string &s) { return core::AuxValue::Of(core::AuxType::String, s); }

enum Table : core::TableId { kSupplier = 1, kOrders = 2, kLineitem = 3, kNation = 4 };

const std::vector<std::optional<std::string>> kParams = {
    std::string("SAUDI ARABIA"), std::string("PERU"), std::string("KENYA"), std::string(""),
    std::string("\xC3\x89GYPTE"), std::string("O'NEIL"), std::string("A|B"),
    std::string("ATLANTIS"), std::nullopt};
const std::vector<std::string> kNationNames = {"SAUDI ARABIA", "PERU", "KENYA", "",
                                               "\xC3\x89GYPTE", "O'NEIL", "A|B"};
const std::vector<std::string> kSupplierNames = {"Supp#01", "Supp#02", "Supp#03", "supp#01",
                                                 "\xC3\x84supp", "Supp#01 ", "S'q"};

std::string BaseSql(const std::optional<std::string> &p) {
  return "SELECT s_name, COUNT(*) AS numwait FROM supplier, lineitem l1, orders, nation "
         "WHERE s_suppkey = l1.l_suppkey AND o_orderkey = l1.l_orderkey AND "
         "o_orderstatus = 'F' AND l1.l_receiptdate > l1.l_commitdate AND EXISTS ("
         "SELECT * FROM lineitem l2 WHERE l2.l_orderkey = l1.l_orderkey AND "
         "l2.l_suppkey <> l1.l_suppkey) AND NOT EXISTS (SELECT * FROM lineitem l3 WHERE "
         "l3.l_orderkey = l1.l_orderkey AND l3.l_suppkey <> l1.l_suppkey AND "
         "l3.l_receiptdate > l3.l_commitdate) AND s_nationkey = n_nationkey AND n_name = " +
         (p ? Lit(*p) : std::string("NULL")) + " GROUP BY s_name ORDER BY numwait DESC, s_name LIMIT 100";
}
std::string AccelSql(const std::optional<std::string> &p) {
  return "SELECT * FROM accel_tpch_q21_verus(" + (p ? Lit(*p) : std::string("NULL")) + ")";
}

using Rows = std::vector<std::pair<std::string, int64_t>>;
Rows Fetch(duckdb::Connection &c, const std::string &sql) {
  auto r = Q(c, sql);
  Rows rows;
  for (duckdb::idx_t i = 0; i < r->RowCount(); ++i)
    rows.emplace_back(r->GetValue(0, i).ToString(), r->GetValue(1, i).GetValue<int64_t>());
  return rows;
}

struct Fuzz {
  duckdb::Connection &c;
  std::mt19937_64 rng;
  bool synthetic;
  std::vector<int64_t> keys;  // candidate orderkeys
  int64_t next_key;
  int32_t base_day;
  std::set<std::pair<int, int64_t>> used;  // (table, rowid) touched in this txn

  Fuzz(duckdb::Connection &c, uint64_t seed, bool synthetic)
      : c(c), rng(seed), synthetic(synthetic) {}

  int64_t R(int64_t n) { return static_cast<int64_t>(rng() % static_cast<uint64_t>(n)); }
  bool P(double p) { return (rng() % 1000000) < p * 1000000; }

  int64_t PickKey() { return keys[R(keys.size())]; }

  // ── row pickers (rowid + projected values), excluding rows used in this txn
  std::optional<std::vector<duckdb::Value>> PickRow(Table t, const std::string &sql) {
    auto r = Q(c, sql);
    std::vector<duckdb::idx_t> ok;
    for (duckdb::idx_t i = 0; i < r->RowCount(); ++i)
      if (!used.count({t, r->GetValue(0, i).GetValue<int64_t>()}))
        ok.push_back(i);
    if (ok.empty())
      return std::nullopt;
    const auto i = ok[R(ok.size())];
    std::vector<duckdb::Value> row;
    for (duckdb::idx_t col = 0; col < r->ColumnCount(); ++col)
      row.push_back(r->GetValue(col, i));
    used.insert({t, row[0].GetValue<int64_t>()});
    return row;
  }
  std::optional<std::vector<duckdb::Value>> PickLineitem() {
    return PickRow(kLineitem, "SELECT rowid, l_orderkey, l_suppkey, l_commitdate, l_receiptdate "
                              "FROM lineitem WHERE l_orderkey = " + std::to_string(PickKey()));
  }
  std::optional<std::vector<duckdb::Value>> PickOrder() {
    return PickRow(kOrders, "SELECT rowid, o_orderkey, o_orderstatus FROM orders WHERE o_orderkey = " +
                                std::to_string(PickKey()));
  }
  std::optional<std::vector<duckdb::Value>> PickSupplier() {
    return PickRow(kSupplier, synthetic ? "SELECT rowid, s_suppkey, s_name, s_nationkey FROM supplier"
                                        : "SELECT rowid, s_suppkey, s_name, s_nationkey FROM supplier "
                                          "WHERE s_suppkey <= 60");
  }
  std::optional<std::vector<duckdb::Value>> PickNation() {
    return PickRow(kNation, "SELECT rowid, n_nationkey, n_name FROM nation");
  }

  int64_t SuppKey() { return synthetic ? 1 + R(11) : 1 + R(60); }
  int32_t NationKey() { return synthetic ? static_cast<int32_t>(R(9)) : static_cast<int32_t>(R(26)); }

  static core::AuxRow Li(int64_t ok, int64_t sk, int32_t cd, int32_t rd) {
    core::AuxRow r;
    r.values = {I(ok), I(sk), D(cd), D(rd)};
    return r;
  }
  static core::AuxRow Ord(int64_t ok, const std::string &st) {
    core::AuxRow r;
    r.values = {I(ok), S(st)};
    return r;
  }
  static core::AuxRow Sup(int64_t sk, const std::string &name, int64_t nk) {
    core::AuxRow r;
    r.values = {I(sk), S(name), I(nk)};
    return r;
  }
  static core::AuxRow Nat(int64_t nk, const std::string &name) {
    core::AuxRow r;
    r.values = {I(nk), S(name)};
    return r;
  }
  static catalog::SharedProbe Probe(Table t, core::MutationKind kind, std::string sql) {
    catalog::SharedProbe p;
    p.table = t;
    p.kind = kind;
    p.update_sql = std::move(sql);
    return p;
  }

  std::string LiInsertSql(int64_t ok, int64_t sk, int32_t cd, int32_t rd) {
    return "INSERT INTO lineitem SELECT * REPLACE (" + std::to_string(ok) + " AS l_orderkey, " +
           std::to_string(sk) + " AS l_suppkey, " + std::to_string(1000 + R(1000000)) +
           " AS l_linenumber, " + Date(cd) + " AS l_commitdate, " + Date(rd) +
           " AS l_receiptdate) FROM tmpl_l";
  }

  // One random operation → probes (exact captured deltas); may add nothing.
  void Op(std::vector<catalog::SharedProbe> &out) {
    const int kind = static_cast<int>(R(100));
    const int32_t cd = base_day;
    if (kind < 16) {  // lineitem insert (existing / new / orphan order)
      const int64_t ok = P(0.8) ? PickKey() : next_key++;
      const int64_t sk = SuppKey();
      const int32_t rd = cd + (P(0.5) ? 1 + R(3) : -1 - R(3));
      auto p = Probe(kLineitem, core::MutationKind::Insert, LiInsertSql(ok, sk, cd, rd));
      p.new_row = Li(ok, sk, cd, rd);
      out.push_back(p);
      if (std::find(keys.begin(), keys.end(), ok) == keys.end())
        keys.push_back(ok);
    } else if (kind < 26) {  // lineitem delete
      if (auto r = PickLineitem()) {
        auto p = Probe(kLineitem, core::MutationKind::Delete,
                       "DELETE FROM lineitem WHERE rowid = " + (*r)[0].ToString());
        p.old_row = Li((*r)[1].GetValue<int64_t>(), (*r)[2].GetValue<int64_t>(), Days((*r)[3]), Days((*r)[4]));
        out.push_back(p);
      }
    } else if (kind < 44) {  // lineitem update: any Q21 column, alone or several at once
      if (auto r = PickLineitem()) {
        int64_t ok = (*r)[1].GetValue<int64_t>(), sk = (*r)[2].GetValue<int64_t>();
        int32_t c0 = Days((*r)[3]), r0 = Days((*r)[4]);
        int64_t ok2 = ok, sk2 = sk;
        int32_t c2 = c0, r2 = r0;
        std::vector<std::string> sets;
        // lateness flips either way relative to the other date
        auto Around = [&](int32_t ref) { return ref + (P(0.5) ? 1 + R(4) : -1 - R(4)); };
        auto SetRd = [&] { r2 = Around(c2); sets.push_back("l_receiptdate = " + Date(r2)); };
        auto SetCd = [&] { c2 = Around(r2); sets.push_back("l_commitdate = " + Date(c2)); };
        auto SetSk = [&] { sk2 = SuppKey(); sets.push_back("l_suppkey = " + std::to_string(sk2)); };
        auto SetOk = [&] {
          ok2 = P(0.9) ? PickKey() : next_key++;
          sets.push_back("l_orderkey = " + std::to_string(ok2));
          if (std::find(keys.begin(), keys.end(), ok2) == keys.end())
            keys.push_back(ok2);
        };
        switch (static_cast<int>(R(9))) {
        case 0: SetRd(); break;
        case 1: SetSk(); break;
        case 2: SetOk(); break;
        case 3: SetCd(); break;
        case 4: SetCd(); SetRd(); break;
        case 5: SetSk(); SetRd(); break;
        case 6: SetOk(); SetSk(); break;
        case 7: SetOk(); SetSk(); SetCd(); SetRd(); break;
        default: SetSk(); SetCd(); break;
        }
        std::string set;
        for (const auto &x : sets)
          set += (set.empty() ? "" : ", ") + x;
        auto p = Probe(kLineitem, core::MutationKind::Update,
                       "UPDATE lineitem SET " + set + " WHERE rowid = " + (*r)[0].ToString());
        p.old_row = Li(ok, sk, c0, r0);
        p.new_row = Li(ok2, sk2, c2, r2);
        out.push_back(p);
      }
    } else if (kind < 54) {  // orders insert (new or duplicate key)
      const int64_t ok = P(0.5) ? PickKey() : next_key++;
      const std::string st = std::string(1, "FFOP"[R(4)]);
      auto p = Probe(kOrders, core::MutationKind::Insert,
                     "INSERT INTO orders SELECT * REPLACE (" + std::to_string(ok) +
                         " AS o_orderkey, " + Lit(st) + " AS o_orderstatus) FROM tmpl_o");
      p.new_row = Ord(ok, st);
      out.push_back(p);
      if (std::find(keys.begin(), keys.end(), ok) == keys.end())
        keys.push_back(ok);
    } else if (kind < 60) {  // orders delete
      if (auto r = PickOrder()) {
        auto p = Probe(kOrders, core::MutationKind::Delete,
                       "DELETE FROM orders WHERE rowid = " + (*r)[0].ToString());
        p.old_row = Ord((*r)[1].GetValue<int64_t>(), (*r)[2].ToString());
        out.push_back(p);
      }
    } else if (kind < 72) {  // orders update: status (incl. to/from 'F') / key
      if (auto r = PickOrder()) {
        const int64_t ok = (*r)[1].GetValue<int64_t>();
        const std::string st = (*r)[2].ToString();
        int64_t ok2 = ok;
        std::string st2 = st, set;
        const double w = static_cast<double>(R(100)) / 100;
        if (w < 0.6) {
          st2 = std::string(1, "FFOP"[R(4)]);
          set = "o_orderstatus = " + Lit(st2);
        } else if (w < 0.8) {
          ok2 = PickKey();
          set = "o_orderkey = " + std::to_string(ok2);
        } else {
          ok2 = PickKey();
          st2 = std::string(1, "FFOP"[R(4)]);
          set = "o_orderkey = " + std::to_string(ok2) + ", o_orderstatus = " + Lit(st2);
        }
        auto p = Probe(kOrders, core::MutationKind::Update,
                       "UPDATE orders SET " + set + " WHERE rowid = " + (*r)[0].ToString());
        p.old_row = Ord(ok, st);
        p.new_row = Ord(ok2, st2);
        out.push_back(p);
      }
    } else if (kind < 86) {  // supplier insert / delete / update (nation, name, key)
      const int sub = static_cast<int>(R(8));
      if (sub == 0) {
        const int64_t sk = SuppKey();
        const std::string name = kSupplierNames[R(kSupplierNames.size())];
        const int64_t nk = NationKey();
        auto p = Probe(kSupplier, core::MutationKind::Insert,
                       "INSERT INTO supplier SELECT * REPLACE (" + std::to_string(sk) + " AS s_suppkey, " +
                           Lit(name) + " AS s_name, " + std::to_string(nk) + " AS s_nationkey) FROM tmpl_s");
        p.new_row = Sup(sk, name, nk);
        out.push_back(p);
      } else if (auto r = PickSupplier()) {
        const int64_t sk = (*r)[1].GetValue<int64_t>(), nk = (*r)[3].GetValue<int64_t>();
        const std::string name = (*r)[2].ToString();
        if (sub == 1) {
          auto p = Probe(kSupplier, core::MutationKind::Delete,
                         "DELETE FROM supplier WHERE rowid = " + (*r)[0].ToString());
          p.old_row = Sup(sk, name, nk);
          out.push_back(p);
        } else {
          int64_t sk2 = sk, nk2 = nk;
          std::string name2 = name, set;
          std::vector<std::string> sets;
          auto SetNk = [&] { nk2 = NationKey(); sets.push_back("s_nationkey = " + std::to_string(nk2)); };
          auto SetName = [&] {
            name2 = kSupplierNames[R(kSupplierNames.size())];
            sets.push_back("s_name = " + Lit(name2));
          };
          auto SetSk = [&] { sk2 = SuppKey(); sets.push_back("s_suppkey = " + std::to_string(sk2)); };
          if (sub == 2) {
            SetNk();
          } else if (sub == 3) {
            SetName();
          } else if (sub == 4) {
            SetSk();
          } else if (sub == 5) {
            SetNk(); SetName();
          } else if (sub == 6) {
            SetSk(); SetNk();
          } else {
            SetSk(); SetName(); SetNk();
          }
          for (const auto &x : sets)
            set += (set.empty() ? "" : ", ") + x;
          auto p = Probe(kSupplier, core::MutationKind::Update,
                         "UPDATE supplier SET " + set + " WHERE rowid = " + (*r)[0].ToString());
          p.old_row = Sup(sk, name, nk);
          p.new_row = Sup(sk2, name2, nk2);
          out.push_back(p);
        }
      }
    } else if (kind < 96) {  // nation insert / delete / rename / rekey
      const int sub = static_cast<int>(R(5));
      if (sub == 0) {
        const int64_t nk = NationKey();
        const std::string name = kNationNames[R(kNationNames.size())];
        auto p = Probe(kNation, core::MutationKind::Insert,
                       "INSERT INTO nation SELECT * REPLACE (" + std::to_string(nk) + " AS n_nationkey, " +
                           Lit(name) + " AS n_name) FROM tmpl_n");
        p.new_row = Nat(nk, name);
        out.push_back(p);
      } else if (auto r = PickNation()) {
        const int64_t nk = (*r)[1].GetValue<int64_t>();
        const std::string name = (*r)[2].ToString();
        if (sub == 1) {
          auto p = Probe(kNation, core::MutationKind::Delete,
                         "DELETE FROM nation WHERE rowid = " + (*r)[0].ToString());
          p.old_row = Nat(nk, name);
          out.push_back(p);
        } else {
          int64_t nk2 = nk;
          std::string name2 = name, set;
          if (sub == 2) {
            name2 = kNationNames[R(kNationNames.size())];
            set = "n_name = " + Lit(name2);
          } else if (sub == 3) {
            nk2 = NationKey();
            set = "n_nationkey = " + std::to_string(nk2);
          } else {
            name2 = kNationNames[R(kNationNames.size())];
            nk2 = NationKey();
            set = "n_name = " + Lit(name2) + ", n_nationkey = " + std::to_string(nk2);
          }
          auto p = Probe(kNation, core::MutationKind::Update,
                         "UPDATE nation SET " + set + " WHERE rowid = " + (*r)[0].ToString());
          p.old_row = Nat(nk, name);
          p.new_row = Nat(nk2, name2);
          out.push_back(p);
        }
      }
    } else {  // no-op for Q21: update of a column Q21 does not read
      if (auto r = PickLineitem()) {
        auto p = Probe(kLineitem, core::MutationKind::Update,
                       "UPDATE lineitem SET l_comment = 'fz' WHERE rowid = " + (*r)[0].ToString());
        const auto row = Li((*r)[1].GetValue<int64_t>(), (*r)[2].GetValue<int64_t>(), Days((*r)[3]), Days((*r)[4]));
        p.old_row = row;
        p.new_row = row;
        out.push_back(p);
      }
    }
  }

  std::vector<catalog::SharedProbe> Txn() {
    used.clear();
    std::vector<catalog::SharedProbe> probes;
    if (P(0.05)) {  // large batch: many lineitem inserts on a few orders
      const int n = 20 + static_cast<int>(R(41));
      std::vector<int64_t> oks;
      for (int i = 0; i < 3; ++i)
        oks.push_back(P(0.7) ? PickKey() : next_key++);
      for (auto ok : oks)
        if (std::find(keys.begin(), keys.end(), ok) == keys.end())
          keys.push_back(ok);
      for (int i = 0; i < n; ++i) {
        const int64_t ok = oks[R(oks.size())], sk = SuppKey();
        const int32_t rd = base_day + (P(0.4) ? 1 : -1);
        auto p = Probe(kLineitem, core::MutationKind::Insert, LiInsertSql(ok, sk, base_day, rd));
        p.new_row = Li(ok, sk, base_day, rd);
        probes.push_back(p);
      }
      return probes;
    }
    const int ops = P(0.7) ? 1 : 2 + static_cast<int>(R(3));
    for (int i = 0; i < ops; ++i)
      Op(probes);
    return probes;
  }
};

void SetupSynthetic(duckdb::Connection &c, std::mt19937_64 &rng) {
  for (const char *t : {"tmpl_l", "tmpl_o", "tmpl_s", "tmpl_n"})
    Q(c, std::string("DROP TABLE IF EXISTS ") + t);
  Q(c, "CREATE TABLE tmpl_l AS SELECT * FROM lineitem LIMIT 1");
  Q(c, "CREATE TABLE tmpl_o AS SELECT * FROM orders LIMIT 1");
  Q(c, "CREATE TABLE tmpl_s AS SELECT * FROM supplier LIMIT 1");
  Q(c, "CREATE TABLE tmpl_n AS SELECT * FROM nation LIMIT 1");
  for (const char *t : {"lineitem", "orders", "supplier", "nation"})
    Q(c, std::string("DELETE FROM ") + t);
  auto R = [&](int64_t n) { return static_cast<int64_t>(rng() % static_cast<uint64_t>(n)); };
  // nations: duplicate names (PERU) and a duplicate key (1)
  const std::vector<std::pair<int, std::string>> nations = {
      {0, "SAUDI ARABIA"}, {1, "PERU"}, {2, "KENYA"}, {3, "PERU"}, {4, ""},
      {5, "\xC3\x89GYPTE"}, {6, "O'NEIL"}, {7, "A|B"}, {1, "KENYA"}};
  for (const auto &[nk, name] : nations)
    Q(c, "INSERT INTO nation SELECT * REPLACE (" + std::to_string(nk) + " AS n_nationkey, " +
             Lit(name) + " AS n_name) FROM tmpl_n");
  // suppliers 1..10 (+ a duplicate key 3); key 11 is referenced but absent
  for (int sk = 1; sk <= 10; ++sk)
    Q(c, "INSERT INTO supplier SELECT * REPLACE (" + std::to_string(sk) + " AS s_suppkey, " +
             Lit(kSupplierNames[R(kSupplierNames.size())]) + " AS s_name, " + std::to_string(R(9)) +
             " AS s_nationkey) FROM tmpl_s");
  Q(c, "INSERT INTO supplier SELECT * REPLACE (3 AS s_suppkey, 'Supp#03' AS s_name, 0 AS s_nationkey) FROM tmpl_s");
  for (int ok = 1; ok <= 40; ++ok) {
    Q(c, "INSERT INTO orders SELECT * REPLACE (" + std::to_string(ok) + " AS o_orderkey, " +
             Lit(std::string(1, "FFFOP"[R(5)])) + " AS o_orderstatus) FROM tmpl_o");
    if (R(6) == 0)  // duplicate 'F' row
      Q(c, "INSERT INTO orders SELECT * REPLACE (" + std::to_string(ok) + " AS o_orderkey, 'F' AS o_orderstatus) FROM tmpl_o");
  }
  for (int ok = 1; ok <= 45; ++ok) {  // 41..45 have no order
    const int n = 1 + static_cast<int>(R(5));
    for (int ln = 1; ln <= n; ++ln)
      Q(c, "INSERT INTO lineitem SELECT * REPLACE (" + std::to_string(ok) + " AS l_orderkey, " +
               std::to_string(1 + R(11)) + " AS l_suppkey, " + std::to_string(ln) + " AS l_linenumber, " +
               Date(9500) + " AS l_commitdate, " + Date(9500 + (R(2) ? 1 + R(3) : -1 - R(3))) +
               " AS l_receiptdate) FROM tmpl_l");
  }
}

} // namespace

int main(int argc, char **argv) {
  try {
    Require(argc >= 6, "usage: q21_verus_duckdb_fuzz <db-copy> <txns> <seed> <synthetic|sf1> <policy|ApplyDelta>");
    const std::string path = argv[1];
    const int txns = std::atoi(argv[2]);
    const uint64_t seed = std::strtoull(argv[3], nullptr, 10);
    const bool synthetic = std::string(argv[4]) == "synthetic";
    const std::string force = argv[5];
    if (force != "policy")
      core::SetForcedMaintenanceActionName(force);

    duckdb::DuckDB db(path);
    duckdb::Connection rc(db), qc(db);
    Q(rc, "PRAGMA threads=1");
    std::mt19937_64 setup_rng(seed ^ 0x9e3779b97f4a7c15ULL);
    if (synthetic) {
      SetupSynthetic(rc, setup_rng);
    } else {
      for (const char *t : {"tmpl_l", "tmpl_o", "tmpl_s", "tmpl_n"})
        Q(rc, std::string("DROP TABLE IF EXISTS ") + t);
      Q(rc, "CREATE TABLE tmpl_l AS SELECT * FROM lineitem LIMIT 1");
      Q(rc, "CREATE TABLE tmpl_o AS SELECT * FROM orders LIMIT 1");
      Q(rc, "CREATE TABLE tmpl_s AS SELECT * FROM supplier LIMIT 1");
      Q(rc, "CREATE TABLE tmpl_n AS SELECT * FROM nation LIMIT 1");
    }
    accelerator_codegen::RegisterAcceleratorFunctions(qc);
    DuckdbRuntime rt(rc, *catalog::FindSharedQueryByAccelerator("tpch_q21_verus"), 10.0);
    rt.Build();

    Fuzz fz(rc, seed, synthetic);
    {
      auto r = synthetic ? Q(rc, "SELECT DISTINCT l_orderkey FROM lineitem UNION SELECT o_orderkey FROM orders ORDER BY 1")
                         : Q(rc, "SELECT o_orderkey FROM orders WHERE o_orderkey % 97 = 0 ORDER BY 1 LIMIT 5000");
      for (duckdb::idx_t i = 0; i < r->RowCount(); ++i)
        fz.keys.push_back(r->GetValue(0, i).GetValue<int64_t>());
      fz.next_key = Q(rc, "SELECT max(o_orderkey) + 1000 FROM orders")->GetValue(0, 0).GetValue<int64_t>();
      fz.base_day = synthetic ? 9500 : Days(Q(rc, "SELECT min(l_commitdate) FROM lineitem")->GetValue(0, 0)) + 100;
    }

    std::map<std::string, int> actions;
    int queries = 0, mismatches = 0, probes_total = 0, nonempty = 0;
    auto check = [&](const std::optional<std::string> &p, int t) {
      const auto a = Fetch(qc, AccelSql(p)), b = Fetch(qc, BaseSql(p));
      ++queries;
      nonempty += !b.empty();
      if (a != b) {
        if (++mismatches <= 10)
          std::cout << "MISMATCH txn " << t << " param " << (p ? "'" + *p + "'" : "NULL") << ": accel "
                    << a.size() << " rows, sql " << b.size() << " rows\n";
      }
    };
    for (int t = 0; t < txns; ++t) {
      auto probes = fz.Txn();
      if (probes.empty())
        continue;
      probes_total += probes.size();
      core::ClearMaintenanceDecisionLog();
      rt.ApplyBatch(probes);
      const auto d = core::LastMaintenanceDecision();
      actions[d ? std::to_string(static_cast<int>(d->staged_action)) : "none"]++;
      if (t % 25 == 24) {
        for (const auto &p : kParams)
          check(p, t);
      } else {
        check(kParams[fz.R(kParams.size())], t);
        check(kParams[fz.R(kParams.size())], t);
      }
    }
    for (const auto &p : kParams)
      check(p, txns);
    std::cout << "q21 verus duckdb fuzz: mode " << argv[4] << " policy " << force << " seed " << seed
              << " txns " << txns << " row_changes " << probes_total << " queries " << queries
              << " nonempty " << nonempty << " mismatches " << mismatches << " staged_actions {";
    for (const auto &[k, v] : actions)
      std::cout << " " << k << ":" << v;
    std::cout << " } (1=ApplyDelta, 3=RebuildAll)\n";
    // ACCEL_AUX_STORE=verus: the state lives in the verified Verus store.
    const bool ok = mismatches == 0 && rt.StaleInstallCount() == 0;
    std::cout << "q21 verus duckdb fuzz: store " << (rt.UsesVerusStore() ? "verus" : "cpp")
              << " versions " << rt.StateVersionCount() << " stale_installs "
              << rt.StaleInstallCount() << "\n";
    std::cout << "q21 verus duckdb fuzz: " << (ok ? "PASS" : "FAIL") << std::endl;
    return ok ? 0 : 1;
  } catch (const std::exception &e) {
    std::cerr << "q21_verus_duckdb_fuzz failed: " << e.what() << "\n";
    return 2;
  }
}
