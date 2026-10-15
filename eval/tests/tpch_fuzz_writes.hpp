// Random write statements on the TPC-H tables, for the aux-state fuzz tests
// (DuckDB, PostgreSQL, MySQL dialects).
//
// The writes change the columns the 22 queries read, insert copied rows under
// fresh keys, and delete rows. Data stays valid TPC-H: keys stay unique and
// every foreign key keeps resolving (reassigned keys are picked from existing
// rows; lineitem suppliers come from the part's partsupp rows; only rows the
// fuzz inserted, or whole orders with their lines, are deleted), so a
// mismatch is about maintenance, not about data no TPC-H database holds.
//
// Each Next() is one or more statements separated by ";\n".
#pragma once

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <random>
#include <string>

namespace accelerator_codegen::test_support {

enum class FuzzDialect { DuckDB, Postgres, MySQL };

class TpchFuzzWrites {
public:
  // `scalar` runs a query returning one integer (used for the key maxima and
  // to pick source rows).
  TpchFuzzWrites(FuzzDialect dialect_p, uint64_t seed, std::function<int64_t(const std::string &)> scalar_p)
      : dialect(dialect_p), rng(seed), scalar(std::move(scalar_p)) {
    max_order = scalar("SELECT max(o_orderkey) FROM orders");
    max_cust = scalar("SELECT max(c_custkey) FROM customer");
    max_part = scalar("SELECT max(p_partkey) FROM part");
    max_supp = scalar("SELECT max(s_suppkey) FROM supplier");
    base_cust = max_cust;
    base_part = max_part;
    base_supp = max_supp;
  }

  std::string Next() { return Next({30, 18, 10, 10, 8, 10, 3, 1}); }

  // One write on a table drawn by `weights`: lineitem, orders, customer, part,
  // partsupp, supplier, nation, region (a test that only needs some tables).
  std::string Next(std::initializer_list<int> weights) {
    switch (Pick(weights)) {
    case 0:
      return Lineitem();
    case 1:
      return Orders();
    case 2:
      return Customer();
    case 3:
      return Part();
    case 4:
      return Partsupp();
    case 5:
      return Supplier();
    case 6:
      return Nation();
    default:
      return Region();
    }
  }

private:
  int Pick(std::initializer_list<int> weights) {
    int total = 0;
    for (int w : weights) {
      total += w;
    }
    int r = static_cast<int>(rng() % static_cast<uint64_t>(total));
    int i = 0;
    for (int w : weights) {
      if (r < w) {
        return i;
      }
      r -= w;
      i++;
    }
    return i - 1;
  }
  int64_t Rand(int64_t lo, int64_t hi) { return lo + static_cast<int64_t>(rng() % static_cast<uint64_t>(hi - lo + 1)); }
  std::string Salt() { return std::to_string(rng() % 1000000); }
  std::string Lit(const std::string &s) { return "'" + s + "'"; }
  std::string Choice(std::initializer_list<const char *> xs) {
    auto it = xs.begin();
    std::advance(it, static_cast<long>(rng() % xs.size()));
    return Lit(*it);
  }

