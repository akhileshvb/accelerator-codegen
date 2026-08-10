#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include "accelerator_codegen/runtime/postgres/scan_adapter.hpp"

#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/vector.hpp"

extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
}

#ifdef ERROR
#undef ERROR
#endif
#ifdef Max
#undef Max
#endif
#ifdef Min
#undef Min
#endif

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

constexpr int32_t kPostgresDateEpochOffset = 10957;
constexpr ::duckdb::idx_t kScanBatchSize = 2048;

std::string DatumText(Datum datum, Oid type_oid) {
  Oid output_function = InvalidOid;
  bool is_varlena = false;
  getTypeOutputInfo(type_oid, &output_function, &is_varlena);
  char *text = OidOutputFunctionCall(output_function, datum);
  std::string result(text ? text : "");
  if (text) {
    pfree(text);
  }
  return result;
}

std::string SqlLiteral(const ::duckdb::Value &value) {
  if (value.IsNull()) {
    return "NULL";
  }
  switch (value.type().id()) {
  case ::duckdb::LogicalTypeId::DATE: {
    const auto date = value.GetValue<::duckdb::date_t>();
    return "DATE '" + ::duckdb::Date::ToString(date) + "'";
  }
  case ::duckdb::LogicalTypeId::VARCHAR:
  case ::duckdb::LogicalTypeId::CHAR: {
    std::string quoted = "'";
    const auto text = value.ToString();
    for (const char ch : text) {
      if (ch == '\'') {
        quoted += "''";
      }
      quoted += ch;
    }
    quoted += "'";
    return quoted;
  }
  default:
    return value.ToString();
  }
}

std::string ComparisonOperator(::duckdb::ExpressionType comparison) {
  switch (comparison) {
  case ::duckdb::ExpressionType::COMPARE_EQUAL:
    return "=";
  case ::duckdb::ExpressionType::COMPARE_NOTEQUAL:
    return "<>";
  case ::duckdb::ExpressionType::COMPARE_LESSTHAN:
    return "<";
  case ::duckdb::ExpressionType::COMPARE_GREATERTHAN:
    return ">";
  case ::duckdb::ExpressionType::COMPARE_LESSTHANOREQUALTO:
    return "<=";
  case ::duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO:
    return ">=";
  default:
    throw std::runtime_error("unsupported native scan filter comparison");
  }
}

std::string BuildScanSql(const NativeScanSpec &scan_spec) {
  std::ostringstream sql;
  sql << "SELECT ";
  for (::duckdb::idx_t column_index = 0;
       column_index < scan_spec.projected_columns.size(); ++column_index) {
    if (column_index != 0) {
      sql << ", ";
    }
    sql << scan_spec.projected_columns[column_index].source_name;
  }
  sql << " FROM " << scan_spec.table_name;
  if (!scan_spec.filters.empty()) {
    sql << " WHERE ";
    for (::duckdb::idx_t filter_index = 0; filter_index < scan_spec.filters.size();
         ++filter_index) {
      if (filter_index != 0) {
        sql << " AND ";
      }
      const auto &filter = scan_spec.filters[filter_index];
      if (filter.comparison_type ==
          ::duckdb::ExpressionType::OPERATOR_IS_NOT_NULL) {
        sql << filter.source_name << " IS NOT NULL";
        continue;
      }
      sql << filter.source_name << ' ' << ComparisonOperator(filter.comparison_type)
          << ' ' << SqlLiteral(filter.constant);
    }
  }
  return sql.str();
}

