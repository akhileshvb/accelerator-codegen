// GENERATED BACKEND
#include "backends/duckdb/api/sql_fallback.hpp"

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <vector>

#include "backends/duckdb/api/decimal128.hpp"
#include "backends/duckdb/api/scan.hpp"

namespace accelerator_codegen::aux_state::engine {

::duckdb::Value AuxValueToDuckdb(const core::AuxValue &value) {
  if (value.is_null) {
    return ::duckdb::Value();
  }
  return std::visit(
      [](const auto &payload) -> ::duckdb::Value {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, int64_t>) {
          return ::duckdb::Value::BIGINT(payload);
        } else if constexpr (std::is_same_v<Payload, uint64_t>) {
          return ::duckdb::Value::UBIGINT(payload);
        } else if constexpr (std::is_same_v<Payload, double>) {
          return ::duckdb::Value::DOUBLE(payload);
        } else if constexpr (std::is_same_v<Payload, core::ScaledDecimal>) {
          return ::duckdb::Value::DECIMAL(payload.value, 18, payload.scale); // any int64 payload
        } else if constexpr (std::is_same_v<Payload, core::ScaledDecimal128>) {
          return engine::duckdb::Decimal128ToValue(payload);
        } else if constexpr (std::is_same_v<Payload, core::AuxDate>) {
          ::duckdb::date_t date{};
          date.days = payload.days_since_epoch;
          return ::duckdb::Value::DATE(date);
        } else {
          return ::duckdb::Value(payload);
        }
      },
      value.payload);
}

namespace {

// "(literal)"; an integer keeps its type (ToSQLString quotes the extreme ones as text).
std::string SqlLiteral(const ::duckdb::Value &value) {
  if (!value.IsNull() && value.type().IsIntegral()) {
    return "(CAST(" + value.ToString() + " AS " + value.type().ToString() + "))";
  }
  return "(" + value.ToSQLString() + ")";
}

} // namespace

std::string WithLiterals(const std::string &sql, const ::duckdb::vector<::duckdb::Value> &values) {
  std::string out = sql;
  for (std::size_t n = values.size(); n >= 1; --n) {
    const std::string marker = "$" + std::to_string(n);
    const std::string literal = SqlLiteral(values[n - 1]);
    for (std::size_t at = out.find(marker); at != std::string::npos; at = out.find(marker, at + literal.size())) {
      const std::size_t after = at + marker.size();
      if (after < out.size() && std::isdigit(static_cast<unsigned char>(out[after]))) {
        at += marker.size() - literal.size(); // part of a longer $nn: skip
        continue;
      }
      out.replace(at, marker.size(), literal);
    }
  }
  return out;
}

namespace {

core::AuxValue AuxValueFromDuckdb(const ::duckdb::Value &value) {
  using ::duckdb::LogicalTypeId;
  const auto &type = value.type();
  switch (type.id()) {
  case LogicalTypeId::DECIMAL: {
    const auto scale = ::duckdb::DecimalType::GetScale(type);
    switch (type.InternalType()) {
    case ::duckdb::PhysicalType::INT16:
      return core::AuxValue::Of(core::AuxType::ScaledDecimal, core::ScaledDecimal{value.GetValueUnsafe<int16_t>(), scale});
    case ::duckdb::PhysicalType::INT32:
      return core::AuxValue::Of(core::AuxType::ScaledDecimal, core::ScaledDecimal{value.GetValueUnsafe<int32_t>(), scale});
    case ::duckdb::PhysicalType::INT64:
      return core::AuxValue::Of(core::AuxType::ScaledDecimal, core::ScaledDecimal{value.GetValueUnsafe<int64_t>(), scale});
    default:
      return duckdb::Decimal128Cell(value.GetValueUnsafe<::duckdb::hugeint_t>(), scale);
    }
  }
  case LogicalTypeId::DOUBLE:
  case LogicalTypeId::FLOAT:
    return core::AuxValue::Of(core::AuxType::Double, value.GetValue<double>());
  case LogicalTypeId::DATE:
    return core::AuxValue::Of(core::AuxType::Date,
                              core::AuxDate{::duckdb::Date::EpochDays(value.GetValue<::duckdb::date_t>())});
  case LogicalTypeId::VARCHAR:
    return core::AuxValue::Of(core::AuxType::String, value.ToString());
  default:
    return core::AuxValue::Of(core::AuxType::Int64, value.GetValue<int64_t>());
  }
}

core::AuxRow RowOf(const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
  core::AuxRow out;
  out.values.reserve(chunk.ColumnCount());
  for (::duckdb::idx_t column = 0; column < chunk.ColumnCount(); ++column) {
    const auto value = chunk.GetValue(column, row);
    out.values.push_back(value.IsNull() ? core::AuxValue::Null(core::AuxType::String)
                                        : AuxValueFromDuckdb(value));
  }
  return out;
}

} // namespace

namespace {
std::atomic<int> g_trap_override{-1}; // -1: the environment decides
} // namespace

void SetFallbackTrap(std::optional<bool> trapped) { g_trap_override = trapped ? int(*trapped) : -1; }

bool FallbackTrapped() {
  if (const int forced = g_trap_override.load(); forced >= 0) {
    return forced != 0;
  }
  const char *value = std::getenv("ACCEL_FALLBACK_TRAP");
  return value && *value && *value != '0';
}

std::string FallbackSql(const SharedQueryCatalogEntry &entry, const core::QueryArguments &arguments) {
  if (FallbackTrapped()) {
    throw std::runtime_error(std::string("ACCEL_FALLBACK_TRAP: fallback SQL would run for ") +
                             (entry.accelerator_id ? entry.accelerator_id : "?"));
  }
  // No arguments: the defaults the base query bakes in.
  const char *query = arguments.values.empty() ? entry.base_sql : entry.fallback_sql;
  if (!query) {
    return {};
  }
  ::duckdb::vector<::duckdb::Value> literals;
  for (const auto &argument : arguments.values) {
    literals.push_back(AuxValueToDuckdb(argument));
  }
  return WithLiterals(query, literals);
}

bool AnswerFromFallbackSql(::duckdb::Connection &connection, const SharedQueryCatalogEntry &entry,
                           const core::QueryArguments &arguments, core::AuxRowSink &sink) {
  const auto sql = FallbackSql(entry, arguments);
  if (sql.empty()) {
    return false;
  }
  auto lock = duckdb::scan::LockScan(connection);
  duckdb::scan::StreamChunks(duckdb::scan::ScanConnection(connection), sql, [&](::duckdb::DataChunk &chunk) {
    for (::duckdb::idx_t row = 0; row < chunk.size(); ++row) {
      sink.WriteRow(RowOf(chunk, row));
    }
  });
  return true;
}

} // namespace accelerator_codegen::aux_state::engine