  // An order-by expression that shuffles rows by `key` (deterministic per salt).
  std::string Shuffle(const std::string &key) {
    switch (dialect) {
    case FuzzDialect::DuckDB:
      return "hash(" + key + ", " + Salt() + ")";
    case FuzzDialect::Postgres:
      return "md5((" + key + ")::text || '" + Salt() + "')";
    default:
      return "MD5(CONCAT(" + key + ", '" + Salt() + "'))";
    }
  }
  // Rows of `table` with primary key `pk` (comma-separated columns): n random ones.
  std::string Some(const std::string &table, const std::string &pk, int n) {
    if (dialect == FuzzDialect::DuckDB) {
      return "rowid IN (SELECT rowid FROM " + table + " ORDER BY hash(rowid, " + Salt() + ") LIMIT " +
             std::to_string(n) + ")";
    }
    const std::string pick = "SELECT " + pk + " FROM " + table + " ORDER BY " + Shuffle(Concat(pk)) +
                             " LIMIT " + std::to_string(n);
    // MySQL cannot read the table it updates in a subquery: go through a derived table.
    const std::string sub = dialect == FuzzDialect::MySQL ? "SELECT * FROM (" + pick + ") fuzz_pick" : pick;
    return "(" + pk + ") IN (" + sub + ")";
  }
  std::string Concat(const std::string &pk) {
    // pk columns as one value to hash
    std::string out;
    std::string col;
    for (char ch : pk + ",") {
      if (ch == ',') {
        if (!col.empty()) {
          out += out.empty() ? col : (dialect == FuzzDialect::MySQL ? ", '|', " : " || '|' || ") + col;
        }
        col.clear();
      } else if (ch != ' ') {
        col += ch;
      }
    }
    if (dialect == FuzzDialect::MySQL && out.find(',') != std::string::npos) {
      return "CONCAT(" + out + ")";
    }
    return out;
  }
  std::string AddDays(const std::string &col, int64_t days) {
    if (dialect == FuzzDialect::MySQL) {
      return "DATE_ADD(" + col + ", INTERVAL " + std::to_string(days) + " DAY)";
    }
    return col + " + (" + std::to_string(days) + ")";
  }
  std::string Days() { return std::to_string(Rand(-120, 120)); }
  std::string Append(const std::string &col, const std::string &suffix) {
    return dialect == FuzzDialect::MySQL ? "CONCAT(" + col + ", " + Lit(suffix) + ")"
                                         : col + " || " + Lit(suffix);
  }
  std::string Prefix(const std::string &prefix, const std::string &col) {
    return dialect == FuzzDialect::MySQL ? "CONCAT(" + Lit(prefix) + ", " + col + ")"
                                         : Lit(prefix) + " || " + col;
  }
  std::string Substr(const std::string &col, int from) {
    return "substr(" + col + ", " + std::to_string(from) + ")";
  }
  // One existing key of `table`.`col` (a scalar subquery).
  std::string Existing(const std::string &table, const std::string &col) {
    const std::string pick = "SELECT " + col + " FROM " + table + " ORDER BY " + Shuffle(col) + " LIMIT 1";
    return "(" + (dialect == FuzzDialect::MySQL ? "SELECT * FROM (" + pick + ") fuzz_key" : pick) + ")";
  }
  int64_t ExistingNow(const std::string &table, const std::string &col) {
    return scalar("SELECT " + col + " FROM " + table + " ORDER BY " + Shuffle(col) + " LIMIT 1");
  }

  static constexpr const char *kLineitemPk = "l_orderkey, l_linenumber";
  static constexpr const char *kPartsuppPk = "ps_partkey, ps_suppkey";

  std::string Lineitem() {
    const int n = static_cast<int>(Rand(1, 4));
    switch (Pick({40, 20, 15, 15, 10})) {
    case 0: {
      std::string set;
      switch (Rand(0, 9)) {
      case 0: set = "l_quantity = greatest(1, l_quantity + " + std::to_string(Rand(-10, 10)) + ")"; break;
      case 1: set = "l_extendedprice = l_extendedprice * " + std::to_string(Rand(50, 150)) + " / 100.0"; break;
      case 2: set = "l_discount = " + std::to_string(Rand(0, 10)) + " / 100.0"; break;
      case 3: set = "l_tax = " + std::to_string(Rand(0, 8)) + " / 100.0"; break;
      case 4: set = "l_returnflag = " + Choice({"R", "A", "N"}); break;
      case 5: set = "l_linestatus = " + Choice({"O", "F"}); break;
      case 6: set = "l_shipdate = " + AddDays("l_shipdate", Rand(-120, 120)); break;
      case 7: set = "l_commitdate = " + AddDays("l_commitdate", Rand(-120, 120)); break;
      case 8: set = "l_receiptdate = " + AddDays("l_receiptdate", Rand(-120, 120)); break;
      default:
        set = "l_shipmode = " + Choice({"MAIL", "SHIP", "AIR", "REG AIR", "TRUCK", "RAIL", "FOB"}) +
              ", l_shipinstruct = " + Choice({"DELIVER IN PERSON", "COLLECT COD", "NONE", "TAKE BACK RETURN"});
      }
      return "UPDATE lineitem SET " + set + " WHERE " + Some("lineitem", kLineitemPk, n);
    }
    case 1: { // another supplier of the same part
      const std::string pick = "SELECT ps_suppkey FROM partsupp WHERE ps_partkey = lineitem.l_partkey ORDER BY " +
                               Shuffle("ps_suppkey") + " LIMIT 1";
      return "UPDATE lineitem SET l_suppkey = (" + pick + ") WHERE " + Some("lineitem", kLineitemPk, n);
    }
    case 2: { // copy lines into their orders under new line numbers (above every existing one)
      const auto top = std::to_string(scalar("SELECT max(l_linenumber) FROM lineitem"));
      return "INSERT INTO lineitem SELECT l_orderkey, l_partkey, l_suppkey, " + top +
             " + row_number() OVER (), l_quantity, l_extendedprice, l_discount, l_tax, l_returnflag, l_linestatus, " +
             AddDays("l_shipdate", Rand(-120, 120)) + ", l_commitdate, " + AddDays("l_receiptdate", Rand(-120, 120)) +
             ", l_shipinstruct, l_shipmode, l_comment FROM lineitem WHERE " + Some("lineitem", kLineitemPk, n);
    }
    case 3:
      return "DELETE FROM lineitem WHERE " + Some("lineitem", kLineitemPk, n);
    default:
      return NewOrder();
    }
  }

