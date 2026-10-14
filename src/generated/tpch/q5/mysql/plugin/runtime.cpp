// MySQL plugin runtime for TPC-H Q5: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// src/generated/mysql/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see src/generated/mysql/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "mysql/runtime.hpp"

#include "tpch/mysql_table_ids.hpp"
#include "tpch/q5/mysql/aux/module.hpp"
#include "tpch/q5/cost_model.hpp"
#include "tpch/q5/layout.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
namespace mysql_tbl = accelerator_codegen::aux_state::engine::mysql::tpch_tables;
using namespace detail;

namespace scans {

void ScanTpchQ5Regions(
    const std::function<void(const query::TpchQ5RegionInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT r_regionkey, r_name FROM region "
      "WHERE r_regionkey IS NOT NULL AND r_name IS NOT NULL",
      [&](const std::vector<std::string> &cols) {
        consumer({static_cast<int32_t>(TypedInt(cols, 0)),
                  TypedText(cols, 1)});
      });
}

void ScanTpchQ5Nations(
    const std::function<void(const query::TpchQ5NationInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT n_nationkey, n_name, n_regionkey, "
      "CASE WHEN n_name IS NULL THEN 1 ELSE 0 END, "
      "CASE WHEN n_regionkey IS NULL THEN 1 ELSE 0 END "
      "FROM nation WHERE n_nationkey IS NOT NULL",
      [&](const std::vector<std::string> &cols) {
        consumer({static_cast<int32_t>(TypedInt(cols, 0)),
                  TypedText(cols, 1),
                  static_cast<int32_t>(TypedInt(cols, 2)),
                  TypedInt(cols, 3) != 0,
                  TypedInt(cols, 4) != 0});
      });
}

void ScanTpchQ5Customers(
    const std::function<void(const query::TpchQ5CustomerInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT c_custkey, c_nationkey FROM customer "
      "WHERE c_custkey IS NOT NULL AND c_nationkey IS NOT NULL",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  static_cast<int32_t>(TypedInt(cols, 1))});
      });
}

void ScanTpchQ5Suppliers(
    const std::function<void(const query::TpchQ5SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_nationkey FROM supplier "
      "WHERE s_suppkey IS NOT NULL AND s_nationkey IS NOT NULL",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  static_cast<int32_t>(TypedInt(cols, 1))});
      });
}

// Orders that can join: NULL custkey / orderdate never reach the revenue sum.
const char kOrdersSql[] =
    "SELECT o_orderkey, o_custkey, o_orderdate FROM orders "
    "WHERE o_orderkey IS NOT NULL AND o_custkey IS NOT NULL "
    "AND o_orderdate IS NOT NULL";

void ScanTpchQ5Orders(
    const std::function<void(const query::TpchQ5OrderInput &)> &consumer) {
  ScanTypedTuples(
      kOrdersSql,
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedInt(cols, 1),
                  TypedDate(cols, 2)});
      });
}

// Lineitems that reach the SUM; price and discount as exact cents.
const char kLineitemsSql[] =
    "SELECT l_orderkey, l_suppkey, CAST(l_extendedprice * 100 AS SIGNED), "
    "CAST(l_discount * 100 AS SIGNED) FROM lineitem "
    "WHERE l_orderkey IS NOT NULL AND l_suppkey IS NOT NULL "
    "AND l_extendedprice IS NOT NULL AND l_discount IS NOT NULL";

void ScanTpchQ5Lineitems(
    const std::function<void(const query::TpchQ5LineitemInput &)> &consumer) {
  ScanTypedTuples(
      kLineitemsSql,
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedInt(cols, 1),
                  TypedInt(cols, 2), 2,
                  TypedInt(cols, 3), 2});
      });
}

// Index lookups (keys go in batches).
template <typename Consumer>
void LookupBatches(const std::string &base_sql, const char *key_column,
                   const std::vector<int64_t> &keys, Consumer &&scan_batch) {
  constexpr std::size_t kBatch = 4096;
  for (std::size_t from = 0; from < keys.size(); from += kBatch) {
    const std::vector<int64_t> batch(
        keys.begin() + from,
        keys.begin() + std::min(keys.size(), from + kBatch));
    scan_batch(base_sql + " AND " + KeyListPredicate(key_column, batch));
  }
}

