// GENERATED QUERY: TPC-H Q19 aux state on DuckDB.
// Registers the catalog entry (SQL, probes, benchmark arguments), the
// table function accel_tpch_q19_aux and the module's binding: its config, the
// tables engine capture watches, and the InitialScan over DuckDB SQL.
#include "tpch/q19/duckdb/aux/module.hpp"
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
namespace q19_catalog {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace catalog_values;

SharedProbe BuildTpchQ19Probe(::duckdb::MaterializedQueryResult &result,
                              ::duckdb::idx_t row) {
  // Select columns: orderkey, partkey, linenumber, qty, price, discount,
  // shipmode, shipinstruct. table=5 matches TpchQ19Config{5,6}.lineitem_table.
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto partkey = result.GetValue(1, row).ToString();
  const auto linenumber = result.GetValue(2, row).ToString();
  double quantity = 0.0;
  try {
    quantity = std::stod(result.GetValue(3, row).ToString());
  } catch (const std::exception &) {
    quantity = static_cast<double>(DecimalRaw(result.GetValue(3, row))) / 100.0;
  }
  SharedProbe probe;
  probe.table = 5;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {1};
  probe.write_delta = 100.0;
  probe.current_value = quantity;
  probe.old_row = {{Int(result.GetValue(1, row)),
                    Decimal(DecimalRaw(result.GetValue(3, row)), 2),
                    Decimal(DecimalRaw(result.GetValue(4, row)), 2),
                    Decimal(DecimalRaw(result.GetValue(5, row)), 2),
                    Text(result.GetValue(6, row)), Text(result.GetValue(7, row))}};
  probe.new_row = probe.old_row;
  probe.new_row.values[1] = Decimal(
      static_cast<int64_t>(DecimalRaw(result.GetValue(3, row)) + 10000), 2);
  probe.update_sql = "UPDATE lineitem SET l_quantity = l_quantity + 100"
                     " WHERE l_orderkey = " + orderkey +
                     " AND l_partkey = " + partkey +
                     " AND l_linenumber = " + linenumber;
  probe.restore_sql = "UPDATE lineitem SET l_quantity = " +
                      std::to_string(quantity) + " WHERE l_orderkey = " +
                      orderkey + " AND l_partkey = " + partkey +
                      " AND l_linenumber = " + linenumber;
  return probe;
}

const SharedQueryRegistrar kEntry{SharedQueryCatalogEntry{19,
     SharedWorkloadKind::Tpch,
     "tpch",
     query::TpchQ19Layout::kAccelerator,
     "SELECT SUM(l_extendedprice * (1 - l_discount)) AS revenue "
     "FROM lineitem, part "
     "WHERE (p_partkey = l_partkey "
     "AND p_brand = 'Brand#31' "
     "AND p_container IN ('SM CASE', 'SM BOX', 'SM PACK', 'SM PKG') "
     "AND l_quantity >= 4 AND l_quantity <= 4 + 10 "
     "AND p_size BETWEEN 1 AND 5 "
     "AND l_shipmode IN ('AIR', 'AIR REG') "
     "AND l_shipinstruct = 'DELIVER IN PERSON') "
     "OR (p_partkey = l_partkey "
     "AND p_brand = 'Brand#12' "
     "AND p_container IN ('MED BAG', 'MED BOX', 'MED PKG', 'MED PACK') "
     "AND l_quantity >= 15 AND l_quantity <= 15 + 10 "
     "AND p_size BETWEEN 1 AND 10 "
     "AND l_shipmode IN ('AIR', 'AIR REG') "
     "AND l_shipinstruct = 'DELIVER IN PERSON') "
     "OR (p_partkey = l_partkey "
     "AND p_brand = 'Brand#51' "
     "AND p_container IN ('LG CASE', 'LG BOX', 'LG PACK', 'LG PKG') "
     "AND l_quantity >= 23 AND l_quantity <= 23 + 10 "
     "AND p_size BETWEEN 1 AND 15 "
     "AND l_shipmode IN ('AIR', 'AIR REG') "
     "AND l_shipinstruct = 'DELIVER IN PERSON')",
     "SELECT * FROM accel_tpch_q19_aux('Brand#31', 4, 'Brand#12', 15, 'Brand#51', 23)",
     "SELECT * FROM accel_tpch_q19('Brand#31', 4, 'Brand#12', 15, 'Brand#51', 23)",
     "SELECT * FROM accel_tpch_q19_aux('Brand#31', 4, 'Brand#12', 15, 'Brand#51', 23)",
     "SELECT l.l_orderkey, l.l_partkey, l.l_linenumber, l.l_quantity, "
     "l.l_extendedprice, l.l_discount, l.l_shipmode, l.l_shipinstruct "
     "FROM lineitem l, part p "
     "WHERE p.p_partkey = l.l_partkey AND p.p_brand = 'Brand#31' "
     "AND p.p_container IN ('SM CASE', 'SM BOX', 'SM PACK', 'SM PKG') "
     "AND l.l_quantity >= 4 AND l.l_quantity <= 14 "
     "AND p.p_size BETWEEN 1 AND 5 "
     "AND l.l_shipmode IN ('AIR', 'AIR REG') "
     "AND l.l_shipinstruct = 'DELIVER IN PERSON' "
     "ORDER BY l.l_orderkey, l.l_linenumber LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::String, std::string{"Brand#31"}),
       core::AuxValue::Of(core::AuxType::Int64, int64_t{4}),
       core::AuxValue::Of(core::AuxType::String, std::string{"Brand#12"}),
       core::AuxValue::Of(core::AuxType::Int64, int64_t{15}),
       core::AuxValue::Of(core::AuxType::String, std::string{"Brand#51"}),
       core::AuxValue::Of(core::AuxType::Int64, int64_t{23})}},
     5,
     false,
     BuildTpchQ19Probe}};

} // namespace q19_catalog
} // namespace
} // namespace accelerator_codegen::aux_state::engine

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace accelerator_codegen::aux_state::query;
using namespace scan;

