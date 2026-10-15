// GENERATED BACKEND — S1 capture: see mysql_statement_capture.hpp.
#include "backends/mysql/engine/mysql_statement_capture.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

#include "backends/mysql/engine/mysql_exact_parse.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

using core::AuxType;
using runtime::ColumnData;
using runtime::StringColumnData;

constexpr int32_t kUnsetScale = -1;

[[noreturn]] void Malformed(const std::string &what) {
  throw std::runtime_error("mutation capture: " + what);
}

std::size_t CellLength(char **args, const unsigned long *lengths, std::size_t index) {
  if (lengths && lengths[index] > 0) {
    return lengths[index];
  }
  return args[index] ? std::strlen(args[index]) : 0;
}

ColumnData NewColumn(char type_code) {
  ColumnData column;
  switch (type_code) {
  case 'b':
    column.type = AuxType::Boolean;
    column.values = std::vector<uint8_t>();
    break;
  case 'i':
    column.type = AuxType::Int64;
    column.values = std::vector<int64_t>();
    break;
  case 'u':
    column.type = AuxType::UInt64;
    column.values = std::vector<uint64_t>();
    break;
  case 'f':
    column.type = AuxType::Double;
    column.values = std::vector<double>();
    break;
  case 'n':
    column.type = AuxType::ScaledDecimal;
    column.scale = kUnsetScale;
    column.values = std::vector<int64_t>();
    break;
  case 'd':
    column.type = AuxType::Date;
    column.values = std::vector<int32_t>();
    break;
  case 's':
    column.type = AuxType::String;
    column.values = StringColumnData{{0}, {}};
    break;
  default:
    Malformed(std::string("unknown column type '") + type_code + "'");
  }
  return column;
}

template <typename T> std::vector<T> &Values(ColumnData &column) {
  return std::get<std::vector<T>>(column.values);
}

// Parses the cell as the column's type and appends it (a NULL appends a
// placeholder); `row` is the cell's row index. Text that does not parse exactly
// throws: a captured row the accelerator cannot decode must never become a
// silent default.
void AppendCell(ColumnData &column, std::size_t row, const CapturedRow::Cell &cell) {
  const bool is_null = !cell.data;
  if (is_null && column.null_flags.empty()) {
    column.null_flags.assign(row, 0); // the rows so far
  }
  if (is_null || !column.null_flags.empty()) {
    column.null_flags.push_back(is_null);
  }
  auto bad = [&](const char *what) {
    Malformed("cannot parse '" + std::string(cell.data, std::min<std::size_t>(cell.length, 64)) +
              "' as " + what);
  };
  switch (column.type) {
  case AuxType::Boolean: {
    const std::string_view text(cell.data ? cell.data : "", cell.length);
    if (!is_null && text != "true" && text != "false" && text != "1" && text != "0") {
      bad("boolean");
    }
    Values<uint8_t>(column).push_back(text == "true" || text == "1");
    break;
  }
  case AuxType::Int64: {
    int64_t value = 0;
    if (!is_null && !exact::ParseInt64(cell.data, cell.length, value)) {
      bad("int64");
    }
    Values<int64_t>(column).push_back(value);
    break;
  }
  case AuxType::UInt64: {
    uint64_t value = 0;
    if (!is_null && !exact::ParseUInt64(cell.data, cell.length, value)) {
      bad("uint64");
    }
    Values<uint64_t>(column).push_back(value);
    break;
  }
  case AuxType::Double: {
    double value = 0;
    if (!is_null) {
      const std::string text(cell.data, cell.length);
      char *end = nullptr;
      value = std::strtod(text.c_str(), &end);
      if (end == text.c_str() || *end != '\0') {
        bad("double");
      }
    }
    Values<double>(column).push_back(value);
    break;
  }
  case AuxType::ScaledDecimal: {
    int64_t unscaled = 0;
    if (!is_null) {
      core::ScaledDecimal decimal;
      if (!exact::ParseDecimal(cell.data, cell.length, decimal)) {
        bad("decimal");
      }
      if (column.scale == kUnsetScale) {
        column.scale = decimal.scale;
      }
      if (!exact::Rescale(decimal, column.scale, unscaled)) {
        bad("decimal of the column's scale");
      }
    }
    Values<int64_t>(column).push_back(unscaled);
    break;
  }
  case AuxType::Date: {
    int32_t days = 0;
    if (!is_null && !exact::ParseDate(cell.data, cell.length, days)) {
      bad("date");
    }
    Values<int32_t>(column).push_back(days);
    break;
  }
  case AuxType::String: {
    auto &strings = std::get<StringColumnData>(column.values);
    strings.bytes.append(cell.data ? cell.data : "", cell.length);
    strings.offsets.push_back(strings.bytes.size());
    break;
  }
  case AuxType::ScaledDecimal128:
    Malformed("128-bit decimals are not captured");
  }
}

bool SameCell(const CapturedRow::Cell &a, const CapturedRow::Cell &b) {
  if (!a.data || !b.data) {
    return a.data == b.data;
  }
  return a.length == b.length && std::memcmp(a.data, b.data, a.length) == 0;
}

