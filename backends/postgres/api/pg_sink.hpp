// GENERATED BACKEND
//
// Rows of an aux result (core::AuxRow) into a Postgres materialized SRF
// result, converting each AuxValue to the Datum of the column's SQL type. Used
// by the reader for rows served by a shard worker and for rows its own SQL
// fallback produced.
#pragma once

#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include <array>
#include <string>
#include <variant>

#include "substrate/aux/core/aux_types.hpp"

extern "C" {
#include "postgres.h"
#include "access/tupdesc.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/numeric.h"
#include "utils/tuplestore.h"
}

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {

namespace core = accelerator_codegen::aux_state::core;

// AuxDate uses Unix-epoch days, while PostgreSQL's DateADT uses days since
// 2000-01-01.
constexpr int32_t kPostgresDateEpochOffset = 10957;

class PgSink final : public core::AuxRowSink {
public:
  PgSink(Tuplestorestate *tuplestore_p, TupleDesc tuple_desc_p)
      : tuplestore(tuplestore_p), tuple_desc(tuple_desc_p) {}

  void Begin(const core::OutputSchema &) override {}

  void WriteRow(const core::AuxRow &row) override {
    std::array<Datum, 12> values{};
    std::array<bool, 12> nulls{};
    if (row.values.size() > values.size()) {
      ereport(ERROR, (errmsg("auxiliary output row exceeds PostgreSQL schema")));
    }
    for (std::size_t column = 0; column < row.values.size(); ++column) {
      if (row.values[column].is_null) {
        nulls[column] = true;
        continue;
      }
      values[column] = ToDatum(
          row.values[column], TupleDescAttr(tuple_desc, column)->atttypid);
    }
    tuplestore_putvalues(tuplestore, tuple_desc, values.data(), nulls.data());
  }

  void End() override {}

private:
  static std::string DecimalText(const core::ScaledDecimal &decimal) {
    return core::ScaledDecimal128ToString(decimal.value, decimal.scale);
  }

  static Datum DecimalDatum(const std::string &text, Oid type_oid) {
    if (type_oid == NUMERICOID) {
      return DirectFunctionCall3(
          numeric_in, CStringGetDatum(text.c_str()),
          ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
    }
    return Float8GetDatum(std::stod(text));
  }

  static Datum ToDatum(const core::AuxValue &value, Oid type_oid) {
    if (std::holds_alternative<int64_t>(value.payload)) {
      return Int64GetDatum(std::get<int64_t>(value.payload));
    }
    if (std::holds_alternative<double>(value.payload)) {
      const Datum as_float8 =
          Float8GetDatum(std::get<double>(value.payload));
      // Q7 (and similar) emit Double while the SQL function returns numeric.
      // A float8 Datum is not a valid Numeric pointer and SIGSEGVs on output.
      if (type_oid == NUMERICOID) {
        return DirectFunctionCall1(float8_numeric, as_float8);
      }
      return as_float8;
    }
    if (std::holds_alternative<core::ScaledDecimal>(value.payload)) {
      return DecimalDatum(
          DecimalText(std::get<core::ScaledDecimal>(value.payload)), type_oid);
    }
    if (const auto *wide = std::get_if<core::ScaledDecimal128>(&value.payload)) {
      return DecimalDatum(
          core::ScaledDecimal128ToString(wide->value, wide->scale), type_oid);
    }
    if (std::holds_alternative<core::AuxDate>(value.payload)) {
      return DateADTGetDatum(std::get<core::AuxDate>(value.payload)
                                 .days_since_epoch - kPostgresDateEpochOffset);
    }
    if (std::holds_alternative<std::string>(value.payload)) {
      const auto &text = std::get<std::string>(value.payload);
      // Exact numeric results are emitted as numeric text (pg_numeric).
      if (type_oid == NUMERICOID) {
        return DirectFunctionCall3(
            numeric_in, CStringGetDatum(text.c_str()),
            ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
      }
      return CStringGetTextDatum(text.c_str());
    }
    return (Datum)0;
  }

  Tuplestorestate *tuplestore;
  TupleDesc tuple_desc;
};

} // namespace detail
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
