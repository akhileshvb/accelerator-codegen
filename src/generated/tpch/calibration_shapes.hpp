// GENERATED (TPC-H, shared by its queries): building blocks for calibration
// writes (core::CalibrationWrite): per-table range shapes of increasing size
// and spread, and Writes(), which turns a change / undo / control on a table
// into one write per shape.  Each query's q<N>/calibration_writes.hpp picks
// its table and columns.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_calibration.hpp"
#include "accelerator_codegen/stateful/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::query::calibration {

namespace core = accelerator_codegen::aux_state::core;

using Shape = std::pair<const char *, const char *>; // name, WHERE

inline std::vector<core::CalibrationWrite> Writes(const char *table, const char *change, const char *undo,
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
inline const std::vector<Shape> kLineitemShapes = {
    {"point", "l_orderkey = 1 AND l_linenumber = 1"},
    {"1 day", "l_shipdate = DATE '1995-06-17'"},
    {"1 week", "l_shipdate BETWEEN DATE '1995-06-11' AND DATE '1995-06-17'"},
    {"1 month", "l_shipdate BETWEEN DATE '1995-06-01' AND DATE '1995-06-30'"},
    {"1 year", "l_shipdate BETWEEN DATE '1995-01-01' AND DATE '1995-12-31'"},
    {"all rows", "1 = 1"},
};
// orders: by o_orderdate
inline const std::vector<Shape> kOrdersShapes = {
    {"point", "o_orderkey = 1"},
    {"1 day", "o_orderdate = DATE '1995-06-17'"},
    {"1 week", "o_orderdate BETWEEN DATE '1995-06-11' AND DATE '1995-06-17'"},
    {"1 month", "o_orderdate BETWEEN DATE '1995-06-01' AND DATE '1995-06-30'"},
    {"1 year", "o_orderdate BETWEEN DATE '1995-01-01' AND DATE '1995-12-31'"},
    {"all rows", "1 = 1"},
};
// partsupp: by ps_partkey
inline const std::vector<Shape> kPartsuppShapes = {
    {"point", "ps_partkey = 1"},
    {"10 keys", "ps_partkey <= 10"},
    {"100 keys", "ps_partkey <= 100"},
    {"1000 keys", "ps_partkey <= 1000"},
    {"10000 keys", "ps_partkey <= 10000"},
    {"all rows", "1 = 1"},
};
// part: by p_partkey
inline const std::vector<Shape> kPartShapes = {
    {"point", "p_partkey = 1"},
    {"10 keys", "p_partkey <= 10"},
    {"100 keys", "p_partkey <= 100"},
    {"1000 keys", "p_partkey <= 1000"},
    {"10000 keys", "p_partkey <= 10000"},
    {"all rows", "1 = 1"},
};
// customer: by c_custkey
inline const std::vector<Shape> kCustomerShapes = {
    {"point", "c_custkey = 1"},
    {"10 keys", "c_custkey <= 10"},
    {"100 keys", "c_custkey <= 100"},
    {"1000 keys", "c_custkey <= 1000"},
    {"10000 keys", "c_custkey <= 10000"},
    {"all rows", "1 = 1"},
};
// supplier: by s_suppkey
inline const std::vector<Shape> kSupplierShapes = {
    {"point", "s_suppkey = 1"},
    {"10 keys", "s_suppkey <= 10"},
    {"100 keys", "s_suppkey <= 100"},
    {"1000 keys", "s_suppkey <= 1000"},
    {"10000 keys", "s_suppkey <= 10000"},
    {"all rows", "1 = 1"},
};

} // namespace accelerator_codegen::aux_state::query::calibration
