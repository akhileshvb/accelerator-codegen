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

namespace {

std::size_t ArgLen(char **args, unsigned long *lengths, std::size_t idx) {
  if (lengths && lengths[idx] > 0) return lengths[idx];
  return args[idx] ? std::strlen(args[idx]) : 0;
}

// Table id (args[0]) and kind (args[1]); false if either is malformed.
bool ParseHeader(char **args, unsigned long *lengths, core::TableId &table_id,
                 int &kind_int) {
  if (!args[0] || !args[1]) return false;
  try {
    table_id = static_cast<core::TableId>(
        std::stoull(std::string(args[0], ArgLen(args, lengths, 0))));
    kind_int = std::stoi(std::string(args[1], ArgLen(args, lengths, 1)));
  } catch (...) {
    return false;
  }
  return kind_int >= 0 && kind_int <= 2;
}

// Builds the batch from args[payload_start ..): the old row's columns (delete,
// update) then the new row's (insert, update), each `schema.size()` values.
// A row the schema does not describe is never half-read: it would leave the
// accelerator silently out of date.
core::MutationBatch BuildBatch(core::TableId table_id, int kind_int,
                               const std::vector<CaptureColumn> &schema,
                               std::size_t arg_count, char **args,
                               unsigned long *lengths, std::size_t payload_start) {
  core::MutationBatch batch;
  batch.table = table_id;
  batch.kind = static_cast<core::MutationKind>(kind_int);
  const std::size_t col_count = schema.size();
  const std::size_t total_payload = arg_count - payload_start;
  const std::size_t expected_payload =
      batch.kind == core::MutationKind::Update ? 2 * col_count : col_count;
  if (total_payload != expected_payload) {
    throw std::runtime_error("mutation capture: table " + std::to_string(table_id) +
                             " expects " + std::to_string(expected_payload) +
                             " column values, got " + std::to_string(total_payload));
  }
  auto read_row = [&](std::size_t start) {
    core::AuxRow row;
    for (std::size_t i = 0; i < col_count; ++i) {
      const std::size_t idx = start + i;
      const char *val = args[idx]; // nullptr: SQL NULL
      row.values.push_back(ParseCaptureValue(val, ArgLen(args, lengths, idx), schema[i]));
    }
    return row;
  };
  if (batch.kind == core::MutationKind::Delete ||
      batch.kind == core::MutationKind::Update) {
    batch.old_rows.push_back(read_row(payload_start));
  }
  if (batch.kind == core::MutationKind::Insert ||
      batch.kind == core::MutationKind::Update) {
    batch.new_rows.push_back(read_row(
        batch.kind == core::MutationKind::Update ? payload_start + col_count : payload_start));
  }
  return batch;
}

// Fills the updated-column list of an update.
core::MutationBatch FinishBatch(core::MutationBatch batch) {
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


// Column type codes of the self-describing capture call (see
// mysql_accel_capture_row): one character per column.
bool ParseTypeCode(char c, core::AuxType &type) {
  switch (c) {
  case 'b': type = core::AuxType::Boolean; return true;
  case 'i': type = core::AuxType::Int64; return true;
  case 'u': type = core::AuxType::UInt64; return true;
  case 'f': type = core::AuxType::Double; return true;
  case 'n': type = core::AuxType::ScaledDecimal; return true;
  case 'd': type = core::AuxType::Date; return true;
  case 's': type = core::AuxType::String; return true;
  default: return false;
  }
}

} // namespace

std::optional<core::MutationBatch>
BuildMutationBatchFromTriggerArgs(std::size_t arg_count, char **args,
                                   unsigned long *lengths) {
  if (!args || arg_count < 2) return std::nullopt;
  core::TableId table_id = 0;
  int kind_int = 0;
  if (!ParseHeader(args, lengths, table_id, kind_int)) return std::nullopt;
  auto schema = LookupCaptureSchema(table_id);
  if (!schema.has_value()) {
    // No schema registered: only the table id and kind can be reported.
    core::MutationBatch batch;
    batch.table = table_id;
    batch.kind = static_cast<core::MutationKind>(kind_int);
    return batch;
  }
  return FinishBatch(BuildBatch(table_id, kind_int, *schema, arg_count, args,
                                lengths, /*payload_start=*/2));
}

std::optional<core::MutationBatch>
BuildMutationBatchFromTypedTriggerArgs(std::size_t arg_count, char **args,
                                       unsigned long *lengths) {
  if (!args || arg_count < 3 || !args[2]) return std::nullopt;
  core::TableId table_id = 0;
  int kind_int = 0;
  if (!ParseHeader(args, lengths, table_id, kind_int)) return std::nullopt;
  std::vector<CaptureColumn> schema;
  const char *types = args[2];
  for (std::size_t i = 0, n = ArgLen(args, lengths, 2); i < n; ++i) {
    CaptureColumn col;
    if (!ParseTypeCode(types[i], col.type)) return std::nullopt;
    schema.push_back(col);
  }
  return FinishBatch(BuildBatch(table_id, kind_int, schema, arg_count, args,
                                lengths, /*payload_start=*/3));
}

} // namespace accelerator_codegen::aux_state::engine::mysql