// Typed streaming scan: reads DuckDB vectors directly (no boxed Values).
template <typename T> struct TypedCol {
  ::duckdb::UnifiedVectorFormat fmt;
  const T *data = nullptr;
  void Init(::duckdb::DataChunk &chunk, ::duckdb::idx_t col) {
    chunk.data[col].ToUnifiedFormat(chunk.size(), fmt);
    data = ::duckdb::UnifiedVectorFormat::GetData<T>(fmt);
  }
  bool Valid(::duckdb::idx_t row) const {
    return fmt.validity.RowIsValid(fmt.sel->get_index(row));
  }
  const T &operator()(::duckdb::idx_t row) const {
    return data[fmt.sel->get_index(row)];
  }
};

template <typename Callback>
void ScanTypedChunks(::duckdb::Connection &connection_p, const std::string &sql,
                     Callback &&callback) {
  auto &connection = ScanConnection(connection_p);
  auto result = connection.SendQuery(sql);
  if (!result || result->HasError())
    throw std::runtime_error(result ? result->GetError()
                                    : "DuckDB scan returned null");
  while (true) {
    auto chunk = result->Fetch();
    if (!chunk || chunk->size() == 0)
      break;
    callback(*chunk);
  }
  if (result->HasError())
    throw std::runtime_error(result->GetError());
}

