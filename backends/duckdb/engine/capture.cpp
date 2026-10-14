// GENERATED BACKEND
#include "backends/duckdb/engine/capture.hpp"

#include <algorithm>
#include <cstddef>

#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "backends/duckdb/engine/scan.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/storage/object_cache.hpp"

namespace accelerator_codegen::aux_state::engine::duckdb {

using namespace scan;

AuxWatchedColumns &WatchedColumns(::duckdb::ClientContext &context) {
  auto &cache = ::duckdb::DatabaseInstance::GetDatabase(context).GetObjectCache();
  auto entry = cache.GetOrCreate<AuxWatchedColumns>(AuxWatchedColumns::kKey);
  return *entry;
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
                          : core::AuxValue::Of(core::AuxType::Date, core::AuxDate {DateDays(value)});
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
    decimal.value = DecimalRaw(value);
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

namespace {

// Converts column `col` of an event (`rows` rows, vector `vec`) into the row-major `out` (row stride `width`),
// reading the vector's physical data directly. Produces the same AuxValue as CapturedValue(vec.GetValue(r)) for
// every row; types without a direct path go through that.
void CapturedColumn(::duckdb::Vector &vec, std::size_t rows, std::vector<core::AuxValue> &out,
                    std::size_t width, std::size_t col) {
  using ::duckdb::LogicalTypeId;
  using ::duckdb::PhysicalType;
  ::duckdb::UnifiedVectorFormat fmt;
  vec.ToUnifiedFormat(rows, fmt);
  const auto &type = vec.GetType();
  auto at = [&](std::size_t r) -> core::AuxValue & { return out[r * width + col]; };
  auto fill = [&](core::AuxType aux_type, auto &&get) {
    for (std::size_t r = 0; r < rows; ++r) {
      const auto idx = fmt.sel->get_index(r);
      auto &dst = at(r);
      dst.type = aux_type;
      if (!fmt.validity.RowIsValid(idx)) {
        dst.is_null = true;
        continue;
      }
      dst.is_null = false;
      dst.payload = get(idx);
    }
  };
  auto fill_null = [&](core::AuxType aux_type) {
    for (std::size_t r = 0; r < rows; ++r) {
      auto &dst = at(r);
      dst.type = aux_type;
      dst.is_null = true;
    }
  };
  switch (type.id()) {
  case LogicalTypeId::BOOLEAN: {
    const auto *data = ::duckdb::UnifiedVectorFormat::GetData<bool>(fmt);
    fill(core::AuxType::Boolean, [&](::duckdb::idx_t i) -> bool { return data[i]; });
    return;
  }
  case LogicalTypeId::TINYINT: {
    const auto *data = ::duckdb::UnifiedVectorFormat::GetData<int8_t>(fmt);
    fill(core::AuxType::Int64, [&](::duckdb::idx_t i) -> int64_t { return data[i]; });
    return;
  }
  case LogicalTypeId::SMALLINT: {
    const auto *data = ::duckdb::UnifiedVectorFormat::GetData<int16_t>(fmt);
    fill(core::AuxType::Int64, [&](::duckdb::idx_t i) -> int64_t { return data[i]; });
    return;
  }
  case LogicalTypeId::INTEGER: {
    const auto *data = ::duckdb::UnifiedVectorFormat::GetData<int32_t>(fmt);
    fill(core::AuxType::Int64, [&](::duckdb::idx_t i) -> int64_t { return data[i]; });
    return;
  }
  case LogicalTypeId::BIGINT: {
    const auto *data = ::duckdb::UnifiedVectorFormat::GetData<int64_t>(fmt);
    fill(core::AuxType::Int64, [&](::duckdb::idx_t i) -> int64_t { return data[i]; });
    return;
  }
  case LogicalTypeId::DATE: {
    const auto *data = ::duckdb::UnifiedVectorFormat::GetData<::duckdb::date_t>(fmt);
    fill(core::AuxType::Date, [&](::duckdb::idx_t i) { return core::AuxDate {::duckdb::Date::EpochDays(data[i])}; });
    return;
  }
  case LogicalTypeId::FLOAT: {
    const auto *data = ::duckdb::UnifiedVectorFormat::GetData<float>(fmt);
    fill(core::AuxType::Double, [&](::duckdb::idx_t i) -> double { return static_cast<double>(data[i]); });
    return;
  }
  case LogicalTypeId::DOUBLE: {
    const auto *data = ::duckdb::UnifiedVectorFormat::GetData<double>(fmt);
    fill(core::AuxType::Double, [&](::duckdb::idx_t i) -> double { return data[i]; });
    return;
  }
  case LogicalTypeId::DECIMAL: {
    const auto scale = static_cast<int32_t>(::duckdb::DecimalType::GetScale(type));
    auto scaled = [&](int64_t raw) {
      core::ScaledDecimal decimal;
      decimal.value = raw;
      decimal.scale = scale;
      return decimal;
    };
    switch (type.InternalType()) {
    case PhysicalType::INT16: {
      const auto *data = ::duckdb::UnifiedVectorFormat::GetData<int16_t>(fmt);
      fill(core::AuxType::ScaledDecimal, [&](::duckdb::idx_t i) { return scaled(data[i]); });
      return;
    }
    case PhysicalType::INT32: {
      const auto *data = ::duckdb::UnifiedVectorFormat::GetData<int32_t>(fmt);
      fill(core::AuxType::ScaledDecimal, [&](::duckdb::idx_t i) { return scaled(data[i]); });
      return;
    }
    case PhysicalType::INT64: {
      const auto *data = ::duckdb::UnifiedVectorFormat::GetData<int64_t>(fmt);
      fill(core::AuxType::ScaledDecimal, [&](::duckdb::idx_t i) { return scaled(data[i]); });
      return;
    }
    case PhysicalType::INT128: {
      const auto *data = ::duckdb::UnifiedVectorFormat::GetData<::duckdb::hugeint_t>(fmt);
      for (std::size_t r = 0; r < rows; ++r) {
        const auto idx = fmt.sel->get_index(r);
        auto &dst = at(r);
        if (!fmt.validity.RowIsValid(idx)) {
          dst = core::AuxValue::Null(core::AuxType::ScaledDecimal);
          continue;
        }
        const auto huge = data[idx];
        const bool fits = (huge.upper == 0 && static_cast<int64_t>(huge.lower) >= 0) ||
                          (huge.upper == -1 && static_cast<int64_t>(huge.lower) < 0);
        if (fits) {
          dst = core::AuxValue::Of(core::AuxType::ScaledDecimal, scaled(static_cast<int64_t>(huge.lower)));
        } else { // beyond 64 bits: exact as decimal text, never via double
          dst = CapturedValue(vec.GetValue(r));
        }
      }
      return;
    }
    default:
      break;
    }
    break;
  }
  case LogicalTypeId::VARCHAR: {
    const auto *data = ::duckdb::UnifiedVectorFormat::GetData<::duckdb::string_t>(fmt);
    fill(core::AuxType::String, [&](::duckdb::idx_t i) { return data[i].GetString(); });
    return;
  }
  case LogicalTypeId::SQLNULL:
    fill_null(core::AuxType::String);
    return;
  default:
    break;
  }
  for (std::size_t r = 0; r < rows; ++r) {
    at(r) = CapturedValue(vec.GetValue(r));
  }
}

} // namespace

void AuxCaptureHub::OnTableMutation(::duckdb::ClientContext &, const ::duckdb::TableMutationEvent &event) {
  if (event.row_count == 0) {
    return;
  }
  std::vector<DuckdbRuntime *> targets;
  {
    std::lock_guard<std::mutex> lock(mutex);
    targets = runtimes;
  }
  if (targets.empty()) {
    return;
  }
  DuckdbRuntime::CapturedEvent shared;
  shared.table_name = event.table_name;
  shared.kind = event.kind == ::duckdb::TableMutationKind::INSERT   ? core::MutationKind::Insert
                : event.kind == ::duckdb::TableMutationKind::DELETE ? core::MutationKind::Delete
                                                                    : core::MutationKind::Update;
  shared.updated_columns.reserve(event.updated_columns.size());
  for (const auto &u : event.updated_columns) {
    shared.updated_columns.push_back(static_cast<int32_t>(u.index));
  }
  // Every row of an event has the same shape (see MutationRecorder).
  shared.rows = event.row_count;
  shared.width = event.columns.size();
  shared.olds = shared.kind == core::MutationKind::Update ? event.old_count : shared.width;
  shared.columns.reserve(shared.width);
  for (const auto &c : event.columns) {
    shared.columns.push_back(static_cast<int32_t>(c.index));
  }
  shared.values.resize(shared.rows * shared.width);
  for (std::size_t c = 0; c < shared.width; ++c) {
    CapturedColumn(*event.vectors[c], shared.rows, shared.values, shared.width, c);
  }
  for (auto *runtime : targets) {
    runtime->OnCapturedEvent(shared);
  }
}

} // namespace accelerator_codegen::aux_state::engine::duckdb
