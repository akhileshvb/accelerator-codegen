// GENERATED BACKEND
#include "accelerator_codegen/runtime/duckdb/duckdb_runtime.hpp"
#include "accelerator_codegen/runtime/duckdb/tpch_rf.hpp"
#include "duckdb/main/attached_database.hpp"
#include <map>
#include "duckdb/accelerator/mutation_recorder.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/storage/object_cache.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

#include <cmath>
#include <limits>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "sec_edgar/q1/duckdb/aux/module.hpp"
#include "sec_edgar/q2/duckdb/aux/module.hpp"
#include "sec_edgar/q3/duckdb/aux/module.hpp"
#include "sec_edgar/q4/duckdb/aux/module.hpp"
#include "sec_edgar/q6/duckdb/aux/module.hpp"
#include "sec_edgar/q24/duckdb/aux/module.hpp"
#ifdef ACCEL_ENABLE_PARALLEL_AUX
#include "sec_edgar/q1/duckdb/aux/parallel/module.hpp"
#include "sec_edgar/q2/duckdb/aux/parallel/module.hpp"
#include "sec_edgar/q3/duckdb/aux/parallel/module.hpp"
#include "sec_edgar/q4/duckdb/aux/parallel/module.hpp"
#include "sec_edgar/q6/duckdb/aux/parallel/module.hpp"
#include "sec_edgar/q24/duckdb/aux/parallel/module.hpp"
#endif
#include "tpch/q1/duckdb/aux/module.hpp"
#if defined(ACCEL_HAVE_Q21_VERUS)
#include "tpch/q21/verus/module.hpp"
#endif
#include "tpch/q4/duckdb/aux/module.hpp"
#include "tpch/q5/duckdb/aux/module.hpp"
#include "tpch/q13/duckdb/aux/module.hpp"
#include "tpch/q8/duckdb/aux/module.hpp"
#include "tpch/q7/duckdb/aux/module.hpp"
#include "tpch/q22/duckdb/aux/module.hpp"
#include "tpch/q3/duckdb/aux/module.hpp"
#include "tpch/q6/duckdb/aux/module.hpp"
#include "tpch/q10/duckdb/aux/module.hpp"
#include "tpch/q11/duckdb/aux/module.hpp"
#include "tpch/q12/duckdb/aux/module.hpp"
#include "tpch/q14/duckdb/aux/module.hpp"
#include "tpch/q16/duckdb/aux/module.hpp"
#include "tpch/q15/duckdb/aux/module.hpp"
#include "tpch/q17/duckdb/aux/module.hpp"
#include "tpch/q18/duckdb/aux/module.hpp"
#include "tpch/q19/duckdb/aux/module.hpp"
#include "tpch/q2/duckdb/aux/module.hpp"
#include "tpch/q20/duckdb/aux/module.hpp"
#include "tpch/q21/duckdb/aux/module.hpp"
#include "tpch/q9/duckdb/aux/module.hpp"
#include "accelerator_codegen/stateful/core/aux_calibration.hpp"
#include "accelerator_codegen/stateful/core/aux_maintenance_policy.hpp"
#include "accelerator_codegen/stateful/core/aux_execute_stats.hpp"
#include "accelerator_codegen/stateful/core/aux_maintenance_decision_log.hpp"
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

void CivilFromDays(int32_t z, int32_t &year, unsigned &month, unsigned &day) {
  z += 719468;
  const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  year = static_cast<int32_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  day = doy - (153 * mp + 2) / 5 + 1;
  month = mp + (mp < 10 ? 3 : -9);
  year += (month <= 2);
}

std::string SqlDateLiteral(int32_t days) {
  int32_t year = 0;
  unsigned month = 0, day = 0;
  CivilFromDays(days, year, month, day);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "DATE '%04d-%02u-%02u'", year, month, day);
  return buf;
}

std::string SqlDateInPredicate(const char *column, const std::set<int32_t> &days) {
  if (days.empty())
    return "FALSE";
  const auto lo = *days.begin();
  const auto hi = *days.rbegin();
  const auto span = static_cast<int64_t>(hi) - lo + 1;
  if (days.size() > 64 || span <= static_cast<int64_t>(days.size()) * 4) {
    return std::string(column) + " BETWEEN " + SqlDateLiteral(lo) + " AND " +
           SqlDateLiteral(hi);
  }
  std::ostringstream sql;
  sql << column << " IN (";
  bool first = true;
  for (const auto day : days) {
    if (!first)
      sql << ", ";
    first = false;
    sql << SqlDateLiteral(day);
  }
  sql << ")";
  return sql.str();
}

std::string SqlIntInPredicate(const char *column, const std::set<int32_t> &values) {
  if (values.empty())
    return "FALSE";
  std::ostringstream sql;
  sql << column << " IN (";
  bool first = true;
  for (const auto value : values) {
    if (!first)
      sql << ", ";
    first = false;
    sql << value;
  }
  sql << ")";
  return sql.str();
}

