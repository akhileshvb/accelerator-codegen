// SCAFFOLDING
// Typed reads of one AuxRow cell: false when the cell is missing, NULL or of another type.
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <variant>

#include "substrate/aux/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::core {

namespace aux_cells_detail {
inline const AuxPayload *Payload(const AuxRow &row, std::size_t index) {
  if (index >= row.values.size() || row.values[index].is_null) {
    return nullptr;
  }
  return &row.values[index].payload;
}
} // namespace aux_cells_detail

inline bool StringCell(const AuxRow &row, std::size_t index, std::string &out) {
  const auto *payload = aux_cells_detail::Payload(row, index);
  const auto *value = payload ? std::get_if<std::string>(payload) : nullptr;
  if (!value) {
    return false;
  }
  out = *value;
  return true;
}

inline bool CharCell(const AuxRow &row, std::size_t index, char &out) {
  std::string value;
  if (!StringCell(row, index, value) || value.empty()) {
    return false;
  }
  out = value[0];
  return true;
}

// Integer, unsigned (when it fits) or date cell as int64.
inline bool IntCell(const AuxRow &row, std::size_t index, int64_t &out) {
  const auto *payload = aux_cells_detail::Payload(row, index);
  if (!payload) {
    return false;
  }
  if (const auto *value = std::get_if<int64_t>(payload)) {
    out = *value;
    return true;
  }
  if (const auto *value = std::get_if<uint64_t>(payload)) {
    if (*value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      return false;
    }
    out = static_cast<int64_t>(*value);
    return true;
  }
  if (const auto *value = std::get_if<AuxDate>(payload)) {
    out = value->days_since_epoch;
    return true;
  }
  return false;
}

// An integer cell that fits int32.
inline bool Int32Cell(const AuxRow &row, std::size_t index, int32_t &out) {
  int64_t value = 0;
  if (!IntCell(row, index, value) || value < std::numeric_limits<int32_t>::min() ||
      value > std::numeric_limits<int32_t>::max()) {
    return false;
  }
  out = static_cast<int32_t>(value);
  return true;
}

// Days since 1970-01-01.
inline bool DateCell(const AuxRow &row, std::size_t index, int32_t &days) {
  int64_t value = 0;
  if (!IntCell(row, index, value) || value < std::numeric_limits<int32_t>::min() ||
      value > std::numeric_limits<int32_t>::max()) {
    return false;
  }
  days = static_cast<int32_t>(value);
  return true;
}

// Double, integer or decimal cell as double.
inline bool DoubleCell(const AuxRow &row, std::size_t index, double &out) {
  const auto *payload = aux_cells_detail::Payload(row, index);
  if (!payload) {
    return false;
  }
  if (const auto *value = std::get_if<double>(payload)) {
    out = *value;
    return true;
  }
  if (const auto *value = std::get_if<int64_t>(payload)) {
    out = static_cast<double>(*value);
    return true;
  }
  if (const auto *value = std::get_if<ScaledDecimal>(payload)) {
    out = static_cast<double>(value->value);
    for (int32_t i = 0; i < value->scale; ++i) {
      out /= 10.0;
    }
    return true;
  }
  return false;
}

// A decimal cell's unscaled value and scale.
inline bool DecimalCell(const AuxRow &row, std::size_t index, int64_t &raw, int32_t &scale) {
  const auto *payload = aux_cells_detail::Payload(row, index);
  const auto *value = payload ? std::get_if<ScaledDecimal>(payload) : nullptr;
  if (!value) {
    return false;
  }
  raw = value->value;
  scale = value->scale;
  return true;
}

// A decimal cell of exactly `scale`: its unscaled value.
inline bool DecimalCellAtScale(const AuxRow &row, std::size_t index, int32_t scale, int64_t &raw) {
  int64_t value = 0;
  int32_t cell_scale = 0;
  if (!DecimalCell(row, index, value, cell_scale) || cell_scale != scale) {
    return false;
  }
  raw = value;
  return true;
}

// A decimal cell as an unscaled value at exactly `scale`: false when it is not
// a decimal, or rescaling would overflow int64 or drop a non-zero digit.
inline bool ExactDecimalCell(const AuxRow &row, std::size_t index, int32_t scale, int64_t &raw) {
  int64_t value = 0;
  int32_t cell_scale = 0;
  if (!DecimalCell(row, index, value, cell_scale)) {
    return false;
  }
  for (; cell_scale < scale; ++cell_scale) {
    if (__builtin_mul_overflow(value, int64_t{10}, &value)) {
      return false;
    }
  }
  for (; cell_scale > scale; --cell_scale) {
    if (value % 10 != 0) {
      return false;
    }
    value /= 10;
  }
  raw = value;
  return true;
}