struct TpchQ19Scan : TpchQ19InitialScan {
  explicit TpchQ19Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanParts(
      const std::function<void(const TpchQ19PartInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT p_partkey, p_brand, p_container, p_size FROM part",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString(),
                    ChunkValue(chunk, 2, row).ToString(),
                    static_cast<int32_t>(SafeInt64(ChunkValue(chunk, 3, row)))});
        });
  }
  void ScanLineitems(
      const std::function<void(const TpchQ19LineitemInput &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT l_partkey, l_quantity, l_extendedprice, l_discount, l_shipmode, l_shipinstruct FROM lineitem",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    DecimalRaw(ChunkValue(chunk, 1, row)),
                    DecimalRaw(ChunkValue(chunk, 2, row)),
                    DecimalRaw(ChunkValue(chunk, 3, row)),
                    ChunkValue(chunk, 4, row).ToString(),
                    ChunkValue(chunk, 5, row).ToString()});
        });
  }
  static std::string FamilyCase() {
    return "CASE WHEN p_container IN ('SM CASE','SM BOX','SM PACK','SM PKG') "
           "AND p_size BETWEEN 1 AND 5 THEN 1 "
           "WHEN p_container IN ('MED BAG','MED BOX','MED PKG','MED PACK') "
           "AND p_size BETWEEN 1 AND 10 THEN 2 "
           "WHEN p_container IN ('LG CASE','LG BOX','LG PACK','LG PKG') "
           "AND p_size BETWEEN 1 AND 15 THEN 3 ELSE 0 END";
  }
  bool ScanQualifyingAggregates(
      const std::function<void(const std::string &, int, int64_t, int64_t,
                               int64_t)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT p.b, p.f, CAST(round(l.l_quantity * 100) AS BIGINT) AS q, "
        "CAST(SUM(CAST(round(l.l_extendedprice * 100) AS BIGINT) * "
        "(100 - CAST(round(l.l_discount * 100) AS BIGINT))) AS BIGINT), "
        "COUNT(*) FROM lineitem l JOIN (SELECT * FROM (SELECT p_partkey, "
        "p_brand AS b, " + FamilyCase() + " AS f FROM part) WHERE f > 0) p "
        "ON l.l_partkey = p.p_partkey "
        "WHERE l.l_shipmode IN ('AIR', 'AIR REG') "
        "AND l.l_shipinstruct = 'DELIVER IN PERSON' GROUP BY 1, 2, 3",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer(ChunkValue(chunk, 0, row).ToString(),
                   static_cast<int>(SafeInt64(ChunkValue(chunk, 1, row))),
                   SafeInt64(ChunkValue(chunk, 2, row)),
                   SafeInt64(ChunkValue(chunk, 3, row)),
                   SafeInt64(ChunkValue(chunk, 4, row)));
        });
    return true;
  }
  bool ScanQualifyingLineitemsByPart(
      const std::function<void(int64_t, int64_t, int64_t, int64_t)> &consumer)
      override {
    ScanTypedChunks(
        connection,
        "SELECT CAST(l_partkey AS BIGINT), "
        "CAST(round(l_quantity * 100) AS BIGINT), "
        "CAST(SUM(CAST(round(l_extendedprice * 100) AS BIGINT) * "
        "(100 - CAST(round(l_discount * 100) AS BIGINT))) AS BIGINT), "
        "COUNT(*) FROM lineitem "
        "WHERE l_shipmode IN ('AIR', 'AIR REG') "
        "AND l_shipinstruct = 'DELIVER IN PERSON' "
        "GROUP BY l_partkey, l_quantity ORDER BY l_partkey",
        [&](::duckdb::DataChunk &chunk) {
          TypedCol<int64_t> pk, qty, rev, cnt;
          pk.Init(chunk, 0);
          qty.Init(chunk, 1);
          rev.Init(chunk, 2);
          cnt.Init(chunk, 3);
          for (::duckdb::idx_t i = 0; i < chunk.size(); ++i) {
            if (!pk.Valid(i) || !qty.Valid(i))
              continue;
            consumer(pk(i), qty(i), rev.Valid(i) ? rev(i) : 0, cnt(i));
          }
        });
    return true;
  }
  bool ScanPartFamilies(
      const std::function<void(int64_t, const std::string &, int)> &consumer)
      override {
    ScanQueryStreaming(
        connection,
        "SELECT p_partkey, p_brand, " + FamilyCase() + " FROM part",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer(SafeInt64(ChunkValue(chunk, 0, row)),
                   ChunkValue(chunk, 1, row).ToString(),
                   static_cast<int>(SafeInt64(ChunkValue(chunk, 2, row))));
        });
    return true;
  }
  bool LookupPart(int64_t partkey, TpchQ19PartInput &out) override {
    auto result = ScanConnection(connection).Query(
        "SELECT p_brand, p_container, p_size FROM part WHERE p_partkey = " +
        std::to_string(partkey));
    if (!result || result->HasError() || result->RowCount() == 0)
      return false;
    out.key = partkey;
    out.brand = result->GetValue(0, 0).ToString();
    out.container = result->GetValue(1, 0).ToString();
    out.size = static_cast<int32_t>(SafeInt64(result->GetValue(2, 0)));
    return true;
  }
  ::duckdb::Connection &connection;
};

const AuxQueryRegistrar kQuery{AuxQuery{
    query::TpchQ19Layout::kAccelerator,
    "accel_tpch_q19_aux",
    {::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::BIGINT, ::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::BIGINT, ::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::BIGINT},
    [](::duckdb::Connection &connection) {
      const TpchQ19Config c{5, 6};
      return MakeBinding<TpchQ19Scan, TpchQ19Module>(connection, CaptureTableList{{"lineitem", c.lineitem_table}, {"part", c.part_table}}, c);
    }}};

} // namespace
} // namespace accelerator_codegen::aux_state::engine::duckdb