std::string SqlInt64InPredicate(const char *column,
                                const std::unordered_set<int64_t> &values) {
  if (values.empty())
    return "FALSE";
  std::ostringstream sql;
  sql << column << " IN (";
  bool first = true;
  for (const auto value : values) {
    if (!first)
      sql << ", ";
    first = false;
    sql << value;
  }
  sql << ")";
  return sql.str();
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

// Scans normally run on the runtime's (writer's) connection. A reader thread
// (ReaderScope from another connection) must not touch it: a ScanRedirect
// sends its scans to a private connection, opened on first use.
struct ScanRedirect {
  ScanRedirect(::duckdb::DatabaseInstance &db_p, std::atomic<uint64_t> &opened_p)
      : db(db_p), opened(opened_p), previous(current) {
    current = this;
  }
  ~ScanRedirect() { current = previous; }
  ::duckdb::Connection &Connection() {
    if (!connection) {
      connection = std::make_unique<::duckdb::Connection>(db);
      opened++;
    }
    return *connection;
  }
  static thread_local ScanRedirect *current;

private:
  ::duckdb::DatabaseInstance &db;
  std::atomic<uint64_t> &opened;
  ScanRedirect *previous;
  std::unique_ptr<::duckdb::Connection> connection;
};
thread_local ScanRedirect *ScanRedirect::current = nullptr;

// SQL this thread ran against the tables (a read that runs any answered from
// SQL, not from the state).
thread_local uint64_t t_table_scans = 0;

::duckdb::Connection &ScanConnection(::duckdb::Connection &connection) {
  ++t_table_scans;
  return ScanRedirect::current ? ScanRedirect::current->Connection() : connection;
}

template <typename Callback>
void ScanQuery(::duckdb::Connection &connection_p, const std::string &sql,
               Callback &&callback) {
  auto &connection = ScanConnection(connection_p);
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
void ScanQueryStreaming(::duckdb::Connection &connection_p, const std::string &sql,
                        Callback &&callback) {
  auto &connection = ScanConnection(connection_p);
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
  bool ScanDayGroupAggregatesForDays(
      const std::set<int32_t> &days,
      const std::function<void(const TpchQ1DayGroupAggregate &)> &consumer)
      override {
    if (days.empty())
      return true;
    const std::string sql =
        "SELECT l_shipdate, l_returnflag, l_linestatus, "
        "COUNT(*)::BIGINT, "
        "SUM(CAST(round(l_quantity * 100) AS BIGINT)), "
        "SUM(CAST(round(l_extendedprice * 100) AS BIGINT)), "
        "SUM(CAST(round(l_discount * 100) AS BIGINT)), "
        "SUM(CAST(round(l_extendedprice * (1 - l_discount) * 10000) AS BIGINT)), "
        "SUM(CAST(round(l_extendedprice * (1 - l_discount) * (1 + l_tax) * "
        "1000000) AS BIGINT)) "
        "FROM lineitem WHERE " +
        SqlDateInPredicate("l_shipdate", days) + " GROUP BY 1, 2, 3";
    ScanQueryStreaming(
        connection, sql,
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
  bool ScanSegmentJoin(
      const std::string &mktsegment,
      const std::function<void(const TpchQ3OrderInput &,
                               const TpchQ3LineitemInput &)> &consumer) override {
    if (mktsegment.empty())
      return true;
    std::ostringstream sql;
    sql << "SELECT o.o_orderkey, o.o_orderdate, o.o_custkey, o.o_shippriority, "
           "l.l_extendedprice, l.l_discount, l.l_shipdate "
           "FROM lineitem l "
           "JOIN orders o ON l.l_orderkey = o.o_orderkey "
           "JOIN customer c ON o.o_custkey = c.c_custkey "
           "WHERE c.c_mktsegment = '"
        << mktsegment << "'";
    ScanQueryStreaming(
        connection, sql.str(),
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    DateDays(ChunkValue(chunk, 1, row)),
                    SafeInt64(ChunkValue(chunk, 2, row)),
                    SafeInt32(ChunkValue(chunk, 3, row))},
                   {SafeInt64(ChunkValue(chunk, 0, row)),
                    DecimalRaw(ChunkValue(chunk, 4, row)), 2,
                    DecimalRaw(ChunkValue(chunk, 5, row)), 2,
                    DateDays(ChunkValue(chunk, 6, row))});
        });
    return true;
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
  void ScanLineitemsForMonths(
      const std::set<int32_t> &month_keys,
      const std::function<void(const TpchQ6LineitemInput &)> &consumer) override {
    if (month_keys.empty())
      return;
    std::ostringstream sql;
    sql << "SELECT l_shipdate, l_discount, l_quantity, l_extendedprice "
           "FROM lineitem WHERE (EXTRACT(year FROM l_shipdate)::INT * 12 + "
           "EXTRACT(month FROM l_shipdate)::INT - 1) IN (";
    bool first = true;
    for (const auto key : month_keys) {
      if (!first)
        sql << ", ";
      first = false;
      sql << key;
    }
    sql << ")";
    ScanQueryStreaming(
        connection, sql.str(),
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
  bool ScanReturnedCustomerRevenueForMonths(
      const std::set<int32_t> &months,
      const std::function<void(const TpchQ10ReturnedRevenueInput &)> &consumer)
      override {
    if (months.empty())
      return true;
    ScanQueryStreaming(
        connection,
        "SELECT CAST(year(o_orderdate) * 100 + month(o_orderdate) AS INTEGER), "
        "o_custkey, "
        "sum(CAST(round(l_extendedprice * (1 - l_discount) * 10000) AS BIGINT)) "
        "FROM lineitem "
        "JOIN orders ON l_orderkey = o_orderkey "
        "WHERE l_returnflag = 'R' AND " +
            SqlIntInPredicate(
                "(year(o_orderdate) * 100 + month(o_orderdate))", months) +
            " GROUP BY 1, 2",
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
  void ScanLineitemsForReceiptDays(
      const std::set<int32_t> &receipt_days,
      const std::function<void(const TpchQ12LineitemInput &)> &consumer)
      override {
    if (receipt_days.empty())
      return;
    ScanQueryStreaming(
        connection,
        "SELECT l_orderkey, l_shipmode, l_commitdate, l_receiptdate, "
        "l_shipdate FROM lineitem WHERE " +
            SqlDateInPredicate("l_receiptdate", receipt_days),
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

struct TpchQ4Scan : TpchQ4InitialScan {
  explicit TpchQ4Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanOrders(
      const std::function<void(const TpchQ4OrderInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT o_orderkey, o_orderdate, o_orderpriority FROM orders",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    DateDays(ChunkValue(chunk, 1, row)),
                    ChunkValue(chunk, 2, row).ToString()});
        });
  }
  void ScanLineitems(
      const std::function<void(const TpchQ4LineitemInput &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT l_orderkey, l_commitdate, l_receiptdate FROM lineitem",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    DateDays(ChunkValue(chunk, 1, row)),
                    DateDays(ChunkValue(chunk, 2, row))});
        });
  }
  ::duckdb::Connection &connection;
};

struct TpchQ22Scan : TpchQ22InitialScan {
  explicit TpchQ22Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanCustomers(
      const std::function<void(const TpchQ22CustomerInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT c_custkey, c_phone, c_acctbal FROM customer",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString(),
                    DecimalRaw(ChunkValue(chunk, 2, row)), 2});
        });
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
  void ScanLineitemsForMonths(
      const std::set<int32_t> &months,
      const std::function<void(const TpchQ14LineitemInput &)> &consumer)
      override {
    if (months.empty())
      return;
    std::ostringstream sql;
    sql << "SELECT l_partkey, l_extendedprice, l_discount, l_shipdate "
           "FROM lineitem WHERE (EXTRACT(year FROM l_shipdate)::INT * 100 + "
           "EXTRACT(month FROM l_shipdate)::INT) IN (";
    bool first = true;
    for (const auto month : months) {
      if (!first)
        sql << ", ";
      first = false;
      sql << month;
    }
    sql << ")";
    ScanQueryStreaming(
        connection, sql.str(),
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


struct TpchQ7Scan : TpchQ7InitialScan {
  explicit TpchQ7Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanNations(
      const std::function<void(const TpchQ7NationInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT n_nationkey, n_name FROM nation",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt32(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString()});
        });
  }
  void ScanSuppliers(
      const std::function<void(const TpchQ7SupplierInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT s_suppkey, s_nationkey FROM supplier",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt32(ChunkValue(chunk, 1, row))});
        });
  }
  void ScanCustomers(
      const std::function<void(const TpchQ7CustomerInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT c_custkey, c_nationkey FROM customer",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt32(ChunkValue(chunk, 1, row))});
        });
  }
  void ScanOrders(
      const std::function<void(const TpchQ7OrderInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT o_orderkey, o_custkey FROM orders",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt64(ChunkValue(chunk, 1, row))});
        });
  }
  void ScanLineitems(
      const std::function<void(const TpchQ7LineitemInput &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT l_orderkey, l_suppkey, l_extendedprice, l_discount, l_shipdate "
        "FROM lineitem",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt64(ChunkValue(chunk, 1, row)),
                    DecimalRaw(ChunkValue(chunk, 2, row)), 2,
                    DecimalRaw(ChunkValue(chunk, 3, row)), 2,
                    DateDays(ChunkValue(chunk, 4, row))});
        });
  }
  ::duckdb::Connection &connection;
};

struct TpchQ8Scan : TpchQ8InitialScan {
  explicit TpchQ8Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanParts(
      const std::function<void(const TpchQ8PartInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT p_partkey, p_type FROM part",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString()});
        });
  }
  void ScanSuppliers(
      const std::function<void(const TpchQ8SupplierInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT s_suppkey, s_nationkey FROM supplier",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt32(ChunkValue(chunk, 1, row))});
        });
  }
  void ScanCustomers(
      const std::function<void(const TpchQ8CustomerInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT c_custkey, c_nationkey FROM customer",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt32(ChunkValue(chunk, 1, row))});
        });
  }
  void ScanOrders(
      const std::function<void(const TpchQ8OrdersInput &)> &consumer) override {
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
      const std::function<void(const TpchQ8LineitemInput &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT l_partkey, l_orderkey, l_suppkey, l_extendedprice, l_discount "
        "FROM lineitem",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt64(ChunkValue(chunk, 1, row)),
                    SafeInt64(ChunkValue(chunk, 2, row)),
                    DecimalRaw(ChunkValue(chunk, 3, row)),
                    DecimalRaw(ChunkValue(chunk, 4, row))});
        });
  }
  void ScanNations(
      const std::function<void(const TpchQ8NationInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT n_nationkey, n_name, n_regionkey FROM nation",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt32(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString(),
                    SafeInt32(ChunkValue(chunk, 2, row))});
        });
  }
  void ScanRegions(
      const std::function<void(const TpchQ8RegionInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT r_regionkey, r_name FROM region",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt32(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString()});
        });
  }
  ::duckdb::Connection &connection;
};