void LookupTpchQ5Lineitems(
    const char *key_column, const std::vector<int64_t> &keys,
    const std::function<void(const query::TpchQ5LineitemInput &)> &consumer) {
  LookupBatches(kLineitemsSql, key_column, keys, [&](const std::string &sql) {
    ScanTypedTuples(sql, [&](const std::vector<std::string> &cols) {
      consumer({TypedInt(cols, 0), TypedInt(cols, 1), TypedInt(cols, 2), 2,
                TypedInt(cols, 3), 2});
    });
  });
}

void LookupTpchQ5OrdersByCustomer(
    const std::vector<int64_t> &custkeys,
    const std::function<void(const query::TpchQ5OrderInput &)> &consumer) {
  LookupBatches(kOrdersSql, "o_custkey", custkeys, [&](const std::string &sql) {
    ScanTypedTuples(sql, [&](const std::vector<std::string> &cols) {
      consumer({TypedInt(cols, 0), TypedInt(cols, 1), TypedDate(cols, 2)});
    });
  });
}

} // namespace scans

class TpchQ5Scan final : public query::TpchQ5InitialScan {
public:
  void ScanRegions(
      const std::function<void(const query::TpchQ5RegionInput &)> &consumer)
      override {
    scans::ScanTpchQ5Regions(consumer);
  }
  void ScanNations(
      const std::function<void(const query::TpchQ5NationInput &)> &consumer)
      override {
    scans::ScanTpchQ5Nations(consumer);
  }
  void ScanCustomers(
      const std::function<void(const query::TpchQ5CustomerInput &)> &consumer)
      override {
    scans::ScanTpchQ5Customers(consumer);
  }
  void ScanSuppliers(
      const std::function<void(const query::TpchQ5SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ5Suppliers(consumer);
  }
  void ScanOrders(
      const std::function<void(const query::TpchQ5OrderInput &)> &consumer)
      override {
    scans::ScanTpchQ5Orders(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ5LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ5Lineitems(consumer);
  }
  void LookupLineitemsByOrder(
      const std::vector<int64_t> &orderkeys,
      const std::function<void(const query::TpchQ5LineitemInput &)> &consumer)
      override {
    scans::LookupTpchQ5Lineitems("l_orderkey", orderkeys, consumer);
  }
  void LookupLineitemsBySupplier(
      const std::vector<int64_t> &suppkeys,
      const std::function<void(const query::TpchQ5LineitemInput &)> &consumer)
      override {
    scans::LookupTpchQ5Lineitems("l_suppkey", suppkeys, consumer);
  }
  void LookupOrdersByCustomer(
      const std::vector<int64_t> &custkeys,
      const std::function<void(const query::TpchQ5OrderInput &)> &consumer)
      override {
    scans::LookupTpchQ5OrdersByCustomer(custkeys, consumer);
  }
};

Runtime<query::TpchQ5Module, query::TpchQ5Config, TpchQ5Scan> &Q5Runtime() {
  static Runtime<query::TpchQ5Module, query::TpchQ5Config, TpchQ5Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q5_COST_MODEL",
              core::kTpchQ5CostModel, core::TpchQ5Layout::kAccelerator,
              query::TpchQ5Config{mysql_tbl::kRegion, mysql_tbl::kNation, mysql_tbl::kCustomer, mysql_tbl::kSupplier, mysql_tbl::kOrders, mysql_tbl::kLineitem});
  return runtime;
}

std::string ExecuteQuery(const core::QueryArguments &arguments) {
  std::string out;
  Q5Runtime().Execute(arguments, out);
  return out;
}

} // namespace

static const MysqlUdfRegistrar kRegisterQ5Udf(MysqlUdfEntry{
    "mysql_accel_tpch_q5_aux", "sd", &ExecuteQuery});

} // namespace accelerator_codegen::aux_state::engine::mysql

#endif
