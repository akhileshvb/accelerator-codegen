// GENERATED QUERY: TPC-H Q11 aux state on DuckDB.
// Registers the catalog entry (SQL, probes, benchmark arguments), the
// table function accel_tpch_q11_aux and the module's binding: its config, the
// tables engine capture watches, and the InitialScan over DuckDB SQL.
#include "tpch/q11/duckdb/aux/module.hpp"
#include "accelerator_codegen/runtime/duckdb/aux_query_registry.hpp"
#include "catalog/query_catalog.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen::aux_state::engine {
namespace {
namespace q11_catalog {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace catalog_values;

SharedProbe BuildTpchQ11Probe(::duckdb::MaterializedQueryResult &result,
                              ::duckdb::idx_t row) {
  // Aux row: partkey, suppkey, availqty, supplycost (TpchQ11Config partsupp=1).
  const auto partkey = result.GetValue(0, row).ToString();
  const auto suppkey = result.GetValue(1, row).ToString();
  const int64_t supplycost_raw = DecimalRaw(result.GetValue(3, row));
  SharedProbe probe;
  probe.table = 1;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {3};
  probe.write_delta = 0.01;
  probe.current_value = static_cast<double>(supplycost_raw) / 100.0;
  probe.old_row = {{Int(result.GetValue(0, row)), Int(result.GetValue(1, row)),
                    Int(result.GetValue(2, row)), Decimal(supplycost_raw, 2)}};
  probe.new_row = probe.old_row;
  probe.new_row.values[3] = Decimal(supplycost_raw + 1, 2);
  probe.update_sql = "UPDATE partsupp SET ps_supplycost = ps_supplycost + 0.01"
                     " WHERE ps_partkey = " +
                     partkey + " AND ps_suppkey = " + suppkey;
  probe.restore_sql = "UPDATE partsupp SET ps_supplycost = " +
                      std::to_string(static_cast<double>(supplycost_raw) / 100.0) +
                      " WHERE ps_partkey = " + partkey +
                      " AND ps_suppkey = " + suppkey;
  return probe;
}

const SharedQueryRegistrar kEntry{SharedQueryCatalogEntry{11,
     SharedWorkloadKind::Tpch,
     "tpch",
     query::TpchQ11Layout::kAccelerator,
     "SELECT ps_partkey, SUM(ps_supplycost * ps_availqty) AS value "
     "FROM partsupp, supplier, nation "
     "WHERE ps_suppkey = s_suppkey AND s_nationkey = n_nationkey "
     "AND n_name = 'ROMANIA' "
     "GROUP BY ps_partkey "
     "HAVING SUM(ps_supplycost * ps_availqty) > ("
     "SELECT SUM(ps_supplycost * ps_availqty) * 0.0001 "
     "FROM partsupp, supplier, nation "
     "WHERE ps_suppkey = s_suppkey AND s_nationkey = n_nationkey "
     "AND n_name = 'ROMANIA') "
     "ORDER BY value DESC",
     "SELECT * FROM accel_tpch_q11_aux('ROMANIA', 0.0001)",
     "SELECT * FROM accel_tpch_q11('ROMANIA', 0.0001)",
     "SELECT * FROM accel_tpch_q11_aux('ROMANIA', 0.0001)",
     "SELECT ps_partkey, ps_suppkey, ps_availqty, ps_supplycost "
     "FROM partsupp ps, supplier s, nation n "
     "WHERE ps.ps_suppkey = s.s_suppkey AND s.s_nationkey = n.n_nationkey "
     "AND n.n_name = 'ROMANIA' "
     "ORDER BY ps_partkey, ps_suppkey LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::String, std::string{"ROMANIA"}),
       core::AuxValue::Of(core::AuxType::Double, 0.0001)}},
     1,
     false,
     BuildTpchQ11Probe}};

} // namespace q11_catalog
} // namespace
} // namespace accelerator_codegen::aux_state::engine

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace accelerator_codegen::aux_state::query;
using namespace scan;

struct TpchQ11Scan : TpchQ11InitialScan {
  explicit TpchQ11Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanPartsupp(
      const std::function<void(const TpchQ11PartsuppInput &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT ps_partkey, ps_suppkey, ps_availqty, ps_supplycost FROM partsupp",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt64(ChunkValue(chunk, 1, row)),
                    SafeInt64(ChunkValue(chunk, 2, row)),
                    DecimalRaw(ChunkValue(chunk, 3, row))});
        });
  }
  void ScanSuppliers(
      const std::function<void(const TpchQ11SupplierInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT s_suppkey, s_nationkey FROM supplier",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt32(ChunkValue(chunk, 1, row))});
        });
  }
  void ScanNations(
      const std::function<void(const TpchQ11NationInput &)> &consumer) override {
    ScanQuery(connection, "SELECT n_nationkey, n_name FROM nation",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({SafeInt32(result.GetValue(0, row)),
                          result.GetValue(1, row).ToString()});
              });
  }
  ::duckdb::Connection &connection;
};

const AuxQueryRegistrar kQuery{AuxQuery{
    query::TpchQ11Layout::kAccelerator,
    "accel_tpch_q11_aux",
    {::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::DOUBLE},
    [](::duckdb::Connection &connection) {
      const TpchQ11Config c{1, 2, 3};
      return MakeBinding<TpchQ11Scan, TpchQ11Module>(connection, CaptureTableList{{"partsupp", c.partsupp_table}, {"supplier", c.supplier_table}, {"nation", c.nation_table}}, c);
    }}};

} // namespace
} // namespace accelerator_codegen::aux_state::engine::duckdb
