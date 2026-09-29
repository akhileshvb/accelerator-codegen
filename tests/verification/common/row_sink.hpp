#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_types.hpp"

namespace accel_verify {

using accelerator_codegen::aux_state::core::AuxRow;
using accelerator_codegen::aux_state::core::AuxRowSink;
using accelerator_codegen::aux_state::core::AuxType;
using accelerator_codegen::aux_state::core::AuxValue;
using accelerator_codegen::aux_state::core::OutputSchema;
using accelerator_codegen::aux_state::core::ScaledDecimal;

class CollectingSink final : public AuxRowSink {
public:
  void Begin(const OutputSchema &schema) override {
    columns = schema.columns.size();
    began = true;
  }
  void WriteRow(const AuxRow &row) override { rows.push_back(row); }
  void End() override { ended = true; }

  std::size_t columns = 0;
  std::vector<AuxRow> rows;
  bool began = false;
  bool ended = false;
};

// Lexicographic key for integer / decimal / string cells (skips doubles).
inline std::string CellKey(const AuxValue &value) {
  if (value.is_null)
    return "N";
  if (std::holds_alternative<int64_t>(value.payload))
    return "i:" + std::to_string(std::get<int64_t>(value.payload));
  if (std::holds_alternative<uint64_t>(value.payload))
    return "u:" + std::to_string(std::get<uint64_t>(value.payload));
  if (std::holds_alternative<ScaledDecimal>(value.payload)) {
    const auto &d = std::get<ScaledDecimal>(value.payload);
    return "d:" + std::to_string(d.value) + "@" + std::to_string(d.scale);
  }
  if (std::holds_alternative<std::string>(value.payload))
    return "s:" + std::get<std::string>(value.payload);
  if (std::holds_alternative<bool>(value.payload))
    return std::get<bool>(value.payload) ? "b:1" : "b:0";
  // Doubles / dates: ignore for equality of aggregate avgs.
  return "x";
}

inline std::string RowKey(const AuxRow &row, bool skip_doubles) {
  std::string out;
  for (const auto &cell : row.values) {
    if (skip_doubles && !cell.is_null &&
        std::holds_alternative<double>(cell.payload))
      continue;
    out += CellKey(cell);
    out.push_back('|');
  }
  return out;
}

inline bool IntegerColumnsEqual(const CollectingSink &a,
                                const CollectingSink &b) {
  if (a.rows.size() != b.rows.size())
    return false;
  std::vector<std::string> ka;
  std::vector<std::string> kb;
  ka.reserve(a.rows.size());
  kb.reserve(b.rows.size());
  for (const auto &row : a.rows)
    ka.push_back(RowKey(row, /*skip_doubles=*/true));
  for (const auto &row : b.rows)
    kb.push_back(RowKey(row, /*skip_doubles=*/true));
  std::sort(ka.begin(), ka.end());
  std::sort(kb.begin(), kb.end());
  return ka == kb;
}

} // namespace accel_verify
