// GENERATED: TPC-H calibration writes; see calibration_writes.hpp.
#include "tpch/calibration_writes.hpp"

#include <string>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {

using Shape = std::pair<const char *, const char *>; // name, WHERE

std::vector<core::CalibrationWrite> Writes(const char *table, const char *change, const char *undo,
                                           const char *control, const char *control_undo,
                                           const std::vector<Shape> &shapes) {
  std::vector<core::CalibrationWrite> out;
  const std::string update = std::string("UPDATE ") + table + " SET ";
  for (const auto &[name, where] : shapes) {
    const std::string w = std::string(" WHERE ") + where;
    out.push_back({name, update + change + w, update + undo + w, update + control + w,
                   update + control_undo + w});
  }
  return out;
}
// lineitem: by l_shipdate
const std::vector<Shape> kLineitemShapes = {
    {"point", "l_orderkey = 1 AND l_linenumber = 1"},
    {"1 day", "l_shipdate = DATE '1995-06-17'"},
    {"1 week", "l_shipdate BETWEEN DATE '1995-06-11' AND DATE '1995-06-17'"},
    {"1 month", "l_shipdate BETWEEN DATE '1995-06-01' AND DATE '1995-06-30'"},
    {"1 year", "l_shipdate BETWEEN DATE '1995-01-01' AND DATE '1995-12-31'"},
    {"all rows", "1 = 1"},
};
// orders: by o_orderdate
const std::vector<Shape> kOrdersShapes = {
    {"point", "o_orderkey = 1"},
    {"1 day", "o_orderdate = DATE '1995-06-17'"},
    {"1 week", "o_orderdate BETWEEN DATE '1995-06-11' AND DATE '1995-06-17'"},
    {"1 month", "o_orderdate BETWEEN DATE '1995-06-01' AND DATE '1995-06-30'"},
    {"1 year", "o_orderdate BETWEEN DATE '1995-01-01' AND DATE '1995-12-31'"},
    {"all rows", "1 = 1"},
};
// partsupp: by ps_partkey
const std::vector<Shape> kPartsuppShapes = {
    {"point", "ps_partkey = 1"},
    {"10 keys", "ps_partkey <= 10"},
    {"100 keys", "ps_partkey <= 100"},
    {"1000 keys", "ps_partkey <= 1000"},
    {"10000 keys", "ps_partkey <= 10000"},
    {"all rows", "1 = 1"},
};
// part: by p_partkey
const std::vector<Shape> kPartShapes = {
    {"point", "p_partkey = 1"},
    {"10 keys", "p_partkey <= 10"},
    {"100 keys", "p_partkey <= 100"},
    {"1000 keys", "p_partkey <= 1000"},
    {"10000 keys", "p_partkey <= 10000"},
    {"all rows", "1 = 1"},
};
// customer: by c_custkey
const std::vector<Shape> kCustomerShapes = {
    {"point", "c_custkey = 1"},
    {"10 keys", "c_custkey <= 10"},
    {"100 keys", "c_custkey <= 100"},
    {"1000 keys", "c_custkey <= 1000"},
    {"10000 keys", "c_custkey <= 10000"},
    {"all rows", "1 = 1"},
};
// supplier: by s_suppkey
const std::vector<Shape> kSupplierShapes = {
    {"point", "s_suppkey = 1"},
    {"10 keys", "s_suppkey <= 10"},
    {"100 keys", "s_suppkey <= 100"},
    {"1000 keys", "s_suppkey <= 1000"},
    {"10000 keys", "s_suppkey <= 10000"},
    {"all rows", "1 = 1"},
};
} // namespace