struct TpchQ13Scan : TpchQ13InitialScan {
  explicit TpchQ13Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanCustomers(
      const std::function<void(const TpchQ13CustomerInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT c_custkey FROM customer",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row))});
        });
  }
  void ScanOrders(
      const std::function<void(const TpchQ13OrderInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT o_custkey, o_comment FROM orders",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString()});
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
  bool LookupSupplierNation(int64_t suppkey, int32_t &nationkey) override {
    bool found = false;
    ScanQueryStreaming(
        connection,
        "SELECT s_nationkey FROM supplier WHERE s_suppkey = " +
            std::to_string(suppkey),
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          nationkey = SafeInt32(ChunkValue(chunk, 0, row));
          found = true;
        });
    return found;
  }
  bool LookupOrderYear(int64_t orderkey, int32_t &year) override {
    bool found = false;
    ScanQueryStreaming(
        connection,
        "SELECT year(o_orderdate) FROM orders WHERE o_orderkey = " +
            std::to_string(orderkey),
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          year = SafeInt32(ChunkValue(chunk, 0, row));
          found = true;
        });
    return found;
  }
  bool LookupPartsuppCost(int64_t partkey, int64_t suppkey,
                          int64_t &supplycost_raw) override {
    bool found = false;
    ScanQueryStreaming(
        connection,
        "SELECT ps_supplycost FROM partsupp WHERE ps_partkey = " +
            std::to_string(partkey) + " AND ps_suppkey = " +
            std::to_string(suppkey),
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          supplycost_raw = DecimalRaw(ChunkValue(chunk, 0, row));
          found = true;
        });
    return found;
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