  std::string NewOrder() {
    const int64_t k = ++max_order;
    const int64_t src = ExistingNow("orders", "o_orderkey");
    return "INSERT INTO orders SELECT " + std::to_string(k) + ", o_custkey, o_orderstatus, o_totalprice, " +
           AddDays("o_orderdate", Rand(-120, 120)) +
           ", o_orderpriority, o_clerk, o_shippriority, o_comment FROM orders WHERE o_orderkey = " +
           std::to_string(src) + ";\nINSERT INTO lineitem SELECT " + std::to_string(k) +
           ", l_partkey, l_suppkey, l_linenumber, l_quantity, l_extendedprice, l_discount, l_tax, "
           "l_returnflag, l_linestatus, l_shipdate, l_commitdate, l_receiptdate, l_shipinstruct, l_shipmode, "
           "l_comment FROM lineitem WHERE l_orderkey = " +
           std::to_string(src);
  }

  std::string Orders() {
    const int n = static_cast<int>(Rand(1, 3));
    switch (Pick({45, 15, 20, 20})) {
    case 0: {
      std::string set;
      switch (Rand(0, 4)) {
      case 0: set = "o_orderdate = " + AddDays("o_orderdate", Rand(-120, 120)); break;
      case 1: set = "o_orderstatus = " + Choice({"F", "O", "P"}); break;
      case 2: set = "o_orderpriority = " + Choice({"1-URGENT", "2-HIGH", "3-MEDIUM", "4-NOT SPECIFIED", "5-LOW"}); break;
      case 3: set = "o_custkey = " + Existing("customer", "c_custkey"); break;
      default: set = "o_comment = " + Choice({"special requests", "ironic deposits", "regular packages", "pending"});
      }
      return "UPDATE orders SET " + set + " WHERE " + Some("orders", "o_orderkey", n);
    }
    case 1:
      return "UPDATE orders SET o_shippriority = " + std::to_string(Rand(0, 1)) + " WHERE " +
             Some("orders", "o_orderkey", n);
    case 2: { // an order and its lines
      const auto k = std::to_string(ExistingNow("orders", "o_orderkey"));
      return "DELETE FROM lineitem WHERE l_orderkey = " + k + ";\nDELETE FROM orders WHERE o_orderkey = " + k;
    }
    default:
      return NewOrder();
    }
  }

  std::string Customer() {
    switch (Pick({60, 25, 15})) {
    case 0: {
      std::string set;
      switch (Rand(0, 4)) {
      case 0: set = "c_mktsegment = " + Choice({"BUILDING", "AUTOMOBILE", "MACHINERY", "HOUSEHOLD", "FURNITURE"}); break;
      case 1: set = "c_nationkey = " + std::to_string(Rand(0, 24)); break;
      case 2: set = "c_acctbal = c_acctbal + " + std::to_string(Rand(-5000, 5000)); break;
      case 3: set = "c_phone = " + Prefix(std::to_string(Rand(10, 34)), Substr("c_phone", 3)); break;
      default: set = "c_name = " + Append("c_name", "x");
      }
      return "UPDATE customer SET " + set + " WHERE " + Some("customer", "c_custkey", static_cast<int>(Rand(1, 3)));
    }
    case 1: {
      const int64_t k = ++max_cust;
      return "INSERT INTO customer SELECT " + std::to_string(k) + ", " + Lit("Customer#fuzz" + std::to_string(k)) +
             ", c_address, c_nationkey, c_phone, c_acctbal, c_mktsegment, c_comment FROM customer WHERE c_custkey = " +
             std::to_string(ExistingNow("customer", "c_custkey"));
    }
    default: // a fuzz customer with no orders
      return "DELETE FROM customer WHERE c_custkey > " + std::to_string(base_cust) +
             " AND c_custkey NOT IN (SELECT o_custkey FROM orders)";
    }
  }

