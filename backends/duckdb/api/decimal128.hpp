// GENERATED BACKEND
#pragma once

// hugeint_t <-> __int128 for the 128-bit decimal cell (core::ScaledDecimal128),
// and the exact DuckDB DECIMAL(38, scale) value of such a cell.

#include <cstdint>

#include "substrate/aux/core/aux_types.hpp"
#include "duckdb/common/hugeint.hpp"
#include "duckdb/common/types/value.hpp"

namespace accelerator_codegen::aux_state::engine::duckdb {

inline __int128 HugeintToInt128(const ::duckdb::hugeint_t &h) {
  return static_cast<__int128>((static_cast<unsigned __int128>(static_cast<uint64_t>(h.upper)) << 64) |
                               static_cast<unsigned __int128>(h.lower));
}

inline ::duckdb::hugeint_t Int128ToHugeint(__int128 v) {
  return ::duckdb::hugeint_t(static_cast<int64_t>(static_cast<unsigned __int128>(v) >> 64),
                             static_cast<uint64_t>(v));
}

inline bool HugeintFitsInt64(const ::duckdb::hugeint_t &h) {
  return (h.upper == 0 && static_cast<int64_t>(h.lower) >= 0) ||
         (h.upper == -1 && static_cast<int64_t>(h.lower) < 0);
}

inline core::AuxValue Decimal128Cell(const ::duckdb::hugeint_t &h, int32_t scale) {
  return core::AuxValue::Of(core::AuxType::ScaledDecimal128,
                            core::ScaledDecimal128{HugeintToInt128(h), scale});
}

inline ::duckdb::Value Decimal128ToValue(const core::ScaledDecimal128 &d) {
  return ::duckdb::Value::DECIMAL(Int128ToHugeint(d.value), 38, static_cast<uint8_t>(d.scale));
}

} // namespace accelerator_codegen::aux_state::engine::duckdb