std::vector<core::CalibrationWrite> TpchCalibrationWrites(int query) {
  switch (query) {
  case 1:
    return Writes("lineitem", "l_quantity = l_quantity + 1", "l_quantity = l_quantity - 1", "l_suppkey = l_suppkey + 1", "l_suppkey = l_suppkey - 1",
                  kLineitemShapes);
  case 2: { // a supplier that is not a part's cheapest (cost > 900, the minimum is
            // almost always lower) changes in place; a cheapest one rebuilds
    std::vector<Shape> shapes;
    static const std::vector<std::string> wheres = [] {
      std::vector<std::string> out;
      for (const auto &[name, where] : kPartsuppShapes)
        out.push_back(std::string(where) + " AND ps_supplycost > 900");
      return out;
    }();
    for (std::size_t i = 0; i < kPartsuppShapes.size(); ++i)
      shapes.push_back({kPartsuppShapes[i].first, wheres[i].c_str()});
    return Writes("partsupp", "ps_supplycost = ps_supplycost - 1", "ps_supplycost = ps_supplycost + 1",
                  "ps_availqty = ps_availqty + 1", "ps_availqty = ps_availqty - 1", shapes);
  }
  case 3:
    return Writes("lineitem", "l_extendedprice = l_extendedprice + 1", "l_extendedprice = l_extendedprice - 1", "l_quantity = l_quantity + 1", "l_quantity = l_quantity - 1",
                  kLineitemShapes);
  case 4:
    return Writes("lineitem", "l_receiptdate = l_receiptdate + INTERVAL '1' DAY", "l_receiptdate = l_receiptdate - INTERVAL '1' DAY", "l_quantity = l_quantity + 1", "l_quantity = l_quantity - 1",
                  kLineitemShapes);
  case 5:
    return Writes("lineitem", "l_extendedprice = l_extendedprice + 1", "l_extendedprice = l_extendedprice - 1", "l_quantity = l_quantity + 1", "l_quantity = l_quantity - 1",
                  kLineitemShapes);
  case 6:
    return Writes("lineitem", "l_extendedprice = l_extendedprice + 1", "l_extendedprice = l_extendedprice - 1", "l_tax = l_tax + 1", "l_tax = l_tax - 1",
                  kLineitemShapes);
  case 7:
    return Writes("lineitem", "l_extendedprice = l_extendedprice + 1", "l_extendedprice = l_extendedprice - 1", "l_quantity = l_quantity + 1", "l_quantity = l_quantity - 1",
                  kLineitemShapes);
  case 8:
    return Writes("lineitem", "l_extendedprice = l_extendedprice + 1", "l_extendedprice = l_extendedprice - 1", "l_quantity = l_quantity + 1", "l_quantity = l_quantity - 1",
                  kLineitemShapes);
  case 9:
    return Writes("lineitem", "l_extendedprice = l_extendedprice + 1", "l_extendedprice = l_extendedprice - 1", "l_tax = l_tax + 1", "l_tax = l_tax - 1",
                  kLineitemShapes);
  case 10:
    return Writes("lineitem", "l_extendedprice = l_extendedprice + 1", "l_extendedprice = l_extendedprice - 1", "l_quantity = l_quantity + 1", "l_quantity = l_quantity - 1",
                  kLineitemShapes);
  case 11:
    return Writes("partsupp", "ps_supplycost = ps_supplycost + 1", "ps_supplycost = ps_supplycost - 1", "ps_comment = concat(ps_comment, 'x')", "ps_comment = substr(ps_comment, 1, length(ps_comment) - 1)",
                  kPartsuppShapes);
  case 12:
    return Writes("lineitem", "l_receiptdate = l_receiptdate + INTERVAL '1' DAY", "l_receiptdate = l_receiptdate - INTERVAL '1' DAY", "l_quantity = l_quantity + 1", "l_quantity = l_quantity - 1",
                  kLineitemShapes);
  case 13:
    return Writes("orders", "o_custkey = o_custkey + 1", "o_custkey = o_custkey - 1", "o_totalprice = o_totalprice + 1", "o_totalprice = o_totalprice - 1",
                  kOrdersShapes);
  case 14:
    return Writes("lineitem", "l_extendedprice = l_extendedprice + 1", "l_extendedprice = l_extendedprice - 1", "l_quantity = l_quantity + 1", "l_quantity = l_quantity - 1",
                  kLineitemShapes);
  case 15:
    return Writes("lineitem", "l_extendedprice = l_extendedprice + 1", "l_extendedprice = l_extendedprice - 1", "l_quantity = l_quantity + 1", "l_quantity = l_quantity - 1",
                  kLineitemShapes);
  case 16: // supplier comments (complaints) are maintained in place; part changes rebuild
    return Writes("supplier", "s_comment = concat(s_comment, 'x')",
                  "s_comment = substr(s_comment, 1, length(s_comment) - 1)", "s_acctbal = s_acctbal + 1",
                  "s_acctbal = s_acctbal - 1", kSupplierShapes);
  case 17:
    return Writes("lineitem", "l_quantity = l_quantity + 1", "l_quantity = l_quantity - 1", "l_tax = l_tax + 1", "l_tax = l_tax - 1",
                  kLineitemShapes);
  case 18:
    return Writes("lineitem", "l_quantity = l_quantity + 1", "l_quantity = l_quantity - 1", "l_tax = l_tax + 1", "l_tax = l_tax - 1",
                  kLineitemShapes);
  case 19:
    return Writes("lineitem", "l_quantity = l_quantity + 1", "l_quantity = l_quantity - 1", "l_tax = l_tax + 1", "l_tax = l_tax - 1",
                  kLineitemShapes);
  case 20:
    return Writes("lineitem", "l_quantity = l_quantity + 1", "l_quantity = l_quantity - 1", "l_extendedprice = l_extendedprice + 1", "l_extendedprice = l_extendedprice - 1",
                  kLineitemShapes);
  case 21:
    return Writes("lineitem", "l_receiptdate = l_receiptdate + INTERVAL '1' DAY", "l_receiptdate = l_receiptdate - INTERVAL '1' DAY", "l_quantity = l_quantity + 1", "l_quantity = l_quantity - 1",
                  kLineitemShapes);
  case 22:
    return Writes("customer", "c_acctbal = c_acctbal + 1", "c_acctbal = c_acctbal - 1", "c_nationkey = c_nationkey + 1", "c_nationkey = c_nationkey - 1",
                  kCustomerShapes);
  default:
    return {};
  }
}

} // namespace accelerator_codegen::aux_state::query