void AppendFlatValue(::duckdb::Vector &vector, ::duckdb::idx_t row_index,
                     HeapTuple tuple, TupleDesc tuple_desc, int attribute,
                     const NativeColumnSpec &column) {
  bool is_null = false;
  const Datum datum =
      SPI_getbinval(tuple, tuple_desc, attribute + 1, &is_null);
  if (is_null) {
    ::duckdb::FlatVector::SetNull(vector, row_index, true);
    return;
  }
  const Oid type_oid = TupleDescAttr(tuple_desc, attribute)->atttypid;
  switch (column.expected_physical_type) {
  case ::duckdb::PhysicalType::INT32: {
    int32_t days = 0;
    if (type_oid == DATEOID) {
      days = DatumGetInt32(datum) + kPostgresDateEpochOffset;
    } else {
      days = static_cast<int32_t>(std::stoll(DatumText(datum, type_oid)));
    }
    ::duckdb::FlatVector::GetData<int32_t>(vector)[row_index] = days;
    return;
  }
  case ::duckdb::PhysicalType::INT64: {
    int64_t value = 0;
    if (type_oid == INT8OID) {
      value = DatumGetInt64(datum);
    } else if (type_oid == INT4OID) {
      value = DatumGetInt32(datum);
    } else if (type_oid == NUMERICOID) {
      const double numeric = DatumGetFloat8(
          DirectFunctionCall1(numeric_float8, datum));
      value = static_cast<int64_t>(std::llround(numeric * 100.0));
    } else {
      value = std::stoll(DatumText(datum, type_oid));
    }
    ::duckdb::FlatVector::GetData<int64_t>(vector)[row_index] = value;
    return;
  }
  case ::duckdb::PhysicalType::DOUBLE: {
    double value = 0.0;
    if (type_oid == FLOAT8OID) {
      value = DatumGetFloat8(datum);
    } else if (type_oid == FLOAT4OID) {
      value = DatumGetFloat4(datum);
    } else if (type_oid == NUMERICOID) {
      value = DatumGetFloat8(DirectFunctionCall1(numeric_float8, datum));
    } else {
      value = std::stod(DatumText(datum, type_oid));
    }
    ::duckdb::FlatVector::GetData<double>(vector)[row_index] = value;
    return;
  }
  case ::duckdb::PhysicalType::VARCHAR: {
    const std::string text = DatumText(datum, type_oid);
    ::duckdb::FlatVector::GetData<::duckdb::string_t>(vector)[row_index] =
        ::duckdb::StringVector::AddString(vector, text);
    return;
  }
  default:
    throw std::runtime_error("unsupported postgres scan column physical type");
  }
}

void FillChunkFromTuples(const NativeScanSpec &scan_spec, TupleDesc tuple_desc,
                         ::duckdb::DataChunk &chunk) {
  const auto row_count = static_cast<::duckdb::idx_t>(SPI_processed);
  chunk.Reset();
  chunk.SetCardinality(row_count);
  for (::duckdb::idx_t column_index = 0;
       column_index < scan_spec.projected_columns.size(); ++column_index) {
    auto &vector = chunk.data[column_index];
    vector.SetVectorType(::duckdb::VectorType::FLAT_VECTOR);
    for (::duckdb::idx_t row_index = 0; row_index < row_count; ++row_index) {
      AppendFlatValue(vector, row_index, SPI_tuptable->vals[row_index],
                      tuple_desc, static_cast<int>(column_index),
                      scan_spec.projected_columns[column_index]);
    }
  }
}

} // namespace

void ScanNativeSpec(const NativeScanSpec &scan_spec,
                    const std::function<void(::duckdb::DataChunk &)> &consumer) {
  const std::string sql = BuildScanSql(scan_spec);
  if (SPI_connect() != SPI_OK_CONNECT) {
    throw std::runtime_error("SPI_connect failed");
  }
  SPIPlanPtr plan = SPI_prepare(sql.c_str(), 0, nullptr);
  if (!plan) {
    SPI_finish();
    throw std::runtime_error("SPI_prepare failed for native scan");
  }
  Portal portal = SPI_cursor_open(nullptr, plan, nullptr, nullptr, true);
  if (!portal) {
    SPI_freeplan(plan);
    SPI_finish();
    throw std::runtime_error("SPI_cursor_open failed for native scan");
  }

  ::duckdb::vector<::duckdb::LogicalType> types;
  types.reserve(scan_spec.projected_columns.size());
  for (const auto &column : scan_spec.projected_columns) {
    types.push_back(column.expected_logical_type);
  }
  ::duckdb::DataChunk chunk;
  chunk.Initialize(::duckdb::Allocator::DefaultAllocator(), types);

  for (;;) {
    SPI_cursor_fetch(portal, true, kScanBatchSize);
    if (SPI_processed == 0) {
      if (SPI_tuptable) {
        SPI_freetuptable(SPI_tuptable);
      }
      break;
    }
    FillChunkFromTuples(scan_spec, SPI_tuptable->tupdesc, chunk);
    consumer(chunk);
    SPI_freetuptable(SPI_tuptable);
  }

  SPI_cursor_close(portal);
  SPI_freeplan(plan);
  SPI_finish();
}

} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