// A 64-bit or 128-bit decimal cell's unscaled value and scale.
inline bool WideDecimalCell(const AuxRow &row, std::size_t index, __int128 &raw, int32_t &scale) {
  const auto *payload = aux_cells_detail::Payload(row, index);
  if (!payload) {
    return false;
  }
  if (const auto *value = std::get_if<ScaledDecimal128>(payload)) {
    raw = value->value;
    scale = value->scale;
    return true;
  }
  int64_t narrow = 0;
  if (!DecimalCell(row, index, narrow, scale)) {
    return false;
  }
  raw = narrow;
  return true;
}

// Rescales an unscaled value, truncating toward zero when the scale shrinks.
inline int64_t ScaleToRaw(int64_t raw, int32_t scale, int32_t target_scale) {
  for (; scale < target_scale; ++scale) {
    raw *= 10;
  }
  for (; scale > target_scale; --scale) {
    raw /= 10;
  }
  return raw;
}

// Tri-state cell reads. Null: SQL NULL, the row is ignored (it cannot join or
// pass the filter). Bad: missing cell or a payload the reader does not accept,
// the state cannot follow the write and is rebuilt.
enum class CellState { Ok, Null, Bad };

namespace aux_cells_detail {
// The cell's payload as T, or Null / Bad.
template <class T> CellState Read(const AuxRow &row, std::size_t index, const T *&value) {
  if (index >= row.values.size()) {
    return CellState::Bad;
  }
  const AuxValue &cell = row.values[index];
  if (cell.is_null) {
    return CellState::Null;
  }
  value = std::get_if<T>(&cell.payload);
  return value ? CellState::Ok : CellState::Bad;
}
} // namespace aux_cells_detail

inline CellState NullableStringCell(const AuxRow &row, std::size_t index, std::string &out) {
  const std::string *value = nullptr;
  const CellState state = aux_cells_detail::Read(row, index, value);
  if (state == CellState::Ok) {
    out = *value;
  }
  return state;
}

// Days of a date cell (a date payload only).
inline CellState NullableDateCell(const AuxRow &row, std::size_t index, int32_t &days) {
  const AuxDate *value = nullptr;
  const CellState state = aux_cells_detail::Read(row, index, value);
  if (state == CellState::Ok) {
    days = value->days_since_epoch;
  }
  return state;
}

// A signed or unsigned (when it fits) integer cell.
inline CellState NullableIntCell(const AuxRow &row, std::size_t index, int64_t &out) {
  if (index < row.values.size() && !row.values[index].is_null) {
    if (const auto *value = std::get_if<uint64_t>(&row.values[index].payload)) {
      if (*value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return CellState::Bad;
      }
      out = static_cast<int64_t>(*value);
      return CellState::Ok;
    }
  }
  const int64_t *value = nullptr;
  const CellState state = aux_cells_detail::Read(row, index, value);
  if (state == CellState::Ok) {
    out = *value;
  }
  return state;
}

// A signed integer or date cell (as days) as int64.
inline CellState NullableIntOrDateCell(const AuxRow &row, std::size_t index, int64_t &out) {
  if (index < row.values.size() && !row.values[index].is_null) {
    if (const auto *value = std::get_if<AuxDate>(&row.values[index].payload)) {
      out = value->days_since_epoch;
      return CellState::Ok;
    }
  }
  const int64_t *value = nullptr;
  const CellState state = aux_cells_detail::Read(row, index, value);
  if (state == CellState::Ok) {
    out = *value;
  }
  return state;
}

// NullableIntOrDateCell that fits int32 (an int32 key or a date in days).
inline CellState NullableInt32OrDateCell(const AuxRow &row, std::size_t index, int32_t &out) {
  int64_t value = 0;
  const CellState state = NullableIntOrDateCell(row, index, value);
  if (state != CellState::Ok) {
    return state;
  }
  if (value < std::numeric_limits<int32_t>::min() || value > std::numeric_limits<int32_t>::max()) {
    return CellState::Bad;
  }
  out = static_cast<int32_t>(value);
  return CellState::Ok;
}

// A decimal cell's unscaled value and scale.
inline CellState NullableDecimalCell(const AuxRow &row, std::size_t index, int64_t &raw,
                                     int32_t &scale) {
  const ScaledDecimal *value = nullptr;
  const CellState state = aux_cells_detail::Read(row, index, value);
  if (state == CellState::Ok) {
    raw = value->value;
    scale = value->scale;
  }
  return state;
}

} // namespace accelerator_codegen::aux_state::core
