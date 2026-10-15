// GENERATED BACKEND
#include "backends/duckdb/engine/capture.hpp"

#include "backends/duckdb/api/duckdb_host.hpp"
#include "backends/duckdb/engine/scan.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/main/database.hpp"

namespace accelerator_codegen::aux_state::engine::duckdb {

// ---- watch provider ----

void AuxWatchedColumns::Add(const std::string &table, const std::vector<int32_t> &columns) {
  std::lock_guard<std::mutex> lock(mutex);
  for (auto c : columns) {
    watched[table][c]++;
  }
}

void AuxWatchedColumns::Remove(const std::string &table, const std::vector<int32_t> &columns) {
  std::lock_guard<std::mutex> lock(mutex);
  auto it = watched.find(table);
  if (it == watched.end()) {
    return;
  }
  for (auto c : columns) {
    auto col = it->second.find(c);
    if (col != it->second.end() && --col->second == 0) {
      it->second.erase(col);
    }
  }
  if (it->second.empty()) {
    watched.erase(it);
  }
}

bool AuxWatchedColumns::Columns(const std::string &table,
                                ::duckdb::vector<::duckdb::PhysicalIndex> &columns) {
  std::lock_guard<std::mutex> lock(mutex);
  auto it = watched.find(table);
  if (it == watched.end()) {
    return false;
  }
  columns.clear();
  for (const auto &c : it->second) {
    columns.emplace_back(static_cast<::duckdb::idx_t>(c.first));
  }
  return true;
}

bool AuxWatchedColumns::TryGetWatchedColumns(::duckdb::TableCatalogEntry &table,
                                             ::duckdb::vector<::duckdb::PhysicalIndex> &columns) {
  return Columns(table.name, columns) && !columns.empty();
}

bool AuxWatchedColumns::TryGetUpdateOldColumns(::duckdb::TableCatalogEntry &table,
                                               const ::duckdb::vector<::duckdb::PhysicalIndex> &,
                                               ::duckdb::vector<::duckdb::PhysicalIndex> &old_columns) {
  return Columns(table.name, old_columns);
}

AuxWatchedColumns &WatchedColumns(::duckdb::DatabaseInstance &db) {
  return *db.GetObjectCache().GetOrCreate<AuxWatchedColumns>(AuxWatchedColumns::kKey);
}

// A captured DuckDB value as the modules read it (the same mapping as the
// PostgreSQL capture: integers Int64, dates AuxDate, decimals ScaledDecimal,
// doubles Double, everything else its text).
core::AuxValue CapturedValue(const ::duckdb::Value &value) {
  using ::duckdb::LogicalTypeId;
  const auto id = value.type().id();
  switch (id) {
  case LogicalTypeId::BOOLEAN:
    return value.IsNull() ? core::AuxValue::Null(core::AuxType::Boolean)
                          : core::AuxValue::Of(core::AuxType::Boolean, value.GetValue<bool>());
  case LogicalTypeId::TINYINT:
  case LogicalTypeId::SMALLINT:
  case LogicalTypeId::INTEGER:
  case LogicalTypeId::BIGINT:
    return value.IsNull() ? core::AuxValue::Null(core::AuxType::Int64)
                          : core::AuxValue::Of(core::AuxType::Int64, value.GetValue<int64_t>());
  case LogicalTypeId::DATE:
    return value.IsNull() ? core::AuxValue::Null(core::AuxType::Date)
                          : core::AuxValue::Of(core::AuxType::Date, core::AuxDate {scan::DateDays(value)});
  case LogicalTypeId::FLOAT:
  case LogicalTypeId::DOUBLE:
    return value.IsNull() ? core::AuxValue::Null(core::AuxType::Double)
                          : core::AuxValue::Of(core::AuxType::Double, value.GetValue<double>());
  case LogicalTypeId::DECIMAL: {
    if (value.IsNull()) {
      return core::AuxValue::Null(core::AuxType::ScaledDecimal);
    }
    if (value.type().InternalType() == ::duckdb::PhysicalType::INT128) {
      const auto huge = value.GetValueUnsafe<::duckdb::hugeint_t>();
      const bool fits = (huge.upper == 0 && static_cast<int64_t>(huge.lower) >= 0) ||
                        (huge.upper == -1 && static_cast<int64_t>(huge.lower) < 0);
      if (!fits) // beyond 64 bits: exact as decimal text (DuckDB parses it back exactly), never via double
        return core::AuxValue::Of(core::AuxType::String, value.ToString());
    }
    core::ScaledDecimal decimal;
    decimal.value = scan::DecimalRaw(value);
    decimal.scale = static_cast<int32_t>(::duckdb::DecimalType::GetScale(value.type()));
    return core::AuxValue::Of(core::AuxType::ScaledDecimal, decimal);
  }
  case LogicalTypeId::VARCHAR:
    return value.IsNull() ? core::AuxValue::Null(core::AuxType::String)
                          : core::AuxValue::Of(core::AuxType::String, ::duckdb::StringValue::Get(value));
  default:
    return value.IsNull() ? core::AuxValue::Null(core::AuxType::String)
                          : core::AuxValue::Of(core::AuxType::String, value.ToString());
  }
}

// ---- event -> columnar batch ----

namespace {

using ::duckdb::LogicalTypeId;
using ::duckdb::PhysicalType;
using runtime::ColumnData;

// Fills `out` with `rows` values of `vec` read as T and converted by `convert`.
template <typename T, typename Out, typename Convert>
void Fill(::duckdb::Vector &vec, std::size_t rows, ColumnData &out, core::AuxType type,
          Convert convert) {
  ::duckdb::UnifiedVectorFormat fmt;
  vec.ToUnifiedFormat(rows, fmt);
  const auto *data = ::duckdb::UnifiedVectorFormat::GetData<T>(fmt);
  std::vector<Out> values(rows);
  for (std::size_t r = 0; r < rows; ++r) {
    const auto idx = fmt.sel->get_index(r);
    if (!fmt.validity.RowIsValid(idx)) {
      if (out.null_flags.empty()) {
        out.null_flags.assign(rows, 0);
      }
      out.null_flags[r] = 1;
      continue;
    }
    values[r] = convert(data[idx]);
  }
  out.type = type;
  out.values = std::move(values);
}

// Any type without a typed path: its text, via the vector's values.
void FillText(::duckdb::Vector &vec, std::size_t rows, ColumnData &out) {
  runtime::StringColumnData text;
  text.offsets.reserve(rows + 1);
  text.offsets.push_back(0);
  for (std::size_t r = 0; r < rows; ++r) {
    const auto value = vec.GetValue(r);
    if (value.IsNull()) {
      if (out.null_flags.empty()) {
        out.null_flags.assign(rows, 0);
      }
      out.null_flags[r] = 1;
    } else {
      text.bytes += value.ToString();
    }
    text.offsets.push_back(text.bytes.size());
  }
  out.type = core::AuxType::String;
  out.values = std::move(text);
}

bool FitsInt64(const ::duckdb::hugeint_t &huge) {
  return (huge.upper == 0 && static_cast<int64_t>(huge.lower) >= 0) ||
         (huge.upper == -1 && static_cast<int64_t>(huge.lower) < 0);
}

ColumnData Column(::duckdb::Vector &vec, std::size_t rows) {
  ColumnData out;
  const auto &type = vec.GetType();
  const auto as_int64 = [](auto v) { return static_cast<int64_t>(v); };
  switch (type.id()) {
  case LogicalTypeId::BOOLEAN:
    Fill<bool, uint8_t>(vec, rows, out, core::AuxType::Boolean, [](bool v) { return uint8_t(v); });
    return out;
  case LogicalTypeId::TINYINT:
    Fill<int8_t, int64_t>(vec, rows, out, core::AuxType::Int64, as_int64);
    return out;
  case LogicalTypeId::SMALLINT:
    Fill<int16_t, int64_t>(vec, rows, out, core::AuxType::Int64, as_int64);
    return out;
  case LogicalTypeId::INTEGER:
    Fill<int32_t, int64_t>(vec, rows, out, core::AuxType::Int64, as_int64);
    return out;
  case LogicalTypeId::BIGINT:
    Fill<int64_t, int64_t>(vec, rows, out, core::AuxType::Int64, as_int64);
    return out;
  case LogicalTypeId::DATE:
    Fill<::duckdb::date_t, int32_t>(vec, rows, out, core::AuxType::Date,
                                    [](::duckdb::date_t d) { return ::duckdb::Date::EpochDays(d); });
    return out;
  case LogicalTypeId::FLOAT:
    Fill<float, double>(vec, rows, out, core::AuxType::Double, [](float v) { return double(v); });
    return out;
  case LogicalTypeId::DOUBLE:
    Fill<double, double>(vec, rows, out, core::AuxType::Double, [](double v) { return v; });
    return out;
  case LogicalTypeId::DECIMAL: {
    out.scale = static_cast<int32_t>(::duckdb::DecimalType::GetScale(type));
    switch (type.InternalType()) {
    case PhysicalType::INT16:
      Fill<int16_t, int64_t>(vec, rows, out, core::AuxType::ScaledDecimal, as_int64);
      return out;
    case PhysicalType::INT32:
      Fill<int32_t, int64_t>(vec, rows, out, core::AuxType::ScaledDecimal, as_int64);
      return out;
    case PhysicalType::INT64:
      Fill<int64_t, int64_t>(vec, rows, out, core::AuxType::ScaledDecimal, as_int64);
      return out;
    case PhysicalType::INT128: {
      bool fits = true;
      Fill<::duckdb::hugeint_t, int64_t>(vec, rows, out, core::AuxType::ScaledDecimal,
                                         [&](const ::duckdb::hugeint_t &h) {
                                           fits = fits && FitsInt64(h);
                                           return static_cast<int64_t>(h.lower);
                                         });
      if (fits) {
        return out;
      }
      // Beyond 64 bits: exact as decimal text, never via double.
      out = ColumnData();
      FillText(vec, rows, out);
      return out;
    }
    default:
      break;
    }
    break;
  }
  case LogicalTypeId::VARCHAR: {
    ::duckdb::UnifiedVectorFormat fmt;
    vec.ToUnifiedFormat(rows, fmt);
    const auto *data = ::duckdb::UnifiedVectorFormat::GetData<::duckdb::string_t>(fmt);
    runtime::StringColumnData text;
    text.offsets.reserve(rows + 1);
    text.offsets.push_back(0);
    for (std::size_t r = 0; r < rows; ++r) {
      const auto idx = fmt.sel->get_index(r);
      if (!fmt.validity.RowIsValid(idx)) {
        if (out.null_flags.empty()) {
          out.null_flags.assign(rows, 0);
        }
        out.null_flags[r] = 1;
      } else {
        text.bytes.append(data[idx].GetData(), data[idx].GetSize());
      }
      text.offsets.push_back(text.bytes.size());
    }
    out.type = core::AuxType::String;
    out.values = std::move(text);
    return out;
  }
  default:
    break;
  }
  out = ColumnData();
  FillText(vec, rows, out);
  return out;
}

core::MutationKind Kind(::duckdb::TableMutationKind kind) {
  switch (kind) {
  case ::duckdb::TableMutationKind::INSERT:
    return core::MutationKind::Insert;
  case ::duckdb::TableMutationKind::DELETE:
    return core::MutationKind::Delete;
  default:
    return core::MutationKind::Update;
  }
}

} // namespace

runtime::ColumnarBatch ToColumnarBatch(const ::duckdb::TableMutationEvent &event,
                                       core::TableId table) {
  runtime::ColumnarBatch batch;
  batch.table = table;
  batch.kind = Kind(event.kind);
  batch.row_count = event.row_count;
  // Updates: the first old_count vectors are every watched column's old
  // value, the rest the updated columns' new values. Inserts / deletes: one
  // vector per watched column.
  const std::size_t olds =
      batch.kind == core::MutationKind::Update ? event.old_count : event.columns.size();
  runtime::ColumnSet first;
  for (std::size_t i = 0; i < olds; ++i) {
    first.attnums.push_back(static_cast<int32_t>(event.columns[i].index));
    first.columns.push_back(Column(*event.vectors[i], event.row_count));
  }
  switch (batch.kind) {
  case core::MutationKind::Insert:
    batch.new_rows = std::move(first);
    break;
  case core::MutationKind::Delete:
    batch.old_rows = std::move(first);
    break;
  case core::MutationKind::Update: {
    // New row: the old one with the updated columns replaced.
    batch.new_rows = first;
    for (std::size_t i = olds; i < event.columns.size(); ++i) {
      const auto attnum = static_cast<int32_t>(event.columns[i].index);
      const int pos = batch.new_rows.Find(attnum);
      if (pos >= 0) {
        batch.new_rows.columns[pos] = Column(*event.vectors[i], event.row_count);
      }
    }
    for (const auto &u : event.updated_columns) {
      batch.updated_attnums.push_back(static_cast<int32_t>(u.index));
    }
    batch.old_rows = std::move(first);
    break;
  }
  }
  return batch;
}

// ---- engine callbacks -> host ----

void DatabaseCapture::OnTableMutation(::duckdb::ClientContext &context,
                                      const ::duckdb::TableMutationEvent &event) {
  host.OnMutation(context, event);
}

void DatabaseCapture::TransactionPreCommit(::duckdb::MetaTransaction &transaction,
                                           ::duckdb::ClientContext &) {
  host.OnPreCommit(transaction);
}

void DatabaseCapture::TransactionCommit(::duckdb::MetaTransaction &transaction,
                                        ::duckdb::ClientContext &context) {
  host.OnCommit(transaction, context);
}

void DatabaseCapture::TransactionRollback(::duckdb::MetaTransaction &transaction,
                                          ::duckdb::ClientContext &) {
  host.OnRollback(transaction);
}

void CommitIds::OnCommitWithContext(::duckdb::ClientContext &context,
                                    ::duckdb::AttachedDatabase &db,
                                    ::duckdb::transaction_t commit_id) {
  host.OnCommitId(context, db, commit_id);
}

} // namespace accelerator_codegen::aux_state::engine::duckdb
