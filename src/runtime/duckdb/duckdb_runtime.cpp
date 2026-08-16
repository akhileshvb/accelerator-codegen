// GENERATED BACKEND
#include "accelerator_codegen/runtime/duckdb/duckdb_runtime.hpp"
#include "accelerator_codegen/runtime/duckdb/tpch_rf.hpp"

#include <cmath>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

#include "sec_edgar/q1/aux/module.hpp"
#include "sec_edgar/q2/aux/module.hpp"
#include "sec_edgar/q3/aux/module.hpp"
#include "sec_edgar/q4/aux/module.hpp"
#include "sec_edgar/q6/aux/module.hpp"
#include "sec_edgar/q24/aux/module.hpp"
#include "tpch/q1/aux/module.hpp"
#include "tpch/q5/aux/module.hpp"
#include "tpch/q3/aux/module.hpp"
#include "tpch/q6/aux/module.hpp"
#include "tpch/q10/aux/module.hpp"
#include "tpch/q11/aux/module.hpp"
#include "tpch/q12/aux/module.hpp"
#include "tpch/q14/aux/module.hpp"
#include "tpch/q16/aux/module.hpp"
#include "tpch/q17/aux/module.hpp"
#include "tpch/q18/aux/module.hpp"
#include "tpch/q2/aux/module.hpp"
#include "tpch/q21/aux/module.hpp"
#include "tpch/q9/aux/module.hpp"
#include "accelerator_codegen/stateful/core/aux_calibration.hpp"
#include "accelerator_codegen/harness/aux_state_footprint.hpp"
#include "accelerator_codegen/harness/cost_calibration.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace duckdb_backend = accelerator_codegen::aux_state::engine::duckdb;
namespace query = accelerator_codegen::aux_state::query;
using namespace query;

void Require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

int64_t DecimalRaw(const ::duckdb::Value &value) {
  if (value.IsNull()) {
    return 0;
  }
  if (value.type().id() == ::duckdb::LogicalTypeId::DECIMAL) {
    const auto scale = static_cast<int32_t>(
        ::duckdb::DecimalType::GetScale(value.type()));
    // GetValue<int64_t>() casts decimals like CAST(... AS BIGINT) and drops
    // fractional digits. Read the scaled integral payload instead.
    int64_t raw = 0;
    switch (value.type().InternalType()) {
    case ::duckdb::PhysicalType::INT16:
      raw = value.GetValueUnsafe<int16_t>();
      break;
    case ::duckdb::PhysicalType::INT32:
      raw = value.GetValueUnsafe<int32_t>();
      break;
    case ::duckdb::PhysicalType::INT64:
      raw = value.GetValueUnsafe<int64_t>();
      break;
    case ::duckdb::PhysicalType::INT128: {
      const auto huge = value.GetValueUnsafe<::duckdb::hugeint_t>();
      if (huge.upper == 0) {
        raw = static_cast<int64_t>(huge.lower);
      } else if (huge.upper == -1 &&
                 static_cast<int64_t>(huge.lower) < 0) {
        raw = static_cast<int64_t>(huge.lower);
      } else {
        return static_cast<int64_t>(
            std::llround(std::stod(value.ToString()) * 100.0));
      }
      break;
    }
    default:
      return static_cast<int64_t>(
          std::llround(std::stod(value.ToString()) * 100.0));
    }
    // Normalize to scale 2 for aux kernels.
    int32_t delta = 2 - scale;
    if (delta == 0) {
      return raw;
    }
    if (delta > 0) {
      for (int32_t i = 0; i < delta; ++i) {
        raw *= 10;
      }
      return raw;
    }
    for (int32_t i = 0; i < -delta; ++i) {
      raw /= 10;
    }
    return raw;
  }
  return static_cast<int64_t>(
      std::llround(std::stod(value.ToString()) * 100.0));
}

int64_t SafeInt64(const ::duckdb::Value &value) {
  if (value.IsNull()) {
    return 0;
  }
  switch (value.type().id()) {
  case ::duckdb::LogicalTypeId::BIGINT:
    return value.GetValue<int64_t>();
  case ::duckdb::LogicalTypeId::INTEGER:
    return value.GetValue<int32_t>();
  case ::duckdb::LogicalTypeId::SMALLINT:
    return value.GetValue<int16_t>();
  case ::duckdb::LogicalTypeId::TINYINT:
    return value.GetValue<int8_t>();
  case ::duckdb::LogicalTypeId::UINTEGER:
    return static_cast<int64_t>(value.GetValue<uint32_t>());
  case ::duckdb::LogicalTypeId::UBIGINT:
    return static_cast<int64_t>(value.GetValue<uint64_t>());
  case ::duckdb::LogicalTypeId::HUGEINT:
    return static_cast<int64_t>(value.GetValue<::duckdb::hugeint_t>().lower);
  default:
    return static_cast<int64_t>(std::llround(std::stod(value.ToString())));
  }
}

int32_t SafeInt32(const ::duckdb::Value &value) {
  return static_cast<int32_t>(SafeInt64(value));
}

int32_t DateDays(const ::duckdb::Value &value) {
  if (value.IsNull()) {
    return 0;
  }
  if (value.type().id() == ::duckdb::LogicalTypeId::DATE) {
    return ::duckdb::Date::EpochDays(value.GetValue<::duckdb::date_t>());
  }
  return SafeInt32(value);
}

std::string OptionalBenchScanLimitSuffix(const char *env_name) {
  const char *value = std::getenv(env_name);
  if (!value || !*value) {
    return "";
  }
  return std::string(" LIMIT ") + value;
}

template <typename Callback>
void ScanQuery(::duckdb::Connection &connection, const std::string &sql,
               Callback &&callback) {
  auto result = connection.Query(sql);
  Require(result != nullptr && !result->HasError(),
          result ? result->GetError() : "DuckDB scan returned null");
  for (::duckdb::idx_t row_index = 0; row_index < result->RowCount();
       ++row_index) {
    callback(*result, row_index);
  }
}