  std::string Part() {
    switch (Pick({60, 25, 15})) {
    case 0: {
      std::string set;
      switch (Rand(0, 4)) {
      case 0: set = "p_brand = " + Lit("Brand#" + std::to_string(Rand(1, 5)) + std::to_string(Rand(1, 5))); break;
      case 1: set = "p_type = " + Choice({"PROMO BURNISHED COPPER", "ECONOMY ANODIZED STEEL", "STANDARD POLISHED BRASS", "MEDIUM PLATED TIN", "LARGE BRUSHED NICKEL"}); break;
      case 2: set = "p_size = " + std::to_string(Rand(1, 50)); break;
      case 3: set = "p_container = " + Choice({"SM CASE", "MED BOX", "LG PACK", "WRAP PKG", "JUMBO JAR", "SM PKG"}); break;
      default: set = "p_name = " + Append("p_name", " green");
      }
      return "UPDATE part SET " + set + " WHERE " + Some("part", "p_partkey", static_cast<int>(Rand(1, 3)));
    }
    case 1: { // a new part with suppliers
      const int64_t k = ++max_part;
      const auto src = std::to_string(ExistingNow("part", "p_partkey"));
      return "INSERT INTO part SELECT " + std::to_string(k) +
             ", p_name, p_mfgr, p_brand, p_type, p_size, p_container, p_retailprice, p_comment FROM part WHERE "
             "p_partkey = " + src + ";\nINSERT INTO partsupp SELECT " + std::to_string(k) +
             ", ps_suppkey, ps_availqty, ps_supplycost, ps_comment FROM partsupp WHERE ps_partkey = " + src;
    }
    default: // supplier rows of a fuzz part (no lineitem refers to fuzz parts)
      return "DELETE FROM partsupp WHERE ps_partkey > " + std::to_string(base_part) + " AND " +
             Some("partsupp", kPartsuppPk, 2);
    }
  }

  std::string Partsupp() {
    if (Pick({70, 30}) == 0) {
      return "UPDATE partsupp SET " +
             std::string(Rand(0, 1) ? "ps_supplycost = ps_supplycost * " + std::to_string(Rand(50, 150)) + " / 100.0"
                                    : "ps_availqty = " + std::to_string(Rand(1, 9999))) +
             " WHERE " + Some("partsupp", kPartsuppPk, static_cast<int>(Rand(1, 3)));
    }
    // a new supplier for an existing part (pair not present yet)
    const auto part = std::to_string(ExistingNow("part", "p_partkey"));
    const auto supp = std::to_string(ExistingNow("supplier", "s_suppkey"));
    return "INSERT INTO partsupp SELECT p_partkey, " + supp + ", " + std::to_string(Rand(1, 9999)) + ", " +
           std::to_string(Rand(100, 99999)) + " / 100.0, 'fuzz' FROM part WHERE p_partkey = " + part +
           " AND NOT EXISTS (SELECT 1 FROM partsupp WHERE ps_partkey = " + part + " AND ps_suppkey = " + supp + ")";
  }

  std::string Supplier() {
    switch (Pick({65, 20, 15})) {
    case 0: {
      std::string set;
      switch (Rand(0, 3)) {
      case 0: set = "s_nationkey = " + std::to_string(Rand(0, 24)); break;
      case 1: set = "s_acctbal = s_acctbal + " + std::to_string(Rand(-3000, 3000)); break;
      case 2: set = "s_comment = " + Choice({"Customer Complaints noted", "quick deposits", "Customer Recommends"}); break;
      default: set = "s_name = " + Append("s_name", "x");
      }
      return "UPDATE supplier SET " + set + " WHERE " + Some("supplier", "s_suppkey", static_cast<int>(Rand(1, 2)));
    }
    case 1: {
      const int64_t k = ++max_supp;
      return "INSERT INTO supplier SELECT " + std::to_string(k) + ", " + Lit("Supplier#fuzz" + std::to_string(k)) +
             ", s_address, s_nationkey, s_phone, s_acctbal, s_comment FROM supplier WHERE s_suppkey = " +
             std::to_string(ExistingNow("supplier", "s_suppkey"));
    }
    default: // fuzz suppliers nothing refers to
      return "DELETE FROM supplier WHERE s_suppkey > " + std::to_string(base_supp) +
             " AND s_suppkey NOT IN (SELECT l_suppkey FROM lineitem) AND s_suppkey NOT IN (SELECT ps_suppkey FROM partsupp)";
    }
  }

  // Renames keep names unique (as in TPC-H data): a marker is appended.
  std::string Nation() {
    if (Rand(0, 1)) {
      return "UPDATE nation SET n_regionkey = " + std::to_string(Rand(0, 4)) + " WHERE n_nationkey = " +
             std::to_string(Rand(0, 24));
    }
    return "UPDATE nation SET n_name = " + Append("n_name", "*") + " WHERE n_nationkey = " +
           std::to_string(Rand(0, 24));
  }

  std::string Region() {
    return "UPDATE region SET r_name = " + Append("r_name", "*") + " WHERE r_regionkey = " +
           std::to_string(Rand(0, 4));
  }

  FuzzDialect dialect;
  std::mt19937_64 rng;
  std::function<int64_t(const std::string &)> scalar;
  int64_t max_order = 0, max_cust = 0, max_part = 0, max_supp = 0;
  int64_t base_cust = 0, base_part = 0, base_supp = 0;
};

} // namespace accelerator_codegen::test_support