// The attnums to keep out of `column_count` columns.
std::vector<int32_t> KeptAttnums(std::size_t column_count, const std::vector<int32_t> &watched) {
  std::vector<int32_t> kept = watched;
  if (kept.empty()) {
    kept.resize(column_count);
    for (std::size_t i = 0; i < column_count; ++i) {
      kept[i] = static_cast<int32_t>(i);
    }
  } else if (static_cast<std::size_t>(kept.back()) >= column_count) {
    Malformed("a watched column is beyond the captured row");
  }
  return kept;
}

void StartSet(runtime::ColumnSet &set, const std::vector<int32_t> &kept, std::string_view types) {
  set.attnums = kept;
  for (const int32_t attnum : kept) {
    set.columns.push_back(NewColumn(types[static_cast<std::size_t>(attnum)]));
  }
}

void AppendRow(runtime::ColumnSet &set, const CapturedRow &row, std::size_t first_cell,
               std::size_t row_index) {
  for (std::size_t i = 0; i < set.attnums.size(); ++i) {
    AppendCell(set.columns[i], row_index,
               row.cells[first_cell + static_cast<std::size_t>(set.attnums[i])]);
  }
}

void FinishSet(runtime::ColumnSet &set) {
  for (auto &column : set.columns) {
    if (column.type == AuxType::ScaledDecimal && column.scale == kUnsetScale) {
      column.scale = 0; // only NULLs
    }
  }
}

} // namespace

CapturedRow DecodeCapturedRow(std::size_t arg_count, char **args, const unsigned long *lengths) {
  if (arg_count < 3 || !args[0] || !args[1] || !args[2]) {
    Malformed("a call needs a table, a kind and column types");
  }
  CapturedRow row;
  int kind = 0;
  try {
    row.table = static_cast<core::TableId>(std::stoull(std::string(args[0], CellLength(args, lengths, 0))));
    kind = std::stoi(std::string(args[1], CellLength(args, lengths, 1)));
  } catch (const std::exception &) {
    Malformed("malformed table or kind");
  }
  if (kind < 0 || kind > 2) {
    Malformed("unknown kind " + std::to_string(kind));
  }
  row.kind = static_cast<core::MutationKind>(kind);
  row.types = std::string_view(args[2], CellLength(args, lengths, 2));
  const std::size_t rows_per_call = row.kind == core::MutationKind::Update ? 2 : 1;
  if (arg_count - 3 != rows_per_call * row.types.size()) {
    Malformed("table " + std::to_string(row.table) + " expects " +
              std::to_string(rows_per_call * row.types.size()) + " column values, got " +
              std::to_string(arg_count - 3));
  }
  for (std::size_t i = 3; i < arg_count; ++i) {
    row.cells.push_back({args[i], args[i] ? CellLength(args, lengths, i) : 0});
  }
  return row;
}

void StatementCapture::Add(const CapturedRow &row, const std::vector<int32_t> &watched) {
  const auto kept = KeptAttnums(row.types.size(), watched);
  const auto &first = row.kind == core::MutationKind::Insert ? open.new_rows : open.old_rows;
  if (open.row_count > 0 &&
      (open.table != row.table || open.kind != row.kind || first.attnums != kept)) {
    Close();
  }
  if (open.row_count == 0) {
    open = runtime::ColumnarBatch();
    open.table = row.table;
    open.kind = row.kind;
    if (row.kind != core::MutationKind::Insert) {
      StartSet(open.old_rows, kept, row.types);
    }
    if (row.kind != core::MutationKind::Delete) {
      StartSet(open.new_rows, kept, row.types);
    }
  }
  const std::size_t columns = row.types.size();
  const std::size_t new_cells = row.kind == core::MutationKind::Update ? columns : 0;
  if (row.kind != core::MutationKind::Insert) {
    AppendRow(open.old_rows, row, 0, open.row_count);
  }
  if (row.kind != core::MutationKind::Delete) {
    AppendRow(open.new_rows, row, new_cells, open.row_count);
  }
  if (row.kind == core::MutationKind::Update) {
    for (std::size_t i = 0; i < columns; ++i) {
      if (!SameCell(row.cells[i], row.cells[columns + i])) {
        changed.insert(static_cast<int32_t>(i));
      }
    }
  }
  open.row_count++;
}

void StatementCapture::Close() {
  if (open.row_count == 0) {
    return;
  }
  FinishSet(open.old_rows);
  FinishSet(open.new_rows);
  open.updated_attnums.assign(changed.begin(), changed.end());
  changed.clear();
  closed.push_back(std::make_shared<const runtime::ColumnarBatch>(std::move(open)));
  open = runtime::ColumnarBatch();
}

std::vector<runtime::ColumnarBatchRef> StatementCapture::Take() {
  Close();
  return std::exchange(closed, {});
}

} // namespace accelerator_codegen::aux_state::engine::mysql