// Streaming scan for large SF builds: avoids materializing the full result.
template <typename Callback>
void ScanQueryStreaming(::duckdb::Connection &connection, const std::string &sql,
                        Callback &&callback) {
  auto result = connection.SendQuery(sql);
  Require(result != nullptr && !result->HasError(),
          result ? result->GetError() : "DuckDB scan returned null");
  while (true) {
    auto chunk = result->Fetch();
    if (!chunk || chunk->size() == 0) {
      break;
    }
    for (::duckdb::idx_t row_index = 0; row_index < chunk->size(); ++row_index) {
      callback(*chunk, row_index);
    }
  }
  Require(!result->HasError(), result->GetError());
}

::duckdb::Value ChunkValue(const ::duckdb::DataChunk &chunk,
                           ::duckdb::idx_t col, ::duckdb::idx_t row) {
  return chunk.GetValue(col, row);
}

char FirstChar(const ::duckdb::Value &value) {
  if (value.IsNull()) {
    return '\0';
  }
  const auto text = value.ToString();
  return text.empty() ? '\0' : text[0];
}

struct MaterializeSink final : core::AuxRowSink {
  void Begin(const core::OutputSchema &) override {}
  void WriteRow(const core::AuxRow &row) override { rows.push_back(row); }
  void End() override {}
  std::vector<core::AuxRow> rows;
};

struct Builder final : core::StateBuilder {
  void Write(core::StateImage image) override {
    images.push_back(std::move(image));
  }
  void Finish() override { finished = true; }
  std::vector<core::StateImage> images;
  bool finished = false;
};

template <typename QueryModule>
struct ModuleBinding : DuckdbRuntime::Binding {
  QueryModule module;

  template <typename... Args>
  ModuleBinding(Args &&...args) : module(std::forward<Args>(args)...) {}

  core::AuxQueryModule &Module() override { return module; }
  void Build(DuckDBAuxBackend &backend,
             core::StateBuilder &builder) override {
    module.Build(backend, builder);
  }
  void Execute(DuckDBAuxBackend &backend,
               const core::QueryArguments &arguments,
               core::AuxRowSink &sink) override {
    module.Execute(backend, arguments, sink);
  }
};

struct TpchQ1Scan : TpchQ1InitialScan {
  explicit TpchQ1Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanLineitems(
      const std::function<void(const TpchQ1LineitemInput &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT l_shipdate, l_quantity, l_extendedprice, l_discount, "
        "l_tax, l_returnflag, l_linestatus FROM lineitem",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({DateDays(ChunkValue(chunk, 0, row)),
                    DecimalRaw(ChunkValue(chunk, 1, row)), 2,
                    DecimalRaw(ChunkValue(chunk, 2, row)), 2,
                    DecimalRaw(ChunkValue(chunk, 3, row)), 2,
                    DecimalRaw(ChunkValue(chunk, 4, row)), 2,
                    FirstChar(ChunkValue(chunk, 5, row)),
                    FirstChar(ChunkValue(chunk, 6, row))});
        });
  }
  bool ScanDayGroupAggregates(
      const std::function<void(const TpchQ1DayGroupAggregate &)> &consumer)
      override {
    ScanQueryStreaming(
        connection,
        "SELECT l_shipdate, l_returnflag, l_linestatus, "
        "COUNT(*)::BIGINT, "
        "SUM(CAST(round(l_quantity * 100) AS BIGINT)), "
        "SUM(CAST(round(l_extendedprice * 100) AS BIGINT)), "
        "SUM(CAST(round(l_discount * 100) AS BIGINT)), "
        "SUM(CAST(round(l_extendedprice * (1 - l_discount) * 10000) AS BIGINT)), "
        "SUM(CAST(round(l_extendedprice * (1 - l_discount) * (1 + l_tax) * "
        "1000000) AS BIGINT)) "
        "FROM lineitem GROUP BY 1, 2, 3",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({DateDays(ChunkValue(chunk, 0, row)),
                    FirstChar(ChunkValue(chunk, 1, row)),
                    FirstChar(ChunkValue(chunk, 2, row)),
                    SafeInt64(ChunkValue(chunk, 3, row)),
                    SafeInt64(ChunkValue(chunk, 4, row)),
                    SafeInt64(ChunkValue(chunk, 5, row)),
                    SafeInt64(ChunkValue(chunk, 6, row)),
                    SafeInt64(ChunkValue(chunk, 7, row)),
                    SafeInt64(ChunkValue(chunk, 8, row))});
        });
    return true;
  }
  ::duckdb::Connection &connection;
};

struct TpchQ3Scan : TpchQ3InitialScan {
  explicit TpchQ3Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanCustomers(
      const std::function<void(const TpchQ3CustomerInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT c_custkey, c_mktsegment FROM customer",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString()});
        });
  }
  void ScanOrders(
      const std::function<void(const TpchQ3OrderInput &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT o_orderkey, o_orderdate, o_custkey, o_shippriority FROM orders",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    DateDays(ChunkValue(chunk, 1, row)),
                    SafeInt64(ChunkValue(chunk, 2, row)),
                    SafeInt32(ChunkValue(chunk, 3, row))});
        });
  }
  bool LookupOrderContext(int64_t orderkey, OrderContext &out) override {
    out = {};
    bool found = false;
    ScanQueryStreaming(
        connection,
        "SELECT o.o_orderkey, o.o_orderdate, o.o_custkey, o.o_shippriority, "
        "c.c_mktsegment "
        "FROM orders o JOIN customer c ON o.o_custkey = c.c_custkey "
        "WHERE o.o_orderkey = " +
            std::to_string(orderkey),
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          out.orderdate_days = DateDays(ChunkValue(chunk, 1, row));
          out.custkey = SafeInt64(ChunkValue(chunk, 2, row));
          out.shippriority = SafeInt32(ChunkValue(chunk, 3, row));
          out.segment = TpchQ3PartitionFromMktsegment(
              ChunkValue(chunk, 4, row).ToString());
          found = out.segment != 0;
        });
    return found;
  }
  void ScanLineitems(
      const std::function<void(const TpchQ3LineitemInput &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT l_orderkey, l_extendedprice, l_discount, l_shipdate FROM lineitem",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    DecimalRaw(ChunkValue(chunk, 1, row)), 2,
                    DecimalRaw(ChunkValue(chunk, 2, row)), 2,
                    DateDays(ChunkValue(chunk, 3, row))});
        });
  }
  ::duckdb::Connection &connection;
};

