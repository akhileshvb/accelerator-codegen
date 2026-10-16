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

} // namespace accelerator_codegen::aux_state::core
