// GENERATED QUERY: TPC-H Q22 aux state on DuckDB.
// Registers the catalog entry (SQL, probes, benchmark arguments), the
// table function accel_tpch_q22_aux and the module's binding: its config, the
// tables engine capture watches, and the InitialScan over DuckDB SQL.
#include "tpch/q22/duckdb/aux/module.hpp"
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
namespace q22_catalog {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace catalog_values;

SharedProbe BuildTpchQ22Probe(::duckdb::MaterializedQueryResult &result,
                              ::duckdb::idx_t row) {
  // Projection: custkey, phone, acctbal. ReadCustomer expects all three.
  // TpchQ22Config{1, 2} → orders_table=1, customer_table=2.
  const auto custkey = result.GetValue(0, row).ToString();
  const double acctbal = std::stod(result.GetValue(2, row).ToString());
  const int64_t bal_raw = DecimalRaw(result.GetValue(2, row));
  SharedProbe probe;
  probe.table = 2;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {2};
  probe.write_delta = 100.0;
  probe.current_value = acctbal;
  probe.old_row = {{Int(result.GetValue(0, row)), Text(result.GetValue(1, row)),
                    Decimal(bal_raw, 2)}};
  probe.new_row = probe.old_row;
  probe.new_row.values[2] = Decimal(bal_raw + 10000, 2); // +100.00
  probe.update_sql =
      "UPDATE customer SET c_acctbal = c_acctbal + 100 WHERE c_custkey = " +
      custkey;
  probe.restore_sql = "UPDATE customer SET c_acctbal = " + std::to_string(acctbal) +
                      " WHERE c_custkey = " + custkey;
  return probe;
}

const SharedQueryRegistrar kEntry{SharedQueryCatalogEntry{22,
     SharedWorkloadKind::Tpch,
     "tpch",
     query::TpchQ22Layout::kAccelerator,
     "WITH custsale AS ("
     "SELECT SUBSTRING(c_phone FROM 1 FOR 2) AS cntrycode, c_acctbal "
     "FROM customer "
     "WHERE SUBSTRING(c_phone FROM 1 FOR 2) IN ('30', '19', '14', '28', '11', '22', '32') "
     "AND c_acctbal > (SELECT AVG(c_acctbal) FROM customer WHERE c_acctbal > 0.00 "
     "AND SUBSTRING(c_phone FROM 1 FOR 2) IN ('30', '19', '14', '28', '11', '22', '32')) "
     "AND NOT EXISTS (SELECT * FROM orders WHERE o_custkey = c_custkey)) "
     "SELECT cntrycode, COUNT(*) AS numcust, SUM(c_acctbal) AS totacctbal "
     "FROM custsale GROUP BY cntrycode ORDER BY cntrycode",
     "SELECT * FROM accel_tpch_q22_aux('30', '19', '14', '28', '11', '22', '32')",
     "SELECT * FROM accel_tpch_q22('30', '19', '14', '28', '11', '22', '32')",
     "SELECT * FROM accel_tpch_q22_aux('30', '19', '14', '28', '11', '22', '32')",
     "SELECT c_custkey, c_phone, c_acctbal FROM customer "
     "WHERE SUBSTRING(c_phone FROM 1 FOR 2) IN "
     "('30', '19', '14', '28', '11', '22', '32') "
     "AND c_acctbal > (SELECT AVG(c_acctbal) FROM customer WHERE c_acctbal > 0.00 "
     "AND SUBSTRING(c_phone FROM 1 FOR 2) IN "
     "('30', '19', '14', '28', '11', '22', '32')) "
     "AND NOT EXISTS (SELECT * FROM orders WHERE o_custkey = c_custkey) "
     "ORDER BY c_custkey LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::String, std::string{"30"}),
       core::AuxValue::Of(core::AuxType::String, std::string{"19"}),
       core::AuxValue::Of(core::AuxType::String, std::string{"14"}),
       core::AuxValue::Of(core::AuxType::String, std::string{"28"}),
       core::AuxValue::Of(core::AuxType::String, std::string{"11"}),
       core::AuxValue::Of(core::AuxType::String, std::string{"22"}),
       core::AuxValue::Of(core::AuxType::String, std::string{"32"})}},
     2,
     false,
     BuildTpchQ22Probe}};

} // namespace q22_catalog
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
  auto scan_lock = LockScan(connection_p);
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

struct TpchQ22Scan : TpchQ22InitialScan {
  explicit TpchQ22Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanCustomers(
      const std::function<void(const TpchQ22CustomerInput &)> &consumer) override {
    ScanTypedChunks(
        connection,
        "SELECT CAST(c_custkey AS BIGINT), c_phone, "
        "CAST(round(c_acctbal * 100) AS BIGINT) FROM customer",
        [&](::duckdb::DataChunk &chunk) {
          TypedCol<int64_t> key, bal;
          TypedCol<::duckdb::string_t> phone;
          key.Init(chunk, 0);
          phone.Init(chunk, 1);
          bal.Init(chunk, 2);
          TpchQ22CustomerInput input;
          for (::duckdb::idx_t i = 0; i < chunk.size(); ++i) {
            if (!key.Valid(i) || !phone.Valid(i) || !bal.Valid(i))
              continue;
            input.custkey = key(i);
            input.phone.assign(phone(i).GetData(), phone(i).GetSize());
            input.acctbal_raw = bal(i);
            input.acctbal_scale = 2;
            consumer(input);
          }
        });
  }
  bool ScanOrderCounts(
      const std::function<void(int64_t, int64_t)> &consumer) override {
    ScanTypedChunks(
        connection,
        "SELECT CAST(o_custkey AS BIGINT), COUNT(*) FROM orders GROUP BY o_custkey",
        [&](::duckdb::DataChunk &chunk) {
          TypedCol<int64_t> key, cnt;
          key.Init(chunk, 0);
          cnt.Init(chunk, 1);
          for (::duckdb::idx_t i = 0; i < chunk.size(); ++i) {
            if (!key.Valid(i) || !cnt.Valid(i))
              continue;
            consumer(key(i), cnt(i));
          }
        });
    return true;
  }
  void ScanOrders(
      const std::function<void(const TpchQ22OrderInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT o_custkey FROM orders",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row))});
        });
  }
  ::duckdb::Connection &connection;
};

const AuxQueryRegistrar kQuery{AuxQuery{
    query::TpchQ22Layout::kAccelerator,
    "accel_tpch_q22_aux",
    {::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::VARCHAR},
    [](::duckdb::Connection &connection) {
      const TpchQ22Config c{1, 2};
      return MakeBinding<TpchQ22Scan, TpchQ22Module>(connection, CaptureTableList{{"orders", c.orders_table}, {"customer", c.customer_table}}, c);
    }}};

} // namespace
} // namespace accelerator_codegen::aux_state::engine::duckdb