struct TpchQ6Scan : TpchQ6InitialScan {
  explicit TpchQ6Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanLineitems(
      const std::function<void(const TpchQ6LineitemInput &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT l_shipdate, l_discount, l_quantity, l_extendedprice FROM lineitem",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({DateDays(ChunkValue(chunk, 0, row)),
                    DecimalRaw(ChunkValue(chunk, 1, row)), 2,
                    DecimalRaw(ChunkValue(chunk, 2, row)), 2,
                    DecimalRaw(ChunkValue(chunk, 3, row)), 2});
        });
  }
  ::duckdb::Connection &connection;
};

struct TpchQ10Scan : TpchQ10InitialScan {
  explicit TpchQ10Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanCustomers(
      const std::function<void(const TpchQ10CustomerInput &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT c_custkey, c_name, c_acctbal, c_nationkey, c_address, "
        "c_phone, c_comment FROM customer",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString(),
                    DecimalRaw(ChunkValue(chunk, 2, row)),
                    SafeInt32(ChunkValue(chunk, 3, row)),
                    ChunkValue(chunk, 4, row).ToString(),
                    ChunkValue(chunk, 5, row).ToString(),
                    ChunkValue(chunk, 6, row).ToString()});
        });
  }
  void ScanNations(
      const std::function<void(const TpchQ10NationInput &)> &consumer) override {
    ScanQuery(connection, "SELECT n_nationkey, n_name FROM nation",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({SafeInt32(result.GetValue(0, row)),
                          result.GetValue(1, row).ToString()});
              });
  }
  void ScanOrders(
      const std::function<void(const TpchQ10OrderInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT o_orderkey, o_custkey, o_orderdate FROM orders",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt64(ChunkValue(chunk, 1, row)),
                    DateDays(ChunkValue(chunk, 2, row))});
        });
  }
  void ScanLineitems(
      const std::function<void(const TpchQ10LineitemInput &)> &consumer) override {
    // Fallback path only: filter returned rows in SQL when the C++ join is used.
    ScanQueryStreaming(
        connection,
        "SELECT l_orderkey, l_extendedprice, l_discount, l_returnflag "
        "FROM lineitem WHERE l_returnflag = 'R'",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    DecimalRaw(ChunkValue(chunk, 1, row)), 2,
                    DecimalRaw(ChunkValue(chunk, 2, row)), 2, true});
        });
  }
  bool ScanReturnedCustomerRevenue(
      const std::function<void(const TpchQ10ReturnedRevenueInput &)> &consumer)
      override {
    // Push returned-lineitem ⨝ orders aggregation into DuckDB. Revenue raw
    // matches TpchQ10RevenueRaw: round(extended * (1-discount) * 10000) per
    // line, then sum. Stream chunks — do not materialize the full GROUP BY.
    ScanQueryStreaming(
        connection,
        "SELECT CAST(year(o_orderdate) * 100 + month(o_orderdate) AS INTEGER), "
        "o_custkey, "
        "sum(CAST(round(l_extendedprice * (1 - l_discount) * 10000) AS BIGINT)) "
        "FROM lineitem "
        "JOIN orders ON l_orderkey = o_orderkey "
        "WHERE l_returnflag = 'R' "
        "GROUP BY 1, 2",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt32(ChunkValue(chunk, 0, row)),
                    SafeInt64(ChunkValue(chunk, 1, row)),
                    SafeInt64(ChunkValue(chunk, 2, row))});
        });
    return true;
  }
  bool LookupOrders(
      const std::vector<int64_t> &orderkeys,
      std::unordered_map<int64_t, TpchQ10Order> &out) override {
    out.clear();
    if (orderkeys.empty()) {
      return true;
    }
    std::ostringstream sql;
    sql << "SELECT o_orderkey, o_custkey, o_orderdate FROM orders "
           "WHERE o_orderkey IN (";
    for (std::size_t i = 0; i < orderkeys.size(); ++i) {
      if (i > 0) {
        sql << ',';
      }
      sql << orderkeys[i];
    }
    sql << ')';
    ScanQueryStreaming(connection, sql.str(),
                       [&](const ::duckdb::DataChunk &chunk,
                           ::duckdb::idx_t row) {
                         out.emplace(SafeInt64(ChunkValue(chunk, 0, row)),
                                     TpchQ10Order{
                                         SafeInt64(ChunkValue(chunk, 1, row)),
                                         DateDays(ChunkValue(chunk, 2, row))});
                       });
    return true;
  }
  ::duckdb::Connection &connection;
};

struct TpchQ12Scan : TpchQ12InitialScan {
  explicit TpchQ12Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanOrders(
      const std::function<void(const TpchQ12OrderInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT o_orderkey, o_orderpriority FROM orders",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString()});
        });
  }
  void ScanLineitems(
      const std::function<void(const TpchQ12LineitemInput &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT l_orderkey, l_shipmode, l_commitdate, l_receiptdate, "
        "l_shipdate FROM lineitem",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString(),
                    DateDays(ChunkValue(chunk, 2, row)),
                    DateDays(ChunkValue(chunk, 3, row)),
                    DateDays(ChunkValue(chunk, 4, row))});
        });
  }
  ::duckdb::Connection &connection;
};

struct TpchQ14Scan : TpchQ14InitialScan {
  explicit TpchQ14Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanParts(
      const std::function<void(const TpchQ14PartInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT p_partkey, p_type FROM part",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString()});
        });
  }
  void ScanLineitems(
      const std::function<void(const TpchQ14LineitemInput &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT l_partkey, l_extendedprice, l_discount, l_shipdate FROM lineitem",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    DecimalRaw(ChunkValue(chunk, 1, row)), 2,
                    DecimalRaw(ChunkValue(chunk, 2, row)), 2,
                    DateDays(ChunkValue(chunk, 3, row))});
        });
  }
  ::duckdb::Connection &connection;
};

