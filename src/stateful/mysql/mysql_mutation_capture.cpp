// GENERATED BACKEND — mutation capture helpers for MySQL triggers.
#include "accelerator_codegen/stateful/mysql/mysql_mutation_capture.hpp"

#include "accelerator_codegen/stateful/core/aux_registry.hpp"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace accelerator_codegen::aux_state::engine::mysql {

namespace {

std::mutex g_schema_mutex;
std::unordered_map<core::TableId, std::vector<CaptureColumn>> g_schema;

bool ParseInt(const char *s, std::size_t len, int64_t &out) {
  if (!s || len == 0) return false;
  std::string str(s, len);
  char *end = nullptr;
  out = static_cast<int64_t>(std::strtoll(str.c_str(), &end, 10));
  return end != str.c_str() && *end == '\0';
}

bool ParseDouble(const char *s, std::size_t len, double &out) {
  if (!s || len == 0) return false;
  std::string str(s, len);
  char *end = nullptr;
  out = std::strtod(str.c_str(), &end);
  return end != str.c_str();
}

bool ParseScaledDecimal(const char *s, std::size_t len, core::ScaledDecimal &out) {
  if (!s || len == 0) return false;
  std::string str(s, len);
  std::size_t dot = str.find('.');
  int32_t scale = 0;
  std::string digits = str;
  if (dot != std::string::npos) {
    scale = static_cast<int32_t>(str.size() - dot - 1);
    digits.erase(dot, 1);
  }
  char *end = nullptr;
  out.value = static_cast<int64_t>(std::strtoll(digits.c_str(), &end, 10));
  if (end == digits.c_str()) {
    return false;
  }
  out.scale = scale;
  return true;
}

int DaysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int>(doe) - 719468;
}

bool ParseDateDays(const char *s, std::size_t len, core::AuxDate &out) {
  if (!s || len < 10) return false;
  std::string str(s, len);
  int y = std::atoi(str.substr(0, 4).c_str());
  unsigned m = static_cast<unsigned>(std::atoi(str.substr(5, 2).c_str()));
  unsigned d = static_cast<unsigned>(std::atoi(str.substr(8, 2).c_str()));
  if (y == 0 || m == 0 || d == 0) return false;
  out.days_since_epoch = DaysFromCivil(y, m, d);
  return true;
}

} // namespace

void RegisterCaptureTableSchema(core::TableId table_id,
                                std::vector<CaptureColumn> cols) {
  std::lock_guard<std::mutex> lock(g_schema_mutex);
  g_schema[table_id] = std::move(cols);
}

std::optional<std::vector<CaptureColumn>>
LookupCaptureSchema(core::TableId table_id) {
  std::lock_guard<std::mutex> lock(g_schema_mutex);
  auto it = g_schema.find(table_id);
  if (it == g_schema.end()) return std::nullopt;
  return it->second;
}

core::AuxValue ParseCaptureValue(const char *raw, std::size_t len,
                                 const CaptureColumn &col) {
  if (!raw || len == 0) {
    return core::AuxValue::Null(col.type);
  }
  std::string str(raw, len);
  if (str == "NULL") {
    return core::AuxValue::Null(col.type);
  }

  switch (col.type) {
    case core::AuxType::Boolean: {
      if (str == "true" || str == "1") {
        return core::AuxValue::Of(core::AuxType::Boolean, true);
      }
      return core::AuxValue::Of(core::AuxType::Boolean, false);
    }
    case core::AuxType::Int64: {
      int64_t val = 0;
      if (ParseInt(raw, len, val)) {
        return core::AuxValue::Of(core::AuxType::Int64, val);
      }
      return core::AuxValue::Null(col.type);
    }
    case core::AuxType::UInt64: {
      int64_t val = 0;
      if (ParseInt(raw, len, val)) {
        return core::AuxValue::Of(core::AuxType::UInt64,
                                  static_cast<uint64_t>(val));
      }
      return core::AuxValue::Null(col.type);
    }
    case core::AuxType::Double: {
      double val = 0.0;
      if (ParseDouble(raw, len, val)) {
        return core::AuxValue::Of(core::AuxType::Double, val);
      }
      return core::AuxValue::Null(col.type);
    }
    case core::AuxType::String:
      return core::AuxValue::Of(core::AuxType::String, str);
    case core::AuxType::ScaledDecimal: {
      core::ScaledDecimal sd;
      if (ParseScaledDecimal(raw, len, sd)) {
        return core::AuxValue::Of(core::AuxType::ScaledDecimal, sd);
      }
      double dval = 0.0;
      if (ParseDouble(raw, len, dval)) {
        return core::AuxValue::Of(core::AuxType::Double, dval);
      }
      return core::AuxValue::Null(col.type);
    }
    case core::AuxType::Date: {
      core::AuxDate date;
      if (ParseDateDays(raw, len, date)) {
        return core::AuxValue::Of(core::AuxType::Date, date);
      }
      return core::AuxValue::Null(col.type);
    }
  }
  return core::AuxValue::Of(core::AuxType::String, str);
}