struct TpchQ15Scan : TpchQ15InitialScan {
  explicit TpchQ15Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanSuppliers(
      const std::function<void(const TpchQ15SupplierInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT s_suppkey, s_name, s_address, s_phone FROM supplier",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString(),
                    ChunkValue(chunk, 2, row).ToString(),
                    ChunkValue(chunk, 3, row).ToString()});
        });
  }
  void ScanLineitems(
      const std::function<void(const TpchQ15LineitemInput &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT l_suppkey, l_extendedprice, l_discount, l_shipdate FROM lineitem",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({DateDays(ChunkValue(chunk, 3, row)),
                    SafeInt64(ChunkValue(chunk, 0, row)),
                    DecimalRaw(ChunkValue(chunk, 1, row)),
                    2,
                    DecimalRaw(ChunkValue(chunk, 2, row)),
                    2});
        });
  }
  void ScanLineitemsForShipdateRange(
      int32_t start_days, int32_t end_days,
      const std::function<void(const TpchQ15LineitemInput &)> &consumer)
      override {
    if (end_days <= start_days)
      return;
    ScanQueryStreaming(
        connection,
        "SELECT l_suppkey, l_extendedprice, l_discount, l_shipdate FROM lineitem "
        "WHERE l_shipdate >= " +
            SqlDateLiteral(start_days) + " AND l_shipdate < " +
            SqlDateLiteral(end_days),
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({DateDays(ChunkValue(chunk, 3, row)),
                    SafeInt64(ChunkValue(chunk, 0, row)),
                    DecimalRaw(ChunkValue(chunk, 1, row)),
                    2,
                    DecimalRaw(ChunkValue(chunk, 2, row)),
                    2});
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
  bool ScanPartQuantityAggregatesForPart(
      int64_t partkey,
      const std::function<void(const TpchQ17PartQuantityAggregate &)> &consumer)
      override {
    if (std::getenv("ACCEL_Q17_BENCH_SCAN_LIMIT")) {
      return false;
    }
    ScanQueryStreaming(
        connection,
        "SELECT " + std::to_string(partkey) +
            "::BIGINT, "
            "CAST(round(l_quantity * 100) AS BIGINT), "
            "SUM(CAST(round(l_extendedprice * 100) AS BIGINT)), "
            "COUNT(*)::BIGINT "
            "FROM lineitem WHERE l_partkey = " +
            std::to_string(partkey) + " GROUP BY 2",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt64(ChunkValue(chunk, 1, row)),
                    SafeInt64(ChunkValue(chunk, 2, row)),
                    SafeInt64(ChunkValue(chunk, 3, row))});
        });
    return true;
  }
  bool ScanPartQuantityAggregatesForParts(
      const std::unordered_set<int64_t> &partkeys,
      const std::function<void(const TpchQ17PartQuantityAggregate &)> &consumer)
      override {
    if (partkeys.empty() || std::getenv("ACCEL_Q17_BENCH_SCAN_LIMIT"))
      return false;
    if (partkeys.size() == 1)
      return ScanPartQuantityAggregatesForPart(*partkeys.begin(), consumer);
    ScanQueryStreaming(
        connection,
        "SELECT l_partkey, "
        "CAST(round(l_quantity * 100) AS BIGINT), "
        "SUM(CAST(round(l_extendedprice * 100) AS BIGINT)), "
        "COUNT(*)::BIGINT "
        "FROM lineitem WHERE " +
            SqlInt64InPredicate("l_partkey", partkeys) +
            " GROUP BY 1, 2",
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
    const std::string min_qty =
        std::to_string(query::TpchQ18State::kMinPersistQuantityRaw);
    ScanQueryStreaming(
        connection,
        "SELECT l_orderkey, "
        "CAST(SUM(CAST(round(l_quantity * 100) AS BIGINT)) AS BIGINT) "
        "FROM lineitem GROUP BY 1 HAVING "
        "CAST(SUM(CAST(round(l_quantity * 100) AS BIGINT)) AS BIGINT) > " +
            min_qty,
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt64(ChunkValue(chunk, 1, row))});
        });
    return true;
  }
  bool ScanSurvivorOrders(
      int64_t min_quantity_raw,
      const std::function<void(const TpchQ18SurvivorAggregate &)> &consumer)
      override {
    ScanQueryStreaming(
        connection,
        "SELECT l.l_orderkey, "
        "CAST(SUM(CAST(round(l.l_quantity * 100) AS BIGINT)) AS BIGINT), "
        "o.o_custkey, o.o_orderdate, o.o_totalprice "
        "FROM lineitem l JOIN orders o ON l.l_orderkey = o.o_orderkey "
        "GROUP BY 1, 3, 4, 5 HAVING "
        "CAST(SUM(CAST(round(l.l_quantity * 100) AS BIGINT)) AS BIGINT) > " +
            std::to_string(min_quantity_raw),
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt64(ChunkValue(chunk, 1, row)),
                    SafeInt64(ChunkValue(chunk, 2, row)),
                    DateDays(ChunkValue(chunk, 3, row)),
                    DecimalRaw(ChunkValue(chunk, 4, row))});
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
  bool LookupOrderQuantitySum(int64_t orderkey, int64_t &sum_raw) override {
    sum_raw = 0;
    bool found = false;
    ScanQueryStreaming(
        connection,
        "SELECT COALESCE(SUM(CAST(round(l_quantity * 100) AS BIGINT)), 0) "
        "FROM lineitem WHERE l_orderkey = " +
            std::to_string(orderkey),
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          sum_raw = SafeInt64(ChunkValue(chunk, 0, row));
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


struct TpchQ20Scan : TpchQ20InitialScan {
  explicit TpchQ20Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanLineitems(
      const std::function<void(const TpchQ20LineitemInput &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT l_shipdate, l_partkey, l_suppkey, l_quantity FROM lineitem",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({DateDays(ChunkValue(chunk, 0, row)),
                    SafeInt64(ChunkValue(chunk, 1, row)),
                    SafeInt64(ChunkValue(chunk, 2, row)),
                    DecimalRaw(ChunkValue(chunk, 3, row))});
        });
  }
  void ScanParts(
      const std::function<void(const TpchQ20PartInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT p_partkey, p_name FROM part",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString()});
        });
  }
  void ScanPartsupps(
      const std::function<void(const TpchQ20PartsuppInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT ps_partkey, ps_suppkey, ps_availqty FROM partsupp",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt64(ChunkValue(chunk, 1, row)),
                    SafeInt64(ChunkValue(chunk, 2, row))});
        });
  }
  void ScanSuppliers(
      const std::function<void(const TpchQ20SupplierInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT s_suppkey, s_name, s_address, s_nationkey FROM supplier",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString(),
                    ChunkValue(chunk, 2, row).ToString(),
                    SafeInt32(ChunkValue(chunk, 3, row))});
        });
  }
  void ScanNations(
      const std::function<void(const TpchQ20NationInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT n_nationkey, n_name FROM nation",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt32(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString()});
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

#if defined(ACCEL_HAVE_Q21_VERUS)
// Scans for the Verus Q21 kernel.  Each implements the SQL its interface
// comment names (the kernel's trusted inputs).  All run on the runtime's
// connection, so inside a write transaction they see its own writes.
struct TpchQ21VerusDuckScan : TpchQ21VerusScan {
  explicit TpchQ21VerusDuckScan(::duckdb::Connection &connection)
      : connection(connection) {}
  static q21v_li_row Row(const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
    return q21v_li_row{SafeInt64(ChunkValue(chunk, 0, row)),
                       SafeInt64(ChunkValue(chunk, 1, row)),
                       DateDays(ChunkValue(chunk, 2, row)),
                       DateDays(ChunkValue(chunk, 3, row))};
  }
  void ScanSuppliers(const std::function<void(int64_t, const std::string &, int32_t)>
                         &consumer) override {
    ScanQueryStreaming(connection, "SELECT s_suppkey, s_name, s_nationkey FROM supplier",
                       [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
                         consumer(SafeInt64(ChunkValue(chunk, 0, row)),
                                  ChunkValue(chunk, 1, row).ToString(),
                                  SafeInt32(ChunkValue(chunk, 2, row)));
                       });
  }
  void ScanNations(
      const std::function<void(int32_t, const std::string &)> &consumer) override {
    ScanQueryStreaming(connection, "SELECT n_nationkey, n_name FROM nation",
                       [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
                         consumer(SafeInt32(ChunkValue(chunk, 0, row)),
                                  ChunkValue(chunk, 1, row).ToString());
                       });
  }
  void ScanFOrderKeys(const std::function<void(int64_t)> &consumer) override {
    ScanQueryStreaming(connection,
                       "SELECT o_orderkey FROM orders WHERE o_orderstatus = 'F'",
                       [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
                         consumer(SafeInt64(ChunkValue(chunk, 0, row)));
                       });
  }
  void ScanFLineitems(const std::function<void(const q21v_li_row &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT l.l_orderkey, l.l_suppkey, l.l_commitdate, l.l_receiptdate "
        "FROM lineitem l WHERE EXISTS (SELECT 1 FROM orders o "
        "WHERE o.o_orderkey = l.l_orderkey AND o.o_orderstatus = 'F')",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer(Row(chunk, row));
        });
  }
  void ScanLineitemsForKeys(const std::vector<int64_t> &keys,
                            const std::function<void(const q21v_li_row &)> &consumer)
      override {
    std::string sql = "SELECT l_orderkey, l_suppkey, l_commitdate, l_receiptdate "
                      "FROM lineitem WHERE l_orderkey IN (";
    for (std::size_t i = 0; i < keys.size(); ++i) {
      if (i)
        sql += ',';
      sql += std::to_string(keys[i]);
    }
    sql += ")";
    ScanQueryStreaming(connection, sql,
                       [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
                         consumer(Row(chunk, row));
                       });
  }
  void RunReference(const std::string &nation,
                    const std::function<void(const std::string &, int64_t)> &consumer)
      override {
    auto stmt = ScanConnection(connection).Prepare(
        "SELECT s_name, COUNT(*) AS numwait FROM supplier, lineitem l1, orders, "
        "nation WHERE s_suppkey = l1.l_suppkey AND o_orderkey = l1.l_orderkey AND "
        "o_orderstatus = 'F' AND l1.l_receiptdate > l1.l_commitdate AND EXISTS ("
        "SELECT * FROM lineitem l2 WHERE l2.l_orderkey = l1.l_orderkey AND "
        "l2.l_suppkey <> l1.l_suppkey) AND NOT EXISTS (SELECT * FROM lineitem l3 "
        "WHERE l3.l_orderkey = l1.l_orderkey AND l3.l_suppkey <> l1.l_suppkey AND "
        "l3.l_receiptdate > l3.l_commitdate) AND s_nationkey = n_nationkey AND "
        "n_name = $1 GROUP BY s_name ORDER BY numwait DESC, s_name LIMIT 100");
    Require(stmt && !stmt->HasError(), stmt ? stmt->GetError() : "Q21 prepare failed");
    // Execute(args...) streams by default; the cast below needs a materialized result.
    ::duckdb::vector<::duckdb::Value> params{::duckdb::Value(nation)};
    auto result = stmt->Execute(params, /*allow_stream_result=*/false);
    Require(result && !result->HasError(), result ? result->GetError() : "Q21 failed");
    auto &materialized = dynamic_cast<::duckdb::MaterializedQueryResult &>(*result);
    for (::duckdb::idx_t row = 0; row < materialized.RowCount(); ++row)
      consumer(materialized.GetValue(0, row).ToString(),
               SafeInt64(materialized.GetValue(1, row)));
  }
  ::duckdb::Connection &connection;
};
#endif

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

// Engine capture: the DuckDB table name of each table a TPC-H module's
// config names (fields `<table>_table`). Generated from the config structs.
template <typename... Config>
std::vector<std::pair<std::string, core::TableId>> CaptureTablesOf(const Config &...) {
  return {};
}
std::vector<std::pair<std::string, core::TableId>> CaptureTablesOf(const TpchQ1Config &c) {
  return {{"lineitem", c.lineitem_table}};
}
std::vector<std::pair<std::string, core::TableId>> CaptureTablesOf(const TpchQ2Config &c) {
  return {{"part", c.part_table}, {"supplier", c.supplier_table}, {"partsupp", c.partsupp_table}, {"nation", c.nation_table}, {"region", c.region_table}};
}
std::vector<std::pair<std::string, core::TableId>> CaptureTablesOf(const TpchQ3Config &c) {
  return {{"customer", c.customer_table}, {"orders", c.orders_table}, {"lineitem", c.lineitem_table}};
}
std::vector<std::pair<std::string, core::TableId>> CaptureTablesOf(const TpchQ4Config &c) {
  return {{"orders", c.orders_table}, {"lineitem", c.lineitem_table}};
}
std::vector<std::pair<std::string, core::TableId>> CaptureTablesOf(const TpchQ5Config &c) {
  return {{"region", c.region_table}, {"nation", c.nation_table}, {"customer", c.customer_table}, {"supplier", c.supplier_table}, {"orders", c.orders_table}, {"lineitem", c.lineitem_table}};
}
std::vector<std::pair<std::string, core::TableId>> CaptureTablesOf(const TpchQ6Config &c) {
  return {{"lineitem", c.lineitem_table}};
}
std::vector<std::pair<std::string, core::TableId>> CaptureTablesOf(const TpchQ7Config &c) {
  return {{"nation", c.nation_table}, {"supplier", c.supplier_table}, {"customer", c.customer_table}, {"orders", c.orders_table}, {"lineitem", c.lineitem_table}};
}
std::vector<std::pair<std::string, core::TableId>> CaptureTablesOf(const TpchQ8Config &c) {
  return {{"part", c.part_table}, {"supplier", c.supplier_table}, {"customer", c.customer_table}, {"orders", c.orders_table}, {"lineitem", c.lineitem_table}, {"nation", c.nation_table}, {"region", c.region_table}};
}
std::vector<std::pair<std::string, core::TableId>> CaptureTablesOf(const TpchQ9Config &c) {
  return {{"part", c.part_table}, {"supplier", c.supplier_table}, {"nation", c.nation_table}, {"orders", c.orders_table}, {"partsupp", c.partsupp_table}, {"lineitem", c.lineitem_table}};
}
std::vector<std::pair<std::string, core::TableId>> CaptureTablesOf(const TpchQ10Config &c) {
  return {{"customer", c.customer_table}, {"nation", c.nation_table}, {"orders", c.orders_table}, {"lineitem", c.lineitem_table}};
}
std::vector<std::pair<std::string, core::TableId>> CaptureTablesOf(const TpchQ11Config &c) {
  return {{"partsupp", c.partsupp_table}, {"supplier", c.supplier_table}, {"nation", c.nation_table}};
}
std::vector<std::pair<std::string, core::TableId>> CaptureTablesOf(const TpchQ12Config &c) {
  return {{"orders", c.orders_table}, {"lineitem", c.lineitem_table}};
}
std::vector<std::pair<std::string, core::TableId>> CaptureTablesOf(const TpchQ13Config &c) {
  return {{"customer", c.customer_table}, {"orders", c.orders_table}};
}
std::vector<std::pair<std::string, core::TableId>> CaptureTablesOf(const TpchQ14Config &c) {
  return {{"part", c.part_table}, {"lineitem", c.lineitem_table}};
}
std::vector<std::pair<std::string, core::TableId>> CaptureTablesOf(const TpchQ15Config &c) {
  return {{"lineitem", c.lineitem_table}, {"supplier", c.supplier_table}};
}
std::vector<std::pair<std::string, core::TableId>> CaptureTablesOf(const TpchQ16Config &c) {
  return {{"part", c.part_table}, {"partsupp", c.partsupp_table}, {"supplier", c.supplier_table}};
}
std::vector<std::pair<std::string, core::TableId>> CaptureTablesOf(const TpchQ17Config &c) {
  return {{"part", c.part_table}, {"lineitem", c.lineitem_table}};
}
std::vector<std::pair<std::string, core::TableId>> CaptureTablesOf(const TpchQ18Config &c) {
  return {{"lineitem", c.lineitem_table}, {"orders", c.orders_table}, {"customer", c.customer_table}};
}
std::vector<std::pair<std::string, core::TableId>> CaptureTablesOf(const TpchQ19Config &c) {
  return {{"lineitem", c.lineitem_table}, {"part", c.part_table}};
}
std::vector<std::pair<std::string, core::TableId>> CaptureTablesOf(const TpchQ20Config &c) {
  return {{"lineitem", c.lineitem_table}, {"part", c.part_table}, {"partsupp", c.partsupp_table}, {"supplier", c.supplier_table}, {"nation", c.nation_table}};
}
std::vector<std::pair<std::string, core::TableId>> CaptureTablesOf(const TpchQ21Config &c) {
  return {{"supplier", c.supplier_table}, {"orders", c.orders_table}, {"lineitem", c.lineitem_table}, {"nation", c.nation_table}};
}
std::vector<std::pair<std::string, core::TableId>> CaptureTablesOf(const TpchQ22Config &c) {
  return {{"orders", c.orders_table}, {"customer", c.customer_table}};
}
std::vector<std::pair<std::string, core::TableId>> CaptureTablesOf(const TpchQ21VerusConfig &c) {
  return {{"supplier", c.supplier_table}, {"orders", c.orders_table}, {"lineitem", c.lineitem_table}, {"nation", c.nation_table}};
}

template <typename Scan, typename QueryModule>
struct QueryBound : Scan, ModuleBinding<QueryModule> {
  template <typename... Args>
  QueryBound(::duckdb::Connection &connection, Args &&...args)
      : Scan(connection),
        ModuleBinding<QueryModule>(std::forward<Args>(args)...,
                                   static_cast<Scan *>(this)),
        tables(CaptureTablesOf(args...)) {} // configs are plain TableIds
  std::vector<std::pair<std::string, core::TableId>> CaptureTables() const override {
    return tables;
  }
  std::vector<std::pair<std::string, core::TableId>> tables;
};

template <typename Scan, typename QueryModule, typename... ConfigArgs>
std::unique_ptr<DuckdbRuntime::Binding>
MakeBinding(::duckdb::Connection &connection, ConfigArgs &&...config_args) {
  return std::unique_ptr<DuckdbRuntime::Binding>(
      new QueryBound<Scan, QueryModule>(connection,
                                        std::forward<ConfigArgs>(config_args)...));
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

std::unique_ptr<DuckdbRuntime::Binding>
CreateBinding(::duckdb::Connection &connection,
              const SharedQueryCatalogEntry &entry) {
#ifdef ACCEL_ENABLE_PARALLEL_AUX
  const bool parallel_aux = IsParallelAuxAccelerator(entry.accelerator_id);
#else
  constexpr bool parallel_aux = false;
  (void)parallel_aux;
#endif
  switch (entry.query_id) {
  case 1:
    if (entry.workload == SharedWorkloadKind::Tpch) {
      return MakeBinding<TpchQ1Scan, TpchQ1Module>(connection, TpchQ1Config{1});
    }
#ifdef ACCEL_ENABLE_PARALLEL_AUX
    if (parallel_aux) {
      return MakeBinding<SecEdgarQ1Scan, SecEdgarQ1ParallelModule>(
          connection, SecEdgarQ1Config{1});
    }
#endif
    return MakeBinding<SecEdgarQ1Scan, SecEdgarQ1Module>(connection,
                                                         SecEdgarQ1Config{1});
  case 2:
    if (entry.workload == SharedWorkloadKind::Tpch) {
      return MakeBinding<TpchQ2Scan, TpchQ2Module>(
          connection, TpchQ2Config{1, 2, 3, 4, 5});
    }
#ifdef ACCEL_ENABLE_PARALLEL_AUX
    if (parallel_aux) {
      return MakeBinding<SecEdgarQ2Scan, SecEdgarQ2ParallelModule>(
          connection, SecEdgarQ2Config{1, 2});
    }
#endif
    return MakeBinding<SecEdgarQ2Scan, SecEdgarQ2Module>(
        connection, SecEdgarQ2Config{1, 2});
  case 3:
    if (entry.workload == SharedWorkloadKind::Tpch) {
      return MakeBinding<TpchQ3Scan, TpchQ3Module>(connection,
                                                  TpchQ3Config{1, 2, 3});
    }
#ifdef ACCEL_ENABLE_PARALLEL_AUX
    if (parallel_aux) {
      return MakeBinding<SecEdgarQ3Scan, SecEdgarQ3ParallelModule>(
          connection, SecEdgarQ3Config{1, 2});
    }
#endif
    return MakeBinding<SecEdgarQ3Scan, SecEdgarQ3Module>(
        connection, SecEdgarQ3Config{1, 2});
  case 4:
    if (entry.workload == SharedWorkloadKind::Tpch) {
      return MakeBinding<TpchQ4Scan, TpchQ4Module>(connection,
                                                  TpchQ4Config{1, 2});
    }
#ifdef ACCEL_ENABLE_PARALLEL_AUX
    if (parallel_aux) {
      return MakeBinding<SecEdgarQ4Scan, SecEdgarQ4ParallelModule>(
          connection, SecEdgarQ4Config{1, 2, 3, 4});
    }
#endif
    return MakeBinding<SecEdgarQ4Scan, SecEdgarQ4Module>(
        connection, SecEdgarQ4Config{1, 2, 3, 4});
  case 5:
    return MakeBinding<TpchQ5Scan, TpchQ5Module>(
        connection, TpchQ5Config{1, 2, 3, 4, 5, 6});
  case 6:
    if (entry.workload == SharedWorkloadKind::Tpch) {
      return MakeBinding<TpchQ6Scan, TpchQ6Module>(connection, TpchQ6Config{1});
    }
#ifdef ACCEL_ENABLE_PARALLEL_AUX
    if (parallel_aux) {
      return MakeBinding<SecEdgarQ6Scan, SecEdgarQ6ParallelModule>(
          connection, SecEdgarQ6Config{1, 2, 3});
    }
#endif
    return MakeBinding<SecEdgarQ6Scan, SecEdgarQ6Module>(
        connection, SecEdgarQ6Config{1, 2, 3});
  case 7:
    return MakeBinding<TpchQ7Scan, TpchQ7Module>(
        connection, TpchQ7Config{0, 1, 2, 3, 4});
  case 8:
    return MakeBinding<TpchQ8Scan, TpchQ8Module>(
        connection, TpchQ8Config{0, 1, 2, 3, 4, 5, 6});
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
  case 13:
    return MakeBinding<TpchQ13Scan, TpchQ13Module>(connection,
                                                   TpchQ13Config{0, 1});
  case 14:
    return MakeBinding<TpchQ14Scan, TpchQ14Module>(connection,
                                                   TpchQ14Config{1, 2});
  case 15:
    return MakeBinding<TpchQ15Scan, TpchQ15Module>(connection,
                                                   TpchQ15Config{1, 2});
  case 16:
    return MakeBinding<TpchQ16Scan, TpchQ16Module>(connection,
                                                   TpchQ16Config{1, 2, 3});
  case 17:
    return MakeBinding<TpchQ17Scan, TpchQ17Module>(connection,
                                                   TpchQ17Config{1, 2});
  case 19:
    return MakeBinding<TpchQ19Scan, TpchQ19Module>(
        connection, TpchQ19Config{5, 6});
  case 18:
    // Catalog probe.table=0 is lineitem; orders/customer must be distinct so
    // lineitem writes take the incremental path instead of MarkRebuild.
    return MakeBinding<TpchQ18Scan, TpchQ18Module>(connection,
                                                   TpchQ18Config{0, 1, 2});
  case 20:
    return MakeBinding<TpchQ20Scan, TpchQ20Module>(
        connection, TpchQ20Config{1, 2, 3, 4, 5});
  case 21:
#if defined(ACCEL_HAVE_Q21_VERUS)
    if (std::string(entry.accelerator_id) == TpchQ21VerusLayout::kAccelerator)
      return MakeBinding<TpchQ21VerusDuckScan, TpchQ21VerusModule>(
          connection, TpchQ21VerusConfig{1, 2, 3, 4});
#endif
    return MakeBinding<TpchQ21Scan, TpchQ21Module>(connection,
                                                   TpchQ21Config{1, 2, 3, 4});
  case 22:
    return MakeBinding<TpchQ22Scan, TpchQ22Module>(connection,
                                                   TpchQ22Config{1, 2});
  case 24:
#ifdef ACCEL_ENABLE_PARALLEL_AUX
    if (parallel_aux) {
      return MakeBinding<SecEdgarQ24Scan, SecEdgarQ24ParallelModule>(
          connection, SecEdgarQ24Config{101, 102});
    }
#endif
    return MakeBinding<SecEdgarQ24Scan, SecEdgarQ24Module>(
        connection, SecEdgarQ24Config{101, 102});
  default:
    throw std::runtime_error("unsupported shared query binding");
  }
}

struct ActiveRuntimeKey {
  ::duckdb::DatabaseInstance *db = nullptr;
  std::string accelerator_id;

  bool operator==(const ActiveRuntimeKey &other) const {
    return db == other.db && accelerator_id == other.accelerator_id;
  }
};

struct ActiveRuntimeKeyHash {
  std::size_t operator()(const ActiveRuntimeKey &key) const {
    return std::hash<::duckdb::DatabaseInstance *>()(key.db) ^
           (std::hash<std::string>()(key.accelerator_id) << 1);
  }
};

std::mutex &ActiveRuntimeMutex() {
  static std::mutex mutex;
  return mutex;
}

std::unordered_map<ActiveRuntimeKey, DuckdbRuntime *, ActiveRuntimeKeyHash> &
ActiveRuntimes() {
  static std::unordered_map<ActiveRuntimeKey, DuckdbRuntime *,
                            ActiveRuntimeKeyHash>
      runtimes;
  return runtimes;
}

void RegisterActiveRuntime(DuckdbRuntime *runtime,
                           ::duckdb::Connection &connection,
                           const SharedQueryCatalogEntry &entry) {
  if (!runtime || !entry.accelerator_id || !connection.context ||
      !connection.context->db) {
    return;
  }
  ActiveRuntimeKey key{connection.context->db.get(), entry.accelerator_id};
  std::lock_guard<std::mutex> lock(ActiveRuntimeMutex());
  ActiveRuntimes()[key] = runtime;
}

void UnregisterActiveRuntime(DuckdbRuntime *runtime) {
  if (!runtime) {
    return;
  }
  std::lock_guard<std::mutex> lock(ActiveRuntimeMutex());
  auto &runtimes = ActiveRuntimes();
  for (auto it = runtimes.begin(); it != runtimes.end();) {
    if (it->second == runtime) {
      it = runtimes.erase(it);
    } else {
      ++it;
    }
  }
}

DuckdbRuntime *FindActiveRuntimeImpl(::duckdb::DatabaseInstance &db,
                                     const std::string &accelerator_id) {
  ActiveRuntimeKey key{&db, accelerator_id};
  std::lock_guard<std::mutex> lock(ActiveRuntimeMutex());
  const auto it = ActiveRuntimes().find(key);
  return it == ActiveRuntimes().end() ? nullptr : it->second;
}

} // namespace

DuckdbRuntime *DuckdbRuntime::FindActive(::duckdb::DatabaseInstance &db,
                                         const std::string &accelerator_id) {
  return FindActiveRuntimeImpl(db, accelerator_id);
}

namespace {

// The oldest start time of an open transaction: aux versions older than the
// one it selects can be pruned.
DuckDBStateStore::FloorProvider OldestSnapshotFloor(::duckdb::ClientContext &context) {
  auto db = AuxCommitClock::DefaultDatabase(context);
  if (!db) {
    return {};
  }
  return [db]() -> core::Version {
    auto &manager = db->GetTransactionManager();
    if (!manager.IsDuckTransactionManager()) {
      return std::numeric_limits<core::Version>::max();
    }
    return manager.Cast<::duckdb::DuckTransactionManager>().LowestActiveStart();
  };
}

// The snapshot epoch from which a state built now is current: the start
// time of a transaction begun now (every commit so far is in it; no captured
// commit runs during the build, since commits go through this connection).
// GetLastCommit() is not on the start-time scale for a database opened from a
// file, so it cannot be used here.
core::Version BuildEpoch(::duckdb::Connection &connection) {
  if (connection.HasActiveTransaction()) {
    return DuckDBAuxBackend::SnapshotEpoch(*connection.context);
  }
  connection.BeginTransaction();
  const auto epoch = DuckDBAuxBackend::SnapshotEpoch(*connection.context);
  connection.Rollback();
  return epoch;
}

} // namespace

namespace {

// ---- Engine capture of writes ----
//
// The DuckDB fork's MutationRecorder reports each insert / update / delete a
// connection makes, with the columns the database's watch provider asks for,
// to every MutationConsumer registered on that connection.

// The watch provider: per table, the columns some runtime's module projects
// (union over runtimes, reference-counted). Registered in the database's
// object cache under the key MutationWatchProvider::TryGet looks up.
class AuxWatchedColumns final : public ::duckdb::MutationWatchProvider {
public:
  static constexpr const char *kKey = "accel_state_registry";
  static ::duckdb::string ObjectType() { return "MutationWatchProvider"; }

  void Add(const std::string &table, const std::vector<int32_t> &columns) {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto c : columns) {
      watched[table][c]++;
    }
  }
  void Remove(const std::string &table, const std::vector<int32_t> &columns) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = watched.find(table);
    if (it == watched.end()) {
      return;
    }
    for (auto c : columns) {
      auto col = it->second.find(c);
      if (col != it->second.end() && --col->second == 0) {
        it->second.erase(col);
      }
    }
    if (it->second.empty()) {
      watched.erase(it);
    }
  }
  bool TryGetWatchedColumns(::duckdb::TableCatalogEntry &table,
                            ::duckdb::vector<::duckdb::PhysicalIndex> &columns) override {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = watched.find(table.name);
    if (it == watched.end()) {
      return false;
    }
    columns.clear();
    for (const auto &c : it->second) {
      columns.emplace_back(static_cast<::duckdb::idx_t>(c.first));
    }
    return !columns.empty();
  }
  // Updates report the updated columns' new values plus the old values of
  // every watched column, so a capture can rebuild the old and new rows.
  bool TryGetUpdateOldColumns(::duckdb::TableCatalogEntry &table,
                              const ::duckdb::vector<::duckdb::PhysicalIndex> &updated,
                              ::duckdb::vector<::duckdb::PhysicalIndex> &old_columns) override {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = watched.find(table.name);
    if (it == watched.end()) {
      return false;
    }
    bool relevant = false;
    for (const auto &u : updated) {
      relevant = relevant || it->second.count(static_cast<int32_t>(u.index)) > 0;
    }
    if (!relevant) {
      return false;
    }
    old_columns.clear();
    for (const auto &c : it->second) {
      old_columns.emplace_back(static_cast<::duckdb::idx_t>(c.first));
    }
    return true;
  }

private:
  std::mutex mutex;
  std::unordered_map<std::string, std::map<int32_t, int>> watched;
};

