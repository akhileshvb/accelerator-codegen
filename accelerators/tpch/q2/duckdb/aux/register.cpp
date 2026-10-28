// GENERATED QUERY: TPC-H Q2 aux state on DuckDB.
// Registers the catalog entry (SQL, probes, benchmark arguments), the
// table function accel_tpch_q2_aux and the module's binding: its config, the
// tables engine capture watches, and the InitialScan over DuckDB SQL.
#include "tpch/q2/duckdb/aux/module.hpp"
#include "tpch/q2/cost_model.hpp"
#include "tpch/q2/layout.hpp"
#include "backends/duckdb/api/aux_query_registry.hpp"
#include "catalog/query_catalog.hpp"

#include <functional>
#include <set>
#include <string>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen::aux_state::engine {
namespace {
namespace q2_catalog {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace catalog_values;

constexpr query::TpchQ2Config kTables{1, 2, 3, 4, 5};

SharedProbe BuildTpchQ2Probe(::duckdb::MaterializedQueryResult &result,
                              ::duckdb::idx_t row) {
  // Lower a *non*-winning EUROPE BRASS partsupp below the regional min so
  // ApplyDelta can install a new min. Raising the unique min forces
  // lost_unique_min → RebuildAll (next-best cost is not stored on the cell).
  const auto partkey = result.GetValue(0, row).ToString();
  const auto suppkey = result.GetValue(1, row).ToString();
  const double cost = std::stod(result.GetValue(2, row).ToString());
  const int64_t cost_raw = DecimalRaw(result.GetValue(2, row));
  const int64_t min_raw = DecimalRaw(result.GetValue(3, row));
  const int64_t new_raw = min_raw - 1;
  const double new_cost = static_cast<double>(new_raw) / 100.0;
  SharedProbe probe;
  probe.table = kTables.partsupp_table;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {2};
  probe.write_delta = new_cost - cost;
  probe.current_value = cost;
  probe.old_row = {{Int(result.GetValue(0, row)),
                    Int(result.GetValue(1, row)),
                    Decimal(cost_raw, 2)}};
  probe.new_row = probe.old_row;
  probe.new_row.values[2] = Decimal(new_raw, 2);
  probe.update_sql = "UPDATE partsupp SET ps_supplycost = " +
                     std::to_string(new_cost) + " WHERE ps_partkey = " +
                     partkey + " AND ps_suppkey = " +
                     suppkey;
  probe.restore_sql = "UPDATE partsupp SET ps_supplycost = " + std::to_string(cost) +
                      " WHERE ps_partkey = " + partkey + " AND ps_suppkey = " +
                      suppkey;
  probe.repeats = true;
  return probe;
}

const SharedQueryRegistrar kEntry{SharedQueryCatalogEntry{2,
     "tpch",
     core::TpchQ2Layout::kAccelerator,
     "SELECT s_acctbal, s_name, n_name, p_partkey, p_mfgr, s_address, s_phone, s_comment "
     "FROM part, supplier, partsupp, nation, region "
     "WHERE p_partkey = ps_partkey AND s_suppkey = ps_suppkey AND p_size = 15 "
     "AND p_type LIKE '%BRASS' AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey "
     "AND r_name = 'EUROPE' "
     "AND ps_supplycost = (SELECT MIN(ps_supplycost) FROM partsupp, supplier, nation, region "
     "WHERE p_partkey = ps_partkey AND s_suppkey = ps_suppkey AND s_nationkey = n_nationkey "
     "AND n_regionkey = r_regionkey AND r_name = 'EUROPE') "
     "ORDER BY s_acctbal DESC, n_name, s_name, p_partkey LIMIT 100",
     "SELECT * FROM accel_tpch_q2_aux(15, '%BRASS', 'EUROPE')",
     "SELECT * FROM accel_tpch_q2(15, '%BRASS', 'EUROPE')",
     "SELECT * FROM accel_tpch_q2_aux(15, '%BRASS', 'EUROPE')",
     "SELECT ps.ps_partkey, ps.ps_suppkey, ps.ps_supplycost, mins.min_cost "
     "FROM partsupp ps "
     "JOIN supplier s ON s.s_suppkey = ps.ps_suppkey "
     "JOIN nation n ON n.n_nationkey = s.s_nationkey "
     "JOIN region r ON r.r_regionkey = n.n_regionkey "
     "JOIN part p ON p.p_partkey = ps.ps_partkey "
     "JOIN LATERAL ("
     "  SELECT MIN(ps2.ps_supplycost) AS min_cost "
     "  FROM partsupp ps2 "
     "  JOIN supplier s2 ON s2.s_suppkey = ps2.ps_suppkey "
     "  JOIN nation n2 ON n2.n_nationkey = s2.s_nationkey "
     "  JOIN region r2 ON r2.r_regionkey = n2.n_regionkey "
     "  WHERE ps2.ps_partkey = ps.ps_partkey AND r2.r_name = r.r_name"
     ") mins ON true "
     "WHERE r.r_name = 'EUROPE' AND p.p_size = 15 AND p.p_type LIKE '%BRASS' "
     "AND ps.ps_supplycost > mins.min_cost "
     "ORDER BY ps.ps_partkey, ps.ps_suppkey LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::Int64, static_cast<int64_t>(15)),
       core::AuxValue::Of(core::AuxType::String, std::string{"%BRASS"}),
       core::AuxValue::Of(core::AuxType::String, std::string{"EUROPE"})}},
     3,
     false,
     BuildTpchQ2Probe,
     core::kTpchQ2CostModel,
     "SELECT s_acctbal, s_name, n_name, p_partkey, p_mfgr, s_address, s_phone, s_comment "
     "FROM part, supplier, partsupp, nation, region "
     "WHERE p_partkey = ps_partkey AND s_suppkey = ps_suppkey AND p_size = $1 "
     "AND p_type LIKE $2 AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey "
     "AND r_name = $3 "
     "AND ps_supplycost = (SELECT MIN(ps_supplycost) FROM partsupp, supplier, nation, region "
     "WHERE p_partkey = ps_partkey AND s_suppkey = ps_suppkey "
     "AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey AND r_name = $3) "
     "ORDER BY s_acctbal DESC, n_name, s_name, p_partkey LIMIT 100"}};

} // namespace q2_catalog
} // namespace
} // namespace accelerator_codegen::aux_state::engine

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace accelerator_codegen::aux_state::query;
using namespace scan;