std::optional<core::MutationBatch>
BuildMutationBatchFromTriggerArgs(std::size_t arg_count, char **args,
                                   unsigned long *lengths) {
  if (!args || arg_count < 2) return std::nullopt;

  // Parse table id (args[0])
  std::size_t len0 = (lengths && lengths[0] > 0) ? lengths[0]
                               : (args[0] ? std::strlen(args[0]) : 0);
  std::string tid_str(args[0], len0);
  core::TableId table_id = 0;
  try {
    table_id = static_cast<core::TableId>(std::stoull(tid_str));
  } catch (...) {
    return std::nullopt;
  }

  // Parse mutation kind (args[1])
  std::size_t len1 = (lengths && lengths[1] > 0) ? lengths[1]
                               : (args[1] ? std::strlen(args[1]) : 0);
  std::string kind_str(args[1], len1);
  int kind_int = 0;
  try {
    kind_int = std::stoi(kind_str);
  } catch (...) {
    return std::nullopt;
  }
  if (kind_int < 0 || kind_int > 2) return std::nullopt;

  core::MutationBatch batch;
  batch.table = table_id;
  batch.kind = static_cast<core::MutationKind>(kind_int);

  // Get registered schema for this table
  auto schema_opt = LookupCaptureSchema(table_id);
  if (!schema_opt.has_value()) {
    // No schema registered — still return a basic batch so the
    // harness can at least see table id and kind.
    // Do not return nullopt here; the harness needs to verify typed fields.
    // Without schema, we cannot parse columns but we store minimal info.
    return batch;
  }
  const auto &schema = *schema_opt;
  std::size_t col_count = schema.size();

  // After table_id + kind, remaining args are:
  // old-row columns (count: col_count), then optionally new-row columns
  std::size_t payload_start = 2;
  std::size_t total_payload = arg_count - payload_start;

  // Build old rows (for DELETE, UPDATE)
  if (batch.kind == core::MutationKind::Delete ||
      batch.kind == core::MutationKind::Update) {
    if (total_payload >= col_count) {
      core::AuxRow old_row;
      for (std::size_t i = 0; i < col_count; ++i) {
        std::size_t idx = payload_start + i;
        const char *val = (idx < arg_count) ? args[idx] : nullptr;
        std::size_t len = (lengths && idx < arg_count && lengths[idx] > 0)
                           ? lengths[idx]
                           : (val ? std::strlen(val) : 0);
        if (val && len > 0) {
          std::string sv(val, len);
          if (sv == "NULL") {
            old_row.values.push_back(core::AuxValue::Null(schema[i].type));
          } else {
            old_row.values.push_back(ParseCaptureValue(val, len, schema[i]));
          }
        } else {
          old_row.values.push_back(core::AuxValue::Null(schema[i].type));
        }
      }
      batch.old_rows.push_back(std::move(old_row));
    }
  }

  // Build new rows (for INSERT, UPDATE)
  if (batch.kind == core::MutationKind::Insert ||
      batch.kind == core::MutationKind::Update) {
    std::size_t new_start = payload_start;
    if (batch.kind == core::MutationKind::Update && total_payload >= col_count) {
      new_start = payload_start + col_count;
    }
    if (new_start + col_count <= arg_count) {
      core::AuxRow new_row;
      for (std::size_t i = 0; i < col_count; ++i) {
        std::size_t idx = new_start + i;
        const char *val = (idx < arg_count) ? args[idx] : nullptr;
        std::size_t len = (lengths && idx < arg_count && lengths[idx] > 0)
                           ? lengths[idx]
                           : (val ? std::strlen(val) : 0);
        if (val && len > 0) {
          std::string sv(val, len);
          if (sv == "NULL") {
            new_row.values.push_back(core::AuxValue::Null(schema[i].type));
          } else {
            new_row.values.push_back(ParseCaptureValue(val, len, schema[i]));
          }
        } else {
          new_row.values.push_back(core::AuxValue::Null(schema[i].type));
        }
      }
      batch.new_rows.push_back(std::move(new_row));
    }
  }


  // Detect updated columns for UPDATE: compare old vs new row values
  // Detect updated columns for UPDATE: compare old vs new row values
  if (batch.kind == core::MutationKind::Update &&
      !batch.old_rows.empty() && !batch.new_rows.empty()) {
    const auto &old_row = batch.old_rows[0];
    const auto &new_row = batch.new_rows[0];
    std::size_t n = std::min(old_row.values.size(), new_row.values.size());
    for (std::size_t j = 0; j < n; ++j) {
      const auto &ov = old_row.values[j];
      const auto &nv = new_row.values[j];
      bool changed = (ov.is_null != nv.is_null);
      if (!changed && !ov.is_null) {
        if (ov.type != nv.type) {
          changed = true;
        } else if (ov.payload.index() != nv.payload.index()) {
          changed = true;
        } else {
          // Same variant alternative — compare via type-specific get
          try {
            if (std::holds_alternative<int64_t>(ov.payload)) {
              changed = (std::get<int64_t>(ov.payload) != std::get<int64_t>(nv.payload));
            } else if (std::holds_alternative<uint64_t>(ov.payload)) {
              changed = (std::get<uint64_t>(ov.payload) != std::get<uint64_t>(nv.payload));
            } else if (std::holds_alternative<double>(ov.payload)) {
              changed = (std::get<double>(ov.payload) != std::get<double>(nv.payload));
            } else if (std::holds_alternative<bool>(ov.payload)) {
              changed = (std::get<bool>(ov.payload) != std::get<bool>(nv.payload));
            } else if (std::holds_alternative<std::string>(ov.payload)) {
              changed = (std::get<std::string>(ov.payload) != std::get<std::string>(nv.payload));
            } else if (std::holds_alternative<core::AuxDate>(ov.payload)) {
              changed = !(std::get<core::AuxDate>(ov.payload) ==
                          std::get<core::AuxDate>(nv.payload));
            } else if (std::holds_alternative<core::ScaledDecimal>(ov.payload)) {
              changed = !(std::get<core::ScaledDecimal>(ov.payload) ==
                          std::get<core::ScaledDecimal>(nv.payload));
            } else {
              changed = true;
            }
          } catch (...) {
            changed = true;
          }
        }
      }
      if (changed) {
        batch.updated_columns.push_back(static_cast<core::ColumnId>(j));
      }
    }
  }

  return batch;
}

} // namespace accelerator_codegen::aux_state::engine::mysql