AuxWatchedColumns &WatchedColumns(::duckdb::ClientContext &context) {
  auto &cache = ::duckdb::DatabaseInstance::GetDatabase(context).GetObjectCache();
  auto entry = cache.GetOrCreate<AuxWatchedColumns>(AuxWatchedColumns::kKey);
  return *entry;
}

// Forwards the connection's mutation events to one runtime.
class AuxCaptureConsumer final : public ::duckdb::MutationConsumer {
public:
  explicit AuxCaptureConsumer(DuckdbRuntime *runtime_p) : runtime(runtime_p) {}
  void OnTableMutation(::duckdb::ClientContext &,
                       const ::duckdb::TableMutationEvent &event) override {
    runtime->OnCapturedMutation(event);
  }

private:
  DuckdbRuntime *runtime;
};

// A captured DuckDB value as the modules read it (the same mapping as the
// PostgreSQL capture: integers Int64, dates AuxDate, decimals ScaledDecimal,
// doubles Double, everything else its text).
core::AuxValue CapturedValue(const ::duckdb::Value &value) {
  using ::duckdb::LogicalTypeId;
  const auto id = value.type().id();
  switch (id) {
  case LogicalTypeId::BOOLEAN:
    return value.IsNull() ? core::AuxValue::Null(core::AuxType::Boolean)
                          : core::AuxValue::Of(core::AuxType::Boolean, value.GetValue<bool>());
  case LogicalTypeId::TINYINT:
  case LogicalTypeId::SMALLINT:
  case LogicalTypeId::INTEGER:
  case LogicalTypeId::BIGINT:
    return value.IsNull() ? core::AuxValue::Null(core::AuxType::Int64)
                          : core::AuxValue::Of(core::AuxType::Int64, value.GetValue<int64_t>());
  case LogicalTypeId::DATE:
    return value.IsNull() ? core::AuxValue::Null(core::AuxType::Date)
                          : core::AuxValue::Of(core::AuxType::Date, core::AuxDate {DateDays(value)});
  case LogicalTypeId::FLOAT:
  case LogicalTypeId::DOUBLE:
    return value.IsNull() ? core::AuxValue::Null(core::AuxType::Double)
                          : core::AuxValue::Of(core::AuxType::Double, value.GetValue<double>());
  case LogicalTypeId::DECIMAL: {
    if (value.IsNull()) {
      return core::AuxValue::Null(core::AuxType::ScaledDecimal);
    }
    core::ScaledDecimal decimal;
    decimal.value = DecimalRaw(value);
    decimal.scale = static_cast<int32_t>(::duckdb::DecimalType::GetScale(value.type()));
    return core::AuxValue::Of(core::AuxType::ScaledDecimal, decimal);
  }
  default:
    return value.IsNull() ? core::AuxValue::Null(core::AuxType::String)
                          : core::AuxValue::Of(core::AuxType::String, value.ToString());
  }
}

} // namespace

