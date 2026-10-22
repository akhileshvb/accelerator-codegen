// GENERATED QUERY: TPC-H Q16 aux state on DuckDB.
// Registers the catalog entry (SQL, probes, benchmark arguments), the
// table function accel_tpch_q16_aux and the module's binding: its config, the
// tables engine capture watches, and the InitialScan over DuckDB SQL.
#include "tpch/q16/duckdb/aux/module.hpp"
#include "tpch/q16/cost_model.hpp"
#include "backends/duckdb/api/aux_query_registry.hpp"
#include "backends/duckdb/api/build_scan.hpp"
#include "catalog/query_catalog.hpp"

#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen::aux_state::engine {
namespace {
namespace q16_catalog {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace catalog_values;

constexpr core::TableId kPartTable = 1;
constexpr core::TableId kPartsuppTable = 2;
constexpr core::TableId kSupplierTable = 3;

SharedProbe BuildTpchQ16Probe(::duckdb::MaterializedQueryResult &result,
                              ::duckdb::idx_t row) {
  // Flip supplier eligibility (Customer Complaints) so ApplyDelta can run.
  // partsupp membership changes still MarkRebuild; this path exercises AD.
  const auto suppkey = result.GetValue(0, row).ToString();
  const auto comment = result.GetValue(1, row).ToString();
  SharedProbe probe;
  probe.table = kSupplierTable;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {1};
  probe.write_delta = 1.0;
  probe.current_value = 0.0;
  probe.old_row = {{Int(result.GetValue(0, row)), Text(result.GetValue(1, row))}};
  probe.new_row = probe.old_row;
  probe.new_row.values[1] =
      Text(comment + " Customer Complaints");
  // Escape single quotes for SQL literal.
  std::string escaped = comment;
  for (std::size_t pos = 0; (pos = escaped.find('\'', pos)) != std::string::npos;
       pos += 2) {
    escaped.insert(pos, "'");
  }
  probe.update_sql =
      "UPDATE supplier SET s_comment = s_comment || ' Customer Complaints' "
      "WHERE s_suppkey = " +
      suppkey;
  probe.restore_sql = "UPDATE supplier SET s_comment = '" + escaped +
                      "' WHERE s_suppkey = " + suppkey;
  return probe;
}

const SharedQueryRegistrar kEntry{SharedQueryCatalogEntry{16,
     SharedWorkloadKind::Tpch,
     "tpch",
     query::TpchQ16Layout::kAccelerator,
     "SELECT p_brand, p_type, p_size, COUNT(DISTINCT ps_suppkey) AS supplier_cnt "
     "FROM partsupp, part "
     "WHERE p_partkey = ps_partkey AND p_brand <> 'Brand#13' "
     "AND p_type NOT LIKE 'SMALL BRUSHED%' "
     "AND p_size IN (26, 24, 27, 29, 7, 20, 22, 33) "
     "AND ps_suppkey NOT IN ("
     "SELECT s_suppkey FROM supplier WHERE s_comment LIKE '%Customer%Complaints%') "
     "GROUP BY p_brand, p_type, p_size "
     "ORDER BY supplier_cnt DESC, p_brand, p_type, p_size",
     "SELECT * FROM accel_tpch_q16_aux('Brand#13', 'SMALL BRUSHED%', 26, 24, 27, 29, 7, 20, 22, 33)",
     "SELECT * FROM accel_tpch_q16('Brand#13', 'SMALL BRUSHED%', 26, 24, 27, 29, 7, 20, 22, 33)",
     "SELECT * FROM accel_tpch_q16_aux('Brand#13', 'SMALL BRUSHED%', 26, 24, 27, 29, 7, 20, 22, 33)",
     // Flip eligibility on a supplier that contributes under catalog filters.
     "SELECT s.s_suppkey, s.s_comment FROM supplier s "
     "WHERE s.s_comment NOT LIKE '%Customer%Complaints%' "
     "AND EXISTS (SELECT 1 FROM partsupp ps, part p "
     "WHERE ps.ps_suppkey = s.s_suppkey AND p.p_partkey = ps.ps_partkey "
     "AND p.p_brand <> 'Brand#13' AND p.p_type NOT LIKE 'SMALL BRUSHED%' "
     "AND p.p_size IN (26, 24, 27, 29, 7, 20, 22, 33)) "
     "ORDER BY s.s_suppkey LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::String, std::string{"Brand#13"}),
       core::AuxValue::Of(core::AuxType::String, std::string{"SMALL BRUSHED%"}),
       core::AuxValue::Of(core::AuxType::Int64, int64_t{26}),
       core::AuxValue::Of(core::AuxType::Int64, int64_t{24}),
       core::AuxValue::Of(core::AuxType::Int64, int64_t{27}),
       core::AuxValue::Of(core::AuxType::Int64, int64_t{29}),
       core::AuxValue::Of(core::AuxType::Int64, int64_t{7}),
       core::AuxValue::Of(core::AuxType::Int64, int64_t{20}),
       core::AuxValue::Of(core::AuxType::Int64, int64_t{22}),
       core::AuxValue::Of(core::AuxType::Int64, int64_t{33})}},
     3,
     false,
     BuildTpchQ16Probe,
     core::kTpchQ16CostModel,
     "SELECT p_brand, p_type, p_size, COUNT(DISTINCT ps_suppkey) AS supplier_cnt "
     "FROM partsupp, part "
     "WHERE p_partkey = ps_partkey AND p_brand <> $1 AND p_type NOT LIKE $2 "
     "AND p_size IN ($3, $4, $5, $6, $7, $8, $9, $10) "
     "AND ps_suppkey NOT IN ( "
     "SELECT s_suppkey FROM supplier WHERE s_comment LIKE '%Customer%Complaints%') "
     "GROUP BY p_brand, p_type, p_size "
     "ORDER BY supplier_cnt DESC, p_brand, p_type, p_size"}};

} // namespace q16_catalog
} // namespace
} // namespace accelerator_codegen::aux_state::engine

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace accelerator_codegen::aux_state::query;
using namespace scan;