struct TpchQ5Scan : TpchQ5InitialScan {
  explicit TpchQ5Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanRegions(
      const std::function<void(const TpchQ5RegionInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT r_regionkey, r_name FROM region",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt32(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString()});
        });
  }
  void ScanNations(
      const std::function<void(const TpchQ5NationInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT n_nationkey, n_name, n_regionkey FROM nation",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt32(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString(),
                    SafeInt32(ChunkValue(chunk, 2, row))});
        });
  }
  void ScanCustomers(
      const std::function<void(const TpchQ5CustomerInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT c_custkey, c_nationkey FROM customer",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt32(ChunkValue(chunk, 1, row))});
        });
  }
  void ScanSuppliers(
      const std::function<void(const TpchQ5SupplierInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT s_suppkey, s_nationkey FROM supplier",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt32(ChunkValue(chunk, 1, row))});
        });
  }
  void ScanOrders(
      const std::function<void(const TpchQ5OrderInput &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT o_orderkey, o_custkey, o_orderdate FROM orders",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt64(ChunkValue(chunk, 1, row)),
                    DateDays(ChunkValue(chunk, 2, row))});
        });
  }
  void ScanLineitems(
      const std::function<void(const TpchQ5LineitemInput &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT l_orderkey, l_suppkey, l_extendedprice, l_discount FROM lineitem",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt64(ChunkValue(chunk, 1, row)),
                    DecimalRaw(ChunkValue(chunk, 2, row)), 2,
                    DecimalRaw(ChunkValue(chunk, 3, row)), 2});
        });
  }
  ::duckdb::Connection &connection;
};

struct TpchQ9Scan : TpchQ9InitialScan {
  explicit TpchQ9Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanParts(
      const std::function<void(const TpchQ9PartInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT p_partkey, p_name FROM part",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString()});
        });
  }
  void ScanSuppliers(
      const std::function<void(const TpchQ9SupplierInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT s_suppkey, s_nationkey FROM supplier",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt32(ChunkValue(chunk, 1, row))});
        });
  }
  void ScanNations(
      const std::function<void(const TpchQ9NationInput &)> &consumer) override {
    ScanQuery(connection, "SELECT n_nationkey, n_name FROM nation",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({SafeInt32(result.GetValue(0, row)),
                          result.GetValue(1, row).ToString()});
              });
  }
  void ScanOrders(
      const std::function<void(const TpchQ9OrdersInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT o_orderkey, o_orderdate FROM orders",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    DateDays(ChunkValue(chunk, 1, row))});
        });
  }
  void ScanPartsupps(
      const std::function<void(const TpchQ9PartsuppInput &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT ps_partkey, ps_suppkey, ps_supplycost FROM partsupp",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt64(ChunkValue(chunk, 1, row)),
                    DecimalRaw(ChunkValue(chunk, 2, row))});
        });
  }
  void ScanLineitems(
      const std::function<void(const TpchQ9LineitemInput &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT l_partkey, l_suppkey, l_orderkey, l_quantity, "
        "l_extendedprice, l_discount FROM lineitem",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt64(ChunkValue(chunk, 1, row)),
                    SafeInt64(ChunkValue(chunk, 2, row)),
                    DecimalRaw(ChunkValue(chunk, 3, row)),
                    DecimalRaw(ChunkValue(chunk, 4, row)),
                    DecimalRaw(ChunkValue(chunk, 5, row))});
        });
  }
  bool ScanProfitContributions(
      const std::function<void(const TpchQ9ProfitContribution &)> &consumer)
      override {
    // Push LI ⨝ orders ⨝ supplier ⨝ partsupp profit aggregation into DuckDB.
    ScanQueryStreaming(
        connection,
        "SELECT l.l_partkey, s.s_nationkey, year(o.o_orderdate), "
        "SUM(CAST(round(l.l_extendedprice * (1 - l.l_discount) * 10000) AS "
        "BIGINT) - "
        "CAST(round(ps.ps_supplycost * l.l_quantity * 10000) AS BIGINT)) "
        "FROM lineitem l "
        "JOIN orders o ON l.l_orderkey = o.o_orderkey "
        "JOIN supplier s ON l.l_suppkey = s.s_suppkey "
        "JOIN partsupp ps ON l.l_partkey = ps.ps_partkey AND "
        "l.l_suppkey = ps.ps_suppkey "
        "GROUP BY 1, 2, 3",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt32(ChunkValue(chunk, 1, row)),
                    SafeInt32(ChunkValue(chunk, 2, row)),
                    SafeInt64(ChunkValue(chunk, 3, row))});
        });
    return true;
  }
  ::duckdb::Connection &connection;
};

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

struct TpchQ16Scan : TpchQ16InitialScan {
  explicit TpchQ16Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanSuppliers(
      const std::function<void(const TpchQ16SupplierInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT s_suppkey, s_comment FROM supplier",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString()});
        });
  }
  void ScanParts(
      const std::function<void(const TpchQ16PartInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT p_partkey, p_brand, p_type, p_size FROM part",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString(),
                    ChunkValue(chunk, 2, row).ToString(),
                    SafeInt32(ChunkValue(chunk, 3, row))});
        });
  }
  void ScanPartsupp(
      const std::function<void(const TpchQ16PartsuppInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT ps_partkey, ps_suppkey FROM partsupp",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt64(ChunkValue(chunk, 1, row))});
        });
  }
  ::duckdb::Connection &connection;
};