void DuckdbRuntime::EnableMutationCapture() {
  if (capture_enabled_) {
    return;
  }
  auto &watched = WatchedColumns(*connection_.context);
  const auto tables = binding_->CaptureTables();
  for (const auto &dep : binding_->Module().Dependencies()) {
    for (const auto &table : tables) {
      if (table.second == dep.table) {
        watched.Add(table.first, dep.mutation_attnums);
      }
    }
  }
  connection_.context->registered_state->GetOrCreate<AuxCaptureConsumer>(
      std::string("accel_aux_capture:") + entry_.accelerator_id, this);
  capture_enabled_ = true;
}

void DuckdbRuntime::DisableMutationCapture() {
  if (!capture_enabled_) {
    return;
  }
  connection_.context->registered_state->Remove(std::string("accel_aux_capture:") +
                                                entry_.accelerator_id);
  auto &watched = WatchedColumns(*connection_.context);
  const auto tables = binding_->CaptureTables();
  for (const auto &dep : binding_->Module().Dependencies()) {
    for (const auto &table : tables) {
      if (table.second == dep.table) {
        watched.Remove(table.first, dep.mutation_attnums);
      }
    }
  }
  capture_enabled_ = false;
}

void DuckdbRuntime::OnCapturedMutation(const ::duckdb::TableMutationEvent &event) {
  core::TableId table_id = 0;
  bool known = false;
  for (const auto &table : binding_->CaptureTables()) {
    if (table.first == event.table_name) {
      table_id = table.second;
      known = true;
    }
  }
  if (!known) {
    return;
  }
  for (const auto &dep : binding_->Module().Dependencies()) {
    if (dep.table != table_id || dep.mutation_attnums.empty()) {
      continue;
    }
    core::MutationBatch batch;
    batch.table = table_id;
    batch.kind = event.kind == ::duckdb::TableMutationKind::INSERT   ? core::MutationKind::Insert
                 : event.kind == ::duckdb::TableMutationKind::DELETE ? core::MutationKind::Delete
                                                                     : core::MutationKind::Update;
    if (batch.kind == core::MutationKind::Update) {
      bool relevant = false;
      for (std::size_t j = 0; j < dep.mutation_attnums.size(); ++j) {
        for (const auto &u : event.updated_columns) {
          if (static_cast<int32_t>(u.index) == dep.mutation_attnums[j]) {
            batch.updated_columns.push_back(static_cast<core::ColumnId>(j));
            relevant = true;
          }
        }
      }
      if (!relevant) {
        continue;
      }
    }
    for (const auto &row : event.rows) {
      // Old values first (watched columns), then the updated columns' new
      // values: the new row is the old one with those replaced.
      std::unordered_map<int32_t, const ::duckdb::Value *> old_values, new_values;
      const std::size_t updated = event.updated_columns.size();
      const std::size_t olds = row.values.size() - (batch.kind == core::MutationKind::Update ? updated : 0);
      for (std::size_t i = 0; i < row.values.size(); ++i) {
        const auto column = static_cast<int32_t>(row.values[i].column_index.index);
        if (batch.kind == core::MutationKind::Update && i >= olds) {
          new_values[column] = &row.values[i].value;
        } else {
          old_values[column] = &row.values[i].value;
        }
      }
      auto project = [&](bool use_new) {
        core::AuxRow out;
        out.values.reserve(dep.mutation_attnums.size());
        for (auto attnum : dep.mutation_attnums) {
          const ::duckdb::Value *v = nullptr;
          if (use_new) {
            auto it = new_values.find(attnum);
            if (it != new_values.end()) {
              v = it->second;
            }
          }
          if (!v) {
            auto it = old_values.find(attnum);
            if (it != old_values.end()) {
              v = it->second;
            }
          }
          out.values.push_back(v ? CapturedValue(*v) : core::AuxValue::Null(core::AuxType::String));
        }
        return out;
      };
      switch (batch.kind) {
      case core::MutationKind::Insert:
        batch.new_rows.push_back(project(false));
        break;
      case core::MutationKind::Delete:
        batch.old_rows.push_back(project(false));
        break;
      case core::MutationKind::Update:
        batch.old_rows.push_back(project(false));
        batch.new_rows.push_back(project(true));
        break;
      }
    }
    if (!batch.old_rows.empty() || !batch.new_rows.empty()) {
      backend_.CaptureMutation(batch);
    }
  }
}