struct TpchQ16Scan : TpchQ16InitialScan {
  explicit TpchQ16Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanSuppliers(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ16SupplierInput &)> &sink) override {
    ScanPartitioned(services, connection, "supplier",
                    {IntColumn("s_suppkey"), TextColumn("s_comment")}, partitions,
                    [&](std::size_t partition, const ChunkBatch &batch) {
                      for (std::size_t i = 0; i < batch.size(); ++i) {
                        const auto row = batch.Row(i);
                        sink(partition, TpchQ16SupplierInput{row.Int(0), row.Text(1)});
                      }
                    });
  }
  void ScanParts(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ16PartInput &)> &sink) override {
    ScanPartitioned(services, connection, "part",
                    {IntColumn("p_partkey"), TextColumn("p_brand"), TextColumn("p_type"),
                     IntColumn("p_size")},
                    partitions, [&](std::size_t partition, const ChunkBatch &batch) {
                      for (std::size_t i = 0; i < batch.size(); ++i) {
                        const auto row = batch.Row(i);
                        const auto size = row.Int(3);
                        if (size != static_cast<int32_t>(size))
                          throw std::runtime_error("TPC-H Q16: p_size exceeds 32 bits");
                        sink(partition, TpchQ16PartInput{row.Int(0), row.Text(1), row.Text(2),
                                                         static_cast<int32_t>(size)});
                      }
                    });
  }
  void ScanPartsupp(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ16PartsuppInput &)> &sink) override {
    ScanPartitioned(services, connection, "partsupp",
                    {IntColumn("ps_partkey"), IntColumn("ps_suppkey")}, partitions,
                    [&](std::size_t partition, const ChunkBatch &batch) {
                      for (std::size_t i = 0; i < batch.size(); ++i) {
                        const auto row = batch.Row(i);
                        sink(partition, TpchQ16PartsuppInput{row.Int(0), row.Int(1)});
                      }
                    });
  }
  ::duckdb::Connection &connection;
};

const AuxQueryRegistrar kQuery{AuxQuery{
    query::TpchQ16Layout::kAccelerator,
    "accel_tpch_q16_aux",
    {::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::INTEGER, ::duckdb::LogicalType::INTEGER, ::duckdb::LogicalType::INTEGER, ::duckdb::LogicalType::INTEGER, ::duckdb::LogicalType::INTEGER, ::duckdb::LogicalType::INTEGER, ::duckdb::LogicalType::INTEGER, ::duckdb::LogicalType::INTEGER},
    [](::duckdb::Connection &connection) {
      const TpchQ16Config c{q16_catalog::kPartTable, q16_catalog::kPartsuppTable, q16_catalog::kSupplierTable};
      return MakeBinding<TpchQ16Scan, TpchQ16Module>(connection, CaptureTableList{{"part", c.part_table}, {"partsupp", c.partsupp_table}, {"supplier", c.supplier_table}}, c);
    }}};

} // namespace
} // namespace accelerator_codegen::aux_state::engine::duckdb