struct TpchQ17Scan : TpchQ17InitialScan {
  explicit TpchQ17Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanParts(
      const std::function<void(const TpchQ17PartInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT p_partkey, p_brand, p_container FROM part",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString(),
                    ChunkValue(chunk, 2, row).ToString()});
        });
  }
  void ScanLineitems(
      const std::function<void(const TpchQ17LineitemInput &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT l_partkey, l_quantity, l_extendedprice FROM lineitem" +
            OptionalBenchScanLimitSuffix("ACCEL_Q17_BENCH_SCAN_LIMIT"),
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    DecimalRaw(ChunkValue(chunk, 1, row)),
                    DecimalRaw(ChunkValue(chunk, 2, row))});
        });
  }
  bool ScanPartQuantityAggregates(
      const std::function<void(const TpchQ17PartQuantityAggregate &)> &consumer)
      override {
    // Bench scan limits apply to the row path only.
    if (std::getenv("ACCEL_Q17_BENCH_SCAN_LIMIT")) {
      return false;
    }
    ScanQueryStreaming(
        connection,
        "SELECT l_partkey, "
        "CAST(round(l_quantity * 100) AS BIGINT), "
        "SUM(CAST(round(l_extendedprice * 100) AS BIGINT)), "
        "COUNT(*)::BIGINT "
        "FROM lineitem "
        "GROUP BY 1, 2",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt64(ChunkValue(chunk, 1, row)),
                    SafeInt64(ChunkValue(chunk, 2, row)),
                    SafeInt64(ChunkValue(chunk, 3, row))});
        });
    return true;
  }
  ::duckdb::Connection &connection;
};

struct TpchQ18Scan : TpchQ18InitialScan {
  explicit TpchQ18Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanLineitems(
      const std::function<void(const TpchQ18LineitemInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT l_orderkey, l_quantity FROM lineitem",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    DecimalRaw(ChunkValue(chunk, 1, row))});
        });
  }
  bool ScanOrderQuantityAggregates(
      const std::function<void(const TpchQ18OrderQuantityAggregate &)> &consumer)
      override {
    ScanQueryStreaming(
        connection,
        "SELECT l_orderkey, SUM(CAST(round(l_quantity * 100) AS BIGINT)) "
        "FROM lineitem GROUP BY 1",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt64(ChunkValue(chunk, 1, row))});
        });
    return true;
  }
  void ScanOrders(
      const std::function<void(const TpchQ18OrderInput &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT o_orderkey, o_custkey, o_orderdate, o_totalprice FROM orders",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt64(ChunkValue(chunk, 1, row)),
                    DateDays(ChunkValue(chunk, 2, row)),
                    DecimalRaw(ChunkValue(chunk, 3, row))});
        });
  }
  bool LookupOrder(int64_t orderkey, TpchQ18OrderInput &out) override {
    out = {};
    bool found = false;
    ScanQueryStreaming(
        connection,
        "SELECT o_orderkey, o_custkey, o_orderdate, o_totalprice FROM orders "
        "WHERE o_orderkey = " +
            std::to_string(orderkey),
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          out = {SafeInt64(ChunkValue(chunk, 0, row)),
                 SafeInt64(ChunkValue(chunk, 1, row)),
                 DateDays(ChunkValue(chunk, 2, row)),
                 DecimalRaw(ChunkValue(chunk, 3, row))};
          found = true;
        });
    return found;
  }
  void ScanCustomers(
      const std::function<void(const TpchQ18CustomerInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT c_custkey, c_name FROM customer",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString()});
        });
  }
  ::duckdb::Connection &connection;
};


struct TpchQ2Scan : TpchQ2InitialScan {
  explicit TpchQ2Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanParts(
      const std::function<void(const TpchQ2PartInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT p_partkey, p_mfgr, p_type, p_size FROM part",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString(),
                    ChunkValue(chunk, 2, row).ToString(),
                    SafeInt32(ChunkValue(chunk, 3, row))});
        });
  }
  void ScanSuppliers(
      const std::function<void(const TpchQ2SupplierInput &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT s_suppkey, s_name, s_address, s_nationkey, s_phone, s_acctbal, s_comment FROM supplier",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString(),
                    ChunkValue(chunk, 2, row).ToString(),
                    SafeInt32(ChunkValue(chunk, 3, row)),
                    ChunkValue(chunk, 4, row).ToString(),
                    DecimalRaw(ChunkValue(chunk, 5, row)),
                    ChunkValue(chunk, 6, row).ToString()});
        });
  }
  void ScanPartsupps(
      const std::function<void(const TpchQ2PartsuppInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT ps_partkey, ps_suppkey, ps_supplycost FROM partsupp",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt64(ChunkValue(chunk, 1, row)),
                    DecimalRaw(ChunkValue(chunk, 2, row))});
        });
  }
  void ScanNations(
      const std::function<void(const TpchQ2NationInput &)> &consumer) override {
    ScanQuery(connection, "SELECT n_nationkey, n_name, n_regionkey FROM nation",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({SafeInt32(result.GetValue(0, row)),
                          result.GetValue(1, row).ToString(),
                          SafeInt32(result.GetValue(2, row))});
              });
  }
  void ScanRegions(
      const std::function<void(const TpchQ2RegionInput &)> &consumer) override {
    ScanQuery(connection, "SELECT r_regionkey, r_name FROM region",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({SafeInt32(result.GetValue(0, row)),
                          result.GetValue(1, row).ToString()});
              });
  }
  ::duckdb::Connection &connection;
};

struct TpchQ21Scan : TpchQ21InitialScan {
  explicit TpchQ21Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanSuppliers(
      const std::function<void(const TpchQ21SupplierInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT s_suppkey, s_name, s_nationkey FROM supplier",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString(),
                    SafeInt32(ChunkValue(chunk, 2, row))});
        });
  }
  void ScanNations(
      const std::function<void(const TpchQ21NationInput &)> &consumer) override {
    ScanQuery(connection, "SELECT n_nationkey, n_name FROM nation",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({SafeInt32(result.GetValue(0, row)),
                          result.GetValue(1, row).ToString()});
              });
  }
  void ScanOrders(
      const std::function<void(const TpchQ21OrderInput &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT o_orderkey, o_orderstatus FROM orders WHERE o_orderstatus = 'F'",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    FirstChar(ChunkValue(chunk, 1, row))});
        });
  }
  void ScanLineitems(
      const std::function<void(const TpchQ21LineitemInput &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT l.l_orderkey, l.l_suppkey, l.l_commitdate, l.l_receiptdate "
        "FROM lineitem l "
        "INNER JOIN orders o ON l.l_orderkey = o.o_orderkey AND "
        "o.o_orderstatus = 'F'" +
            OptionalBenchScanLimitSuffix("ACCEL_Q21_BENCH_SCAN_LIMIT"),
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt64(ChunkValue(chunk, 1, row)),
                    DateDays(ChunkValue(chunk, 2, row)),
                    DateDays(ChunkValue(chunk, 3, row))});
        });
  }
  ::duckdb::Connection &connection;
};