void DuckdbRuntime::BeginWrite() { connection_.BeginTransaction(); }

void DuckdbRuntime::ApplyWrite(const std::string &sql) {
  BeginWrite();
  auto result = connection_.Query(sql);
  if (!result || result->HasError()) {
    RollbackWrite();
    throw std::runtime_error(sql + ": " + (result ? result->GetError() : "null result"));
  }
  CommitWrite();
}

std::vector<core::CalibrationWrite> DuckdbRuntime::CalibrationWrites() const {
  return binding_->Module().CalibrationWrites();
}

core::CalibrationEngine::Timed
DuckdbCalibrationEngine::WriteThenRead(const std::string &sql,
                                       std::optional<maintenance::MaintenanceAction> forced) {
  core::SetForcedMaintenanceActionName(forced ? maintenance::MaintenanceActionName(*forced) : "");
  core::ClearMaintenanceDecisionLog();
  Timed timed;
  const auto start = std::chrono::steady_clock::now();
  try {
    runtime.ApplyWrite(sql);
    const auto read_start = std::chrono::steady_clock::now();
    runtime.Execute();
    timed.read_us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() -
                                                              read_start)
                        .count();
  } catch (...) {
    core::SetForcedMaintenanceActionName("");
    throw;
  }
  timed.us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
  core::SetForcedMaintenanceActionName("");
  timed.decision = core::LastMaintenanceDecision();
  return timed;
}

