// GENERATED BACKEND
//
// Answers a query from SQL in the reader's own transaction: the catalog
// entry's fallback_sql with the call's arguments as typed parameters
// ($n), run through SPI at the statement's snapshot (so the transaction's own
// writes are seen), the rows converted to core::AuxRow by column type.
#pragma once

#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "backends/postgres/api/catalog/query_catalog.hpp"
#include "backends/postgres/api/pg_sink.hpp"
#include "backends/postgres/state/shared.hpp"
#include "substrate/aux/core/aux_types.hpp"

extern "C" {
#include "executor/spi.h"
#include "nodes/parsenodes.h"
#include "utils/date.h"
#include "utils/lsyscache.h"
}

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {

struct SpiParameters {
  std::vector<Oid> types;
  std::vector<Datum> values;
  std::vector<char> nulls; // ' ' or 'n', as SPI wants
};

inline Datum NumericFromText(const std::string &text) {
  return DirectFunctionCall3(numeric_in, CStringGetDatum(text.c_str()), ObjectIdGetDatum(InvalidOid),
                             Int32GetDatum(-1));
}

inline SpiParameters ToSpiParameters(const core::QueryArguments &arguments) {
  SpiParameters out;
  for (const auto &argument : arguments.values) {
    Oid type = TEXTOID;
    Datum value = 0;
    if (!argument.is_null) {
      const auto &payload = argument.payload;
      if (const auto *i = std::get_if<int64_t>(&payload)) {
        type = INT8OID;
        value = Int64GetDatum(*i);
      } else if (const auto *u = std::get_if<uint64_t>(&payload)) {
        type = INT8OID;
        value = Int64GetDatum(static_cast<int64_t>(*u));
      } else if (const auto *d = std::get_if<double>(&payload)) {
        type = FLOAT8OID;
        value = Float8GetDatum(*d);
      } else if (const auto *dec = std::get_if<core::ScaledDecimal>(&payload)) {
        type = NUMERICOID;
        value = NumericFromText(core::ScaledDecimal128ToString(dec->value, dec->scale));
      } else if (const auto *wide = std::get_if<core::ScaledDecimal128>(&payload)) {
        type = NUMERICOID;
        value = NumericFromText(core::ScaledDecimal128ToString(wide->value, wide->scale));
      } else if (const auto *date = std::get_if<core::AuxDate>(&payload)) {
        type = DATEOID;
        value = DateADTGetDatum(date->days_since_epoch - kPostgresDateEpochOffset);
      } else if (const auto *flag = std::get_if<bool>(&payload)) {
        type = BOOLOID;
        value = BoolGetDatum(*flag);
      } else {
        value = CStringGetTextDatum(std::get<std::string>(payload).c_str());
      }
    }
    out.types.push_back(type);
    out.values.push_back(value);
    out.nulls.push_back(argument.is_null ? 'n' : ' ');
  }
  return out;
}

// "-12.340" -> {-12340, 3}.
inline core::AuxValue NumericCell(const char *text) {
  __int128 value = 0;
  int scale = 0;
  bool negative = false, fraction = false;
  for (const char *p = text; *p; ++p) {
    if (*p == '-') {
      negative = true;
    } else if (*p == '.') {
      fraction = true;
    } else if (*p >= '0' && *p <= '9') {
      value = value * 10 + (*p - '0');
      scale += fraction;
    } else {
      throw std::runtime_error(std::string("numeric result not representable: ") + text);
    }
  }
  return core::AuxValue::Of(core::AuxType::ScaledDecimal128,
                            core::ScaledDecimal128{negative ? -value : value, scale});
}

inline core::AuxValue CellOf(HeapTuple tuple, TupleDesc desc, int column) {
  bool is_null = false;
  const Datum datum = SPI_getbinval(tuple, desc, column + 1, &is_null);
  const Oid type = TupleDescAttr(desc, column)->atttypid;
  if (is_null) {
    return core::AuxValue::Null(core::AuxType::String);
  }
  switch (type) {
  case INT2OID:
    return core::AuxValue::Of(core::AuxType::Int64, static_cast<int64_t>(DatumGetInt16(datum)));
  case INT4OID:
    return core::AuxValue::Of(core::AuxType::Int64, static_cast<int64_t>(DatumGetInt32(datum)));
  case INT8OID:
    return core::AuxValue::Of(core::AuxType::Int64, DatumGetInt64(datum));
  case FLOAT4OID:
    return core::AuxValue::Of(core::AuxType::Double, static_cast<double>(DatumGetFloat4(datum)));
  case FLOAT8OID:
    return core::AuxValue::Of(core::AuxType::Double, DatumGetFloat8(datum));
  case DATEOID:
    return core::AuxValue::Of(core::AuxType::Date,
                              core::AuxDate{DatumGetDateADT(datum) + kPostgresDateEpochOffset});
  case BOOLOID:
    return core::AuxValue::Of(core::AuxType::Boolean, DatumGetBool(datum));
  case NUMERICOID: {
    char *text = SPI_getvalue(tuple, desc, column + 1);
    const auto cell = NumericCell(text);
    pfree(text);
    return cell;
  }
  default: {
    char *text = SPI_getvalue(tuple, desc, column + 1);
    std::string out(text);
    pfree(text);
    return core::AuxValue::Of(core::AuxType::String, std::move(out));
  }
  }
}

// Runs `sql` with `arguments` as its $n parameters in this transaction and
// writes the rows to `sink`.
inline void AnswerFromPostgresSql(const char *sql, const core::QueryArguments &arguments,
                                  core::AuxRowSink &sink) {
  auto parameters = ToSpiParameters(arguments);
  if (SPI_connect() != SPI_OK_CONNECT) {
    throw std::runtime_error("SPI_connect failed");
  }
  // Prepared with CURSOR_OPT_PARALLEL_OK (SPI_execute_with_args plans serially):
  // the query runs with the parallelism its base SQL would get.
  SPIPlanPtr plan = SPI_prepare_cursor(sql, static_cast<int>(parameters.types.size()),
                                       parameters.types.data(), CURSOR_OPT_PARALLEL_OK);
  if (!plan) {
    SPI_finish();
    throw std::runtime_error("fallback SQL did not prepare: " + std::string(SPI_result_code_string(SPI_result)));
  }
  const int status =
      SPI_execute_plan(plan, parameters.values.data(), parameters.nulls.data(), /*read_only=*/true, 0);
  SPI_freeplan(plan);
  if (status != SPI_OK_SELECT) {
    SPI_finish();
    throw std::runtime_error("fallback SQL failed: " + std::string(SPI_result_code_string(status)));
  }
  TupleDesc desc = SPI_tuptable->tupdesc;
  for (uint64 row = 0; row < SPI_processed; ++row) {
    core::AuxRow out;
    out.values.reserve(desc->natts);
    for (int column = 0; column < desc->natts; ++column) {
      out.values.push_back(CellOf(SPI_tuptable->vals[row], desc, column));
    }
    sink.WriteRow(out);
  }
  SPI_freetuptable(SPI_tuptable);
  SPI_finish();
}

} // namespace detail
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