struct SecEdgarQ1Scan : SecEdgarQ1InitialScan {
  explicit SecEdgarQ1Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanPre(
      const std::function<void(const SecEdgarQ1PreInput &)> &consumer) const override {
    ScanQuery(connection, "SELECT stmt, rfile, adsh, line FROM pre",
              [&](auto &result, ::duckdb::idx_t row) {
                const auto stmt = result.GetValue(0, row);
                const auto rfile = result.GetValue(1, row);
                consumer({stmt.IsNull() ? std::string() : stmt.ToString(),
                          rfile.IsNull() ? std::string() : rfile.ToString(),
                          rfile.IsNull(), stmt.IsNull(),
                          result.GetValue(2, row).ToString(),
                          static_cast<int32_t>(SafeInt32(result.GetValue(3, row)))});
              });
  }
  ::duckdb::Connection &connection;
};

struct SecEdgarQ2Scan : SecEdgarQ2InitialScan {
  explicit SecEdgarQ2Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanNum(
      const std::function<void(const SecEdgarQ2NumInput &)> &consumer) override {
    ScanQuery(connection, "SELECT adsh, tag, uom, value FROM num",
              [&](auto &result, ::duckdb::idx_t row) {
                const auto value = result.GetValue(3, row);
                consumer({result.GetValue(0, row).ToString(),
                          result.GetValue(1, row).ToString(),
                          result.GetValue(2, row).ToString(),
                          value.IsNull() ? 0.0 : std::stod(value.ToString()),
                          value.IsNull()});
              });
  }
  void ScanSub(
      const std::function<void(const SecEdgarQ2SubInput &)> &consumer) override {
    ScanQuery(connection, "SELECT adsh, name, fy FROM sub",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({result.GetValue(0, row).ToString(),
                          result.GetValue(1, row).ToString(),
                          static_cast<int32_t>(SafeInt32(result.GetValue(2, row)))});
              });
  }
  ::duckdb::Connection &connection;
};

struct SecEdgarQ3Scan : SecEdgarQ3InitialScan {
  explicit SecEdgarQ3Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanNum(
      const std::function<void(const SecEdgarQ3NumInput &)> &consumer) override {
    ScanQuery(connection, "SELECT adsh, uom, value FROM num",
              [&](auto &result, ::duckdb::idx_t row) {
                const auto value = result.GetValue(2, row);
                consumer({result.GetValue(0, row).ToString(),
                          result.GetValue(1, row).ToString(),
                          value.IsNull() ? 0.0 : std::stod(value.ToString()),
                          value.IsNull()});
              });
  }
  void ScanSub(
      const std::function<void(const SecEdgarQ3SubInput &)> &consumer) override {
    ScanQuery(connection, "SELECT adsh, name, cik, fy FROM sub",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({result.GetValue(0, row).ToString(),
                          result.GetValue(1, row).ToString(),
                          static_cast<int32_t>(SafeInt32(result.GetValue(2, row))),
                          static_cast<int32_t>(SafeInt32(result.GetValue(3, row)))});
              });
  }
  ::duckdb::Connection &connection;
};

struct SecEdgarQ4Scan : SecEdgarQ4InitialScan {
  explicit SecEdgarQ4Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanSub(
      const std::function<void(const SecEdgarQ4SubInput &)> &consumer) const override {
    ScanQuery(connection, "SELECT adsh, cik, sic FROM sub",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({result.GetValue(0, row).ToString(),
                          static_cast<int32_t>(SafeInt32(result.GetValue(1, row))),
                          static_cast<int32_t>(SafeInt32(result.GetValue(2, row)))});
              });
  }
  void ScanTag(
      const std::function<void(const SecEdgarQ4TagInput &)> &consumer) const override {
    ScanQuery(connection, "SELECT tag, version, abstract, tlabel FROM tag",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({result.GetValue(0, row).ToString(),
                          result.GetValue(1, row).ToString(),
                          static_cast<int32_t>(SafeInt32(result.GetValue(2, row))),
                          result.GetValue(3, row).ToString()});
              });
  }
  void ScanPre(
      const std::function<void(const SecEdgarQ4PreInput &)> &consumer) const override {
    ScanQuery(connection, "SELECT adsh, tag, version, stmt FROM pre",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({result.GetValue(0, row).ToString(),
                          result.GetValue(1, row).ToString(),
                          result.GetValue(2, row).ToString(),
                          result.GetValue(3, row).ToString()});
              });
  }
  void ScanNum(
      const std::function<void(const SecEdgarQ4NumInput &)> &consumer) const override {
    ScanQuery(connection, "SELECT adsh, tag, version, uom, value FROM num",
              [&](auto &result, ::duckdb::idx_t row) {
                const auto value = result.GetValue(4, row);
                consumer({result.GetValue(0, row).ToString(),
                          result.GetValue(1, row).ToString(),
                          result.GetValue(2, row).ToString(),
                          result.GetValue(3, row).ToString(),
                          value.IsNull() ? 0.0 : std::stod(value.ToString()),
                          value.IsNull()});
              });
  }
  ::duckdb::Connection &connection;
};

struct SecEdgarQ6Scan : SecEdgarQ6InitialScan {
  explicit SecEdgarQ6Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanNum(
      const std::function<void(const SecEdgarQ6NumInput &)> &consumer) override {
    ScanQuery(connection, "SELECT adsh, tag, version, uom, value FROM num",
              [&](auto &result, ::duckdb::idx_t row) {
                const auto value = result.GetValue(4, row);
                consumer({result.GetValue(0, row).ToString(),
                          result.GetValue(1, row).ToString(),
                          result.GetValue(2, row).ToString(),
                          result.GetValue(3, row).ToString(),
                          value.IsNull() ? 0.0 : std::stod(value.ToString()),
                          value.IsNull()});
              });
  }
  void ScanSub(
      const std::function<void(const SecEdgarQ6SubInput &)> &consumer) override {
    ScanQuery(connection, "SELECT adsh, name, fy FROM sub",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({result.GetValue(0, row).ToString(),
                          result.GetValue(1, row).ToString(),
                          static_cast<int32_t>(SafeInt32(result.GetValue(2, row)))});
              });
  }
  void ScanPre(
      const std::function<void(const SecEdgarQ6PreInput &)> &consumer) override {
    ScanQuery(connection,
              "SELECT adsh, tag, version, stmt, plabel FROM pre",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({result.GetValue(0, row).ToString(),
                          result.GetValue(1, row).ToString(),
                          result.GetValue(2, row).ToString(),
                          result.GetValue(3, row).ToString(),
                          result.GetValue(4, row).ToString()});
              });
  }
  ::duckdb::Connection &connection;
};

