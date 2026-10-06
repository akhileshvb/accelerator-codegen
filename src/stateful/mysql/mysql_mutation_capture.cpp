// GENERATED BACKEND — mutation capture helpers for MySQL triggers.
#include "accelerator_codegen/stateful/mysql/mysql_mutation_capture.hpp"

#include "accelerator_codegen/stateful/core/aux_registry.hpp"
#include "accelerator_codegen/stateful/mysql/mysql_exact_parse.hpp"

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

// One captured column.  `raw == nullptr` is SQL NULL (the UDF layer passes a
// NULL argument as a null pointer).  Text that does not parse exactly as the
// column's type throws: a captured row the accelerator cannot decode must not
// become a silent default or NULL (the caller invalidates the aux state).
// For non-text columns an empty value or the text "NULL" is also read as NULL
// (hand-written captures); a text column keeps both literally.
core::AuxValue ParseCaptureValue(const char *raw, std::size_t len,
                                 const CaptureColumn &col) {
  if (!raw) {
    return core::AuxValue::Null(col.type);
  }
  if (col.type == core::AuxType::String) {
    return core::AuxValue::Of(core::AuxType::String, std::string(raw, len));
  }
  if (len == 0 || (len == 4 && std::memcmp(raw, "NULL", 4) == 0)) {
    return core::AuxValue::Null(col.type);
  }
  auto bad = [&](const char *what) {
    throw std::runtime_error(std::string("mutation capture: cannot parse '") +
                             std::string(raw, len).substr(0, 64) + "' as " + what);
  };
  switch (col.type) {
    case core::AuxType::Boolean: {
      const std::string str(raw, len);
      if (str == "true" || str == "1") return core::AuxValue::Of(core::AuxType::Boolean, true);
      if (str == "false" || str == "0") return core::AuxValue::Of(core::AuxType::Boolean, false);
      bad("boolean");
      break;
    }
    case core::AuxType::Int64: {
      int64_t val = 0;
      if (!exact::ParseInt64(raw, len, val)) bad("int64");
      return core::AuxValue::Of(core::AuxType::Int64, val);
    }
    case core::AuxType::UInt64: {
      uint64_t val = 0;
      if (!exact::ParseUInt64(raw, len, val)) bad("uint64");
      return core::AuxValue::Of(core::AuxType::UInt64, val);
    }
    case core::AuxType::Double: {
      const std::string str(raw, len);
      char *end = nullptr;
      const double val = std::strtod(str.c_str(), &end);
      if (end == str.c_str() || *end != '\0') bad("double");
      return core::AuxValue::Of(core::AuxType::Double, val);
    }
    case core::AuxType::ScaledDecimal: {
      core::ScaledDecimal sd;
      if (!exact::ParseDecimal(raw, len, sd)) bad("decimal");
      return core::AuxValue::Of(core::AuxType::ScaledDecimal, sd);
    }
    case core::AuxType::Date: {
      int32_t days = 0;
      if (!exact::ParseDate(raw, len, days)) bad("date");
      return core::AuxValue::Of(core::AuxType::Date, core::AuxDate{days});
    }
    case core::AuxType::String:
      break;
  }
  return core::AuxValue::Of(core::AuxType::String, std::string(raw, len));
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
  // A row the schema does not describe is never half-read: it would leave
  // the accelerator silently out of date.
  const std::size_t expected_payload =
      batch.kind == core::MutationKind::Update ? 2 * col_count : col_count;
  if (total_payload != expected_payload) {
    throw std::runtime_error("mutation capture: table " + std::to_string(table_id) +
                             " expects " + std::to_string(expected_payload) +
                             " column values, got " + std::to_string(total_payload));
  }

  // Build old rows (for DELETE, UPDATE)
  if (batch.kind == core::MutationKind::Delete ||
      batch.kind == core::MutationKind::Update) {
    if (total_payload >= col_count) {
      core::AuxRow old_row;
      for (std::size_t i = 0; i < col_count; ++i) {
        std::size_t idx = payload_start + i;
        const char *val = args[idx]; // nullptr: SQL NULL
        const std::size_t len = (lengths && lengths[idx] > 0) ? lengths[idx]
                                 : (val ? std::strlen(val) : 0);
        old_row.values.push_back(ParseCaptureValue(val, len, schema[i]));
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
        const char *val = args[idx]; // nullptr: SQL NULL
        const std::size_t len = (lengths && lengths[idx] > 0) ? lengths[idx]
                                 : (val ? std::strlen(val) : 0);
        new_row.values.push_back(ParseCaptureValue(val, len, schema[i]));
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