struct TpchQ2Scan : TpchQ2InitialScan {
  explicit TpchQ2Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanNationsSerial(
      const std::function<void(const TpchQ2NationInput &)> &consumer) override {
    ScanQuery(connection, "SELECT n_nationkey, n_name, n_regionkey FROM nation",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({SafeInt32(result.GetValue(0, row)),
                          result.GetValue(1, row).ToString(),
                          SafeInt32(result.GetValue(2, row))});
              });
  }
  void ScanRegionsSerial(
      const std::function<void(const TpchQ2RegionInput &)> &consumer) override {
    ScanQuery(connection, "SELECT r_regionkey, r_name FROM region",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({SafeInt32(result.GetValue(0, row)),
                          result.GetValue(1, row).ToString()});
              });
  }
  void ScanTable(const core::AuxEngineServices &services, const std::string &table,
                 const std::vector<BuildColumn> &columns, std::size_t partitions,
                 const Sink &sink) override {
    ScanPartitioned(services, connection, table, columns, partitions, sink);
  }
  ::duckdb::Connection &connection;
};

const AuxQueryRegistrar kQuery{AuxQuery{
    core::TpchQ2Layout::kAccelerator,
    "accel_tpch_q2_aux",
    {::duckdb::LogicalType::INTEGER, ::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::VARCHAR},
    [](::duckdb::Connection &connection) {
      const TpchQ2Config c = q2_catalog::kTables;
      return MakeBinding<TpchQ2Scan, TpchQ2Module>(connection, CaptureTableList{{"part", c.part_table}, {"supplier", c.supplier_table}, {"partsupp", c.partsupp_table}, {"nation", c.nation_table}, {"region", c.region_table}}, c);
    }}};

} // namespace
} // namespace accelerator_codegen::aux_state::engine::duckdb