struct SecEdgarQ24Scan : SecEdgarQ24InitialScan {
  explicit SecEdgarQ24Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanNum(
      const std::function<void(const SecEdgarQ24NumInput &)> &consumer) override {
    ScanQuery(connection, "SELECT adsh, tag, version, uom, value, ddate FROM num",
              [&](auto &result, ::duckdb::idx_t row) {
                const auto value = result.GetValue(4, row);
                const auto date = result.GetValue(5, row);
                consumer({result.GetValue(0, row).ToString(),
                          result.GetValue(1, row).ToString(),
                          result.GetValue(2, row).ToString(),
                          result.GetValue(3, row).ToString(),
                          value.IsNull() ? 0.0 : std::stod(value.ToString()),
                          value.IsNull(),
                          static_cast<int32_t>(SafeInt32(date))});
              });
  }
  void ScanPre(
      const std::function<void(const SecEdgarQ24PreInput &)> &consumer) override {
    ScanQuery(connection, "SELECT adsh, tag, version FROM pre",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({result.GetValue(0, row).ToString(),
                          result.GetValue(1, row).ToString(),
                          result.GetValue(2, row).ToString()});
              });
  }
  ::duckdb::Connection &connection;
};

template <typename Scan, typename QueryModule>
struct QueryBound : Scan, ModuleBinding<QueryModule> {
  template <typename... Args>
  QueryBound(::duckdb::Connection &connection, Args &&...args)
      : Scan(connection),
        ModuleBinding<QueryModule>(std::forward<Args>(args)...,
                                   static_cast<Scan *>(this)) {}
};

template <typename Scan, typename QueryModule, typename... ConfigArgs>
std::unique_ptr<DuckdbRuntime::Binding>
MakeBinding(::duckdb::Connection &connection, ConfigArgs &&...config_args) {
  return std::unique_ptr<DuckdbRuntime::Binding>(
      new QueryBound<Scan, QueryModule>(connection,
                                        std::forward<ConfigArgs>(config_args)...));
}

std::unique_ptr<DuckdbRuntime::Binding>
CreateBinding(::duckdb::Connection &connection,
              const SharedQueryCatalogEntry &entry) {
  switch (entry.query_id) {
  case 1:
    if (entry.workload == SharedWorkloadKind::Tpch) {
      return MakeBinding<TpchQ1Scan, TpchQ1Module>(connection, TpchQ1Config{1});
    }
    return MakeBinding<SecEdgarQ1Scan, SecEdgarQ1Module>(connection,
                                                         SecEdgarQ1Config{1});
  case 2:
    if (entry.workload == SharedWorkloadKind::Tpch) {
      return MakeBinding<TpchQ2Scan, TpchQ2Module>(
          connection, TpchQ2Config{1, 2, 3, 4, 5});
    }
    return MakeBinding<SecEdgarQ2Scan, SecEdgarQ2Module>(
        connection, SecEdgarQ2Config{1, 2});
  case 3:
    if (entry.workload == SharedWorkloadKind::Tpch) {
      return MakeBinding<TpchQ3Scan, TpchQ3Module>(connection,
                                                  TpchQ3Config{1, 2, 3});
    }
    return MakeBinding<SecEdgarQ3Scan, SecEdgarQ3Module>(
        connection, SecEdgarQ3Config{1, 2});
  case 4:
    return MakeBinding<SecEdgarQ4Scan, SecEdgarQ4Module>(
        connection, SecEdgarQ4Config{1, 2, 3, 4});
  case 5:
    return MakeBinding<TpchQ5Scan, TpchQ5Module>(
        connection, TpchQ5Config{1, 2, 3, 4, 5, 6});
  case 6:
    if (entry.workload == SharedWorkloadKind::Tpch) {
      return MakeBinding<TpchQ6Scan, TpchQ6Module>(connection, TpchQ6Config{1});
    }
    return MakeBinding<SecEdgarQ6Scan, SecEdgarQ6Module>(
        connection, SecEdgarQ6Config{1, 2, 3});
  case 9:
    return MakeBinding<TpchQ9Scan, TpchQ9Module>(connection,
                                                TpchQ9Config{1, 2, 3, 4, 5, 6});
  case 10:
    return MakeBinding<TpchQ10Scan, TpchQ10Module>(
        connection, TpchQ10Config{1, 2, 3, 4});
  case 11:
    return MakeBinding<TpchQ11Scan, TpchQ11Module>(connection,
                                                   TpchQ11Config{1, 2, 3});
  case 12:
    return MakeBinding<TpchQ12Scan, TpchQ12Module>(connection,
                                                    TpchQ12Config{1, 2});
  case 14:
    return MakeBinding<TpchQ14Scan, TpchQ14Module>(connection,
                                                   TpchQ14Config{1, 2});
  case 16:
    return MakeBinding<TpchQ16Scan, TpchQ16Module>(connection,
                                                   TpchQ16Config{1, 2, 3});
  case 17:
    return MakeBinding<TpchQ17Scan, TpchQ17Module>(connection,
                                                   TpchQ17Config{1, 2});
  case 18:
    // Catalog probe.table=0 is lineitem; orders/customer must be distinct so
    // lineitem writes take the incremental path instead of MarkRebuild.
    return MakeBinding<TpchQ18Scan, TpchQ18Module>(connection,
                                                   TpchQ18Config{0, 1, 2});
  case 21:
    return MakeBinding<TpchQ21Scan, TpchQ21Module>(connection,
                                                   TpchQ21Config{1, 2, 3, 4});
  case 24:
    return MakeBinding<SecEdgarQ24Scan, SecEdgarQ24Module>(
        connection, SecEdgarQ24Config{101, 102});
  default:
    throw std::runtime_error("unsupported shared query binding");
  }
}

} // namespace