void DuckdbRuntime::RollbackWrite() {
  connection_.Rollback();
  coordinator_.Abort();
}

DuckdbRuntime::DuckdbRuntime(::duckdb::Connection &connection,
                             const SharedQueryCatalogEntry &entry,
                             double expected_queries_before_write)
    : connection_(connection), entry_(entry),
      clock_(connection.context->registered_state->GetOrCreate<AuxCommitClock>(
          "accel_aux_commit_clock", AuxCommitClock::DefaultDatabase(*connection.context))),
      state_store_(entry.accelerator_id,
                   [clock = clock_] { return clock->epoch.load(); },
                   OldestSnapshotFloor(*connection.context)),
      // Same epochs and floor as state_store_: the two stores differ only
      // in their protocol's implementation.
      verus_store_(VerusAuxStoreRequested()
                       ? MakeVerusAuxStore(entry.accelerator_id,
                                           [clock = clock_] { return clock->epoch.load(); },
                                           OldestSnapshotFloor(*connection.context))
                       : nullptr),
      coordinator_(),
      expected_queries_before_write_(
          std::make_shared<double>(expected_queries_before_write)),
      backend_(coordinator_, ActiveStore(),
               [](const ::duckdb::TableMutationEvent &) {
                 return core::MutationBatch{};
               },
               [] { return DuckDBAuxBackend::ReaderVisibility(); },
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
  backend_.SetExecutionContext(connection.context.get());
}

void DuckdbRuntime::CommitWrite() {
  // Readers whose transaction starts after this writer's may see the
  // commit: they wait for its publish. (The commit hook then moves the
  // epoch to the commit id + 1 for the publish.)
  clock_->epoch = DuckDBAuxBackend::SnapshotEpoch(*connection_.context) + 1;
  try {
    coordinator_.PreCommit();
    connection_.Commit();
  } catch (...) {
    coordinator_.Abort();
    throw;
  }
  coordinator_.Commit();
}

void DuckdbRuntime::SetExpectedQueriesBeforeNextWrite(
    double expected_queries_before_write) {
  *expected_queries_before_write_ = expected_queries_before_write;
}

double DuckdbRuntime::ExpectedQueriesBeforeNextWrite() const {
  return *expected_queries_before_write_;
}

void DuckdbRuntime::Build(bool register_active) {
  const auto build_epoch = BuildEpoch(connection_);
  Builder builder;
  binding_->Build(backend_, builder);
  Require(builder.finished, "shared DuckDB build did not finish");
  // Current from the snapshot the build read (no captured commit can run
  // meanwhile: commits go through this runtime's connection).
  if (verus_store_) {
    verus_store_->Install(std::move(builder.images), entry_.accelerator_id,
                          build_epoch);
  } else {
    state_store_.Install(std::move(builder.images), entry_.accelerator_id,
                         build_epoch);
  }
  if (register_active) {
    RegisterActiveRuntime(this, connection_, entry_);
  }
}

std::vector<core::AuxRow> DuckdbRuntime::ExecuteCollect() {
  return ExecuteCollect(ToCoreArguments(entry_.benchmark_arguments));
}

std::vector<core::AuxRow> DuckdbRuntime::ExecuteCollect(
    const core::QueryArguments &arguments) {
  AuxReadScope one_version; // every read in this query: one state version
  // Normally unused: commits publish valid state (ReadersCanBuildState).
  std::optional<ScanRedirect> redirect;
  auto *reader = DuckDBAuxBackend::CurrentReader();
  if (reader && reader != connection_.context.get()) {
    redirect.emplace(*connection_.context->db, reader_fallbacks);
  }
  MaterializeSink sink;
  t_table_scans = 0;
  binding_->Execute(backend_, arguments, sink);
  core::RecordAuxExecute(t_table_scans > 0 ? core::AuxExecutePath::Fallback
                                           : core::AuxExecutePath::Hit);
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
  core::ClearMaintenanceDecisionLog();
  const auto start = std::chrono::steady_clock::now();
  connection_.BeginTransaction();
  auto result = connection_.Query(ProbeWriteSql(probe));
  Require(result != nullptr && !result->HasError(),
          result ? result->GetError() : "DuckDB write failed");
  backend_.CaptureMutation(BuildSharedMutation(entry_, probe));
  CommitWrite();
  AdvanceProbe(probe);
  const auto end = std::chrono::steady_clock::now();
  core::RecordMaintenanceActualCostUs(static_cast<double>(
      std::chrono::duration_cast<std::chrono::microseconds>(end - start)
          .count()));
}

void DuckdbRuntime::ApplyBatch(std::vector<SharedProbe> &probes) {
  Require(!probes.empty(), "ApplyBatch requires at least one probe");
  core::ClearMaintenanceDecisionLog();
  const auto start = std::chrono::steady_clock::now();
  connection_.BeginTransaction();
  for (auto &probe : probes) {
    auto result = connection_.Query(ProbeWriteSql(probe));
    Require(result != nullptr && !result->HasError(),
            result ? result->GetError() : "DuckDB batch write failed");
    backend_.CaptureMutation(BuildSharedMutation(entry_, probe));
    AdvanceProbe(probe);
  }
  CommitWrite();
  const auto end = std::chrono::steady_clock::now();
  core::RecordMaintenanceActualCostUs(static_cast<double>(
      std::chrono::duration_cast<std::chrono::microseconds>(end - start)
          .count()));
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
  CommitWrite();
}

void DuckdbRuntime::ResetCalibrationState() {
  ActiveStore().Invalidate(core::AllPartitionsPlan());
  coordinator_.Abort();
}

void DuckdbRuntime::InvalidateAll() {
  ActiveStore().Invalidate(core::AllPartitionsPlan());
}

core::AuxStateStore &DuckdbRuntime::ActiveStore() {
  if (verus_store_) {
    return *verus_store_;
  }
  return state_store_;
}

std::size_t DuckdbRuntime::StateVersionCount() const {
  return verus_store_ ? verus_store_->TotalVersionCount()
                      : state_store_.TotalVersionCount();
}

std::size_t DuckdbRuntime::StaleInstallCount() const {
  return verus_store_ ? verus_store_->StaleInstalls() : 0;
}

void DuckdbRuntime::ApplyMutationBatch(const core::MutationBatch &mutation) {
  // No engine transaction: publish for every snapshot (epoch 0).
  clock_->epoch = 0;
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
  if (verus_store_) {
    return core::AuxStoreDiagnosticsCollector::ThreadLocal().Snapshot(
        static_cast<uint64_t>(verus_store_->ValidPartitionCount()),
        static_cast<uint64_t>(verus_store_->TotalPartitionCount()));
  }
  return core::AuxStoreDiagnosticsCollector::ThreadLocal().Snapshot(
      static_cast<uint64_t>(state_store_.ValidPartitionCount()),
      static_cast<uint64_t>(state_store_.TotalPartitionCount()));
}

void DuckdbRuntime::ResetStoreDiagnosticsProfiling() {
  core::ResetAuxStoreDiagnosticsProfiling();
}

uint64_t DuckdbRuntime::AuxStateFootprintBytes() const {
  if (verus_store_) {
    return 0; // the Verus store's states are not measured
  }
  return harness::ComputeAuxStateFootprintBytes(state_store_,
                                                entry_.accelerator_id);
}

DuckdbRuntime::~DuckdbRuntime() {
  UnregisterActiveRuntime(this);
  DisableMutationCapture();
}

} // namespace accelerator_codegen::aux_state::engine::duckdb