DuckdbRuntime::DuckdbRuntime(::duckdb::Connection &connection,
                             const SharedQueryCatalogEntry &entry,
                             double expected_queries_before_write)
    : connection_(connection), entry_(entry),
      state_store_(entry.accelerator_id, legacy_store_), coordinator_(),
      expected_queries_before_write_(
          std::make_shared<double>(expected_queries_before_write)),
      backend_(coordinator_, state_store_,
               [](const ::duckdb::TableMutationEvent &) {
                 return core::MutationBatch{};
               },
               [] { return core::VisibilityToken{}; },
               [entry] {
                 return core::LoadAuxCostModelJson(
                     harness::SharedCostModelConfigPath(entry.workload,
                                                        entry.query_id));
               },
               [expected = expected_queries_before_write_]() {
                 return *expected;
               }),
      binding_(CreateBinding(connection, entry)) {
  coordinator_.Register(std::shared_ptr<core::AuxQueryModule>(
      &binding_->Module(), [](core::AuxQueryModule *) {}));
}

void DuckdbRuntime::SetExpectedQueriesBeforeNextWrite(
    double expected_queries_before_write) {
  *expected_queries_before_write_ = expected_queries_before_write;
}

double DuckdbRuntime::ExpectedQueriesBeforeNextWrite() const {
  return *expected_queries_before_write_;
}

void DuckdbRuntime::Build() {
  Builder builder;
  binding_->Build(backend_, builder);
  Require(builder.finished, "shared DuckDB build did not finish");
  legacy_store_.InvalidateAll();
  for (const auto &image : builder.images) {
    legacy_store_.PublishPartition(image.partition, image.payload, 0);
  }
}

std::vector<core::AuxRow> DuckdbRuntime::ExecuteCollect() {
  return ExecuteCollect(ToCoreArguments(entry_.benchmark_arguments));
}

std::vector<core::AuxRow> DuckdbRuntime::ExecuteCollect(
    const core::QueryArguments &arguments) {
  MaterializeSink sink;
  binding_->Execute(backend_, arguments, sink);
  last_rows = sink.rows.size();
  return std::move(sink.rows);
}

void DuckdbRuntime::Execute() {
  Execute(ToCoreArguments(entry_.benchmark_arguments));
}

void DuckdbRuntime::Execute(const core::QueryArguments &arguments) {
  core::BeginAuxExecuteProfiling();
  auto rows = ExecuteCollect(arguments);
  core::EndAuxExecuteProfiling();
  last_rows = rows.size();
}

void DuckdbRuntime::Apply(SharedProbe &probe) {
  connection_.BeginTransaction();
  auto result = connection_.Query(probe.update_sql);
  Require(result != nullptr && !result->HasError(),
          result ? result->GetError() : "DuckDB write failed");
  backend_.CaptureMutation(BuildSharedMutation(entry_, probe));
  coordinator_.PreCommit();
  connection_.Commit();
  coordinator_.Commit();
  probe.current_value += probe.write_delta;
}

void DuckdbRuntime::ApplyRfChunk(int write_seq, int order_count) {
  if (entry_.workload != SharedWorkloadKind::Tpch) {
    throw std::runtime_error("ApplyRfChunk is only supported for TPC-H");
  }
  // Q11 does not watch orders/lineitem; RF would mutate the DB without
  // invalidating aux. Skip capture (and SQL) so RF sweeps stay meaningful.
  if (entry_.query_id == 11) {
    return;
  }
  connection_.BeginTransaction();
  const auto plan =
      NextTpchRfChunkPlan(write_seq, order_count, connection_);
  const auto applied =
      ApplyTpchRfChunkSql(connection_, entry_.query_id, plan);
  for (const auto &mutation : applied.mutations) {
    if (!mutation.old_rows.empty() || !mutation.new_rows.empty()) {
      backend_.CaptureMutation(mutation);
    }
  }
  coordinator_.PreCommit();
  connection_.Commit();
  coordinator_.Commit();
}

void DuckdbRuntime::ResetCalibrationState() {
  legacy_store_.InvalidateAll();
  coordinator_.Abort();
}

void DuckdbRuntime::InvalidateAll() { legacy_store_.InvalidateAll(); }

void DuckdbRuntime::ApplyMutationBatch(const core::MutationBatch &mutation) {
  backend_.CaptureMutation(mutation);
  coordinator_.PreCommit();
  coordinator_.Commit();
}

void DuckdbRuntime::ExecuteBaseQuery() {
  auto result = connection_.Query(entry_.base_sql);
  Require(result != nullptr && !result->HasError(),
          result ? result->GetError() : "DuckDB base query failed");
  for (::duckdb::idx_t row = 0; row < result->RowCount(); ++row) {
    for (::duckdb::idx_t column = 0; column < result->ColumnCount(); ++column) {
      (void)result->GetValue(column, row);
    }
  }
}

core::AuxStoreDiagnosticsSnapshot DuckdbRuntime::StoreDiagnosticsSnapshot() const {
  return core::AuxStoreDiagnosticsCollector::ThreadLocal().Snapshot(
      static_cast<uint64_t>(legacy_store_.ValidPartitionCount()),
      static_cast<uint64_t>(legacy_store_.TotalPartitionSlots()));
}

void DuckdbRuntime::ResetStoreDiagnosticsProfiling() {
  core::ResetAuxStoreDiagnosticsProfiling();
}

uint64_t DuckdbRuntime::AuxStateFootprintBytes() const {
  return harness::ComputeAuxStateFootprintBytes(legacy_store_,
                                                entry_.accelerator_id);
}

DuckdbRuntime::~DuckdbRuntime() = default;

} // namespace accelerator_codegen::aux_state::engine::duckdb
