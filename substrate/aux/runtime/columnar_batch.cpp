// SCAFFOLDING
// C5: ColumnarBatch value access, projection to the legacy row view, and back.
#include "substrate/aux/runtime/columnar_batch.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace accelerator_codegen::aux_state::runtime {

using core::AuxDate;
using core::AuxRow;
using core::AuxType;
using core::AuxValue;
using core::ScaledDecimal;

core::AuxValue ColumnData::Value(std::size_t row) const {
  if (IsNull(row)) {
    return AuxValue::Null(type);
  }
  switch (type) {
  case AuxType::Boolean:
    return AuxValue::Of<bool>(type, std::get<std::vector<uint8_t>>(values)[row] != 0);
  case AuxType::Int64:
    return AuxValue::Of<int64_t>(type, std::get<std::vector<int64_t>>(values)[row]);
  case AuxType::UInt64:
    return AuxValue::Of<uint64_t>(type, std::get<std::vector<uint64_t>>(values)[row]);
  case AuxType::ScaledDecimal:
    return AuxValue::Of<ScaledDecimal>(
        type, ScaledDecimal{std::get<std::vector<int64_t>>(values)[row], scale});
  case AuxType::ScaledDecimal128:
    return AuxValue::Of<core::ScaledDecimal128>(
        type, core::ScaledDecimal128{std::get<std::vector<__int128>>(values)[row], scale});
  case AuxType::Double:
    return AuxValue::Of<double>(type, std::get<std::vector<double>>(values)[row]);
  case AuxType::Date:
    return AuxValue::Of<AuxDate>(type, AuxDate{std::get<std::vector<int32_t>>(values)[row]});
  case AuxType::String:
    return AuxValue::Of<std::string>(
        type, std::string(std::get<StringColumnData>(values).At(row)));
  }
  throw std::logic_error("ColumnData::Value: unknown AuxType");
}

int ColumnSet::Find(int32_t attnum) const {
  for (std::size_t i = 0; i < attnums.size(); ++i) {
    if (attnums[i] == attnum) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

namespace {

// Appends column `col` to every row of `rows` (column-major: the typed
// storage is resolved once per column, not once per cell).
void AppendColumn(const ColumnData &col, std::size_t row_count, std::vector<AuxRow> &rows) {
  const bool has_nulls = !col.null_flags.empty();
  auto fill = [&](auto &&make) {
    if (!has_nulls) {
      for (std::size_t r = 0; r < row_count; ++r) {
        rows[r].values.push_back(make(r));
      }
    } else {
      for (std::size_t r = 0; r < row_count; ++r) {
        rows[r].values.push_back(col.null_flags[r] != 0 ? AuxValue::Null(col.type) : make(r));
      }
    }
  };
  switch (col.type) {
  case AuxType::Boolean: {
    const auto &v = std::get<std::vector<uint8_t>>(col.values);
    fill([&](std::size_t r) { return AuxValue::Of<bool>(AuxType::Boolean, v[r] != 0); });
    break;
  }
  case AuxType::Int64: {
    const auto &v = std::get<std::vector<int64_t>>(col.values);
    fill([&](std::size_t r) { return AuxValue::Of<int64_t>(AuxType::Int64, v[r]); });
    break;
  }
  case AuxType::UInt64: {
    const auto &v = std::get<std::vector<uint64_t>>(col.values);
    fill([&](std::size_t r) { return AuxValue::Of<uint64_t>(AuxType::UInt64, v[r]); });
    break;
  }
  case AuxType::ScaledDecimal: {
    const auto &v = std::get<std::vector<int64_t>>(col.values);
    const int32_t scale = col.scale;
    fill([&](std::size_t r) {
      return AuxValue::Of<ScaledDecimal>(AuxType::ScaledDecimal, ScaledDecimal{v[r], scale});
    });
    break;
  }
  case AuxType::ScaledDecimal128: {
    const auto &v = std::get<std::vector<__int128>>(col.values);
    const int32_t scale = col.scale;
    fill([&](std::size_t r) {
      return AuxValue::Of<core::ScaledDecimal128>(AuxType::ScaledDecimal128,
                                                  core::ScaledDecimal128{v[r], scale});
    });
    break;
  }
  case AuxType::Double: {
    const auto &v = std::get<std::vector<double>>(col.values);
    fill([&](std::size_t r) { return AuxValue::Of<double>(AuxType::Double, v[r]); });
    break;
  }
  case AuxType::Date: {
    const auto &v = std::get<std::vector<int32_t>>(col.values);
    fill([&](std::size_t r) { return AuxValue::Of<AuxDate>(AuxType::Date, AuxDate{v[r]}); });
    break;
  }
  case AuxType::String: {
    const auto &v = std::get<StringColumnData>(col.values);
    fill([&](std::size_t r) {
      return AuxValue::Of<std::string>(AuxType::String, std::string(v.At(r)));
    });
    break;
  }
  }
}

std::vector<AuxRow> BuildRows(const ColumnSet &set, const std::vector<int32_t> &projection,
                              std::size_t row_count) {
  std::vector<const ColumnData *> cols;
  cols.reserve(projection.size());
  for (int32_t attnum : projection) {
    const int pos = set.Find(attnum);
    if (pos < 0) {
      throw std::invalid_argument("ToMutationBatch: attnum " + std::to_string(attnum) +
                                  " was not captured");
    }
    cols.push_back(&set.columns[static_cast<std::size_t>(pos)]);
  }
  std::vector<AuxRow> rows(row_count);
  for (auto &row : rows) {
    row.values.reserve(cols.size());
  }
  for (const ColumnData *col : cols) {
    AppendColumn(*col, row_count, rows);
  }
  return rows;
}

} // namespace

core::MutationBatch ToMutationBatch(const ColumnarBatch &batch,
                                    const std::vector<int32_t> &projection) {
  const bool has_old = batch.kind != core::MutationKind::Insert;
  const bool has_new = batch.kind != core::MutationKind::Delete;

  std::vector<int32_t> proj = projection;
  if (proj.empty()) {
    // Every captured column of the sets this kind uses, in attnum order.
    if (has_old) {
      proj.insert(proj.end(), batch.old_rows.attnums.begin(), batch.old_rows.attnums.end());
    }
    if (has_new) {
      proj.insert(proj.end(), batch.new_rows.attnums.begin(), batch.new_rows.attnums.end());
    }
    std::sort(proj.begin(), proj.end());
    proj.erase(std::unique(proj.begin(), proj.end()), proj.end());
  }

  core::MutationBatch out;
  out.table = batch.table;
  out.kind = batch.kind;
  if (has_old) {
    out.old_rows = BuildRows(batch.old_rows, proj, batch.row_count);
  }
  if (has_new) {
    out.new_rows = BuildRows(batch.new_rows, proj, batch.row_count);
  }
  // A column outside the projection is reported as position proj.size()
  // (once): the module learns that none of its columns changed.
  bool outside = false;
  for (int32_t attnum : batch.updated_attnums) {
    const auto it = std::find(proj.begin(), proj.end(), attnum);
    if (it != proj.end()) {
      out.updated_columns.push_back(static_cast<core::ColumnId>(it - proj.begin()));
    } else {
      outside = true;
    }
  }
  if (outside) {
    out.updated_columns.push_back(static_cast<core::ColumnId>(proj.size()));
  }
  return out;
}

namespace {

// A column's type and scale, from its first non-NULL value (else the type
// its NULLs declare).
struct Inferred {
  AuxType type = AuxType::String;
  int32_t scale = 0;
  bool found = false;
};

void Infer(const std::vector<AuxRow> &rows, std::size_t col, Inferred &inf) {
  for (const AuxRow &row : rows) {
    const AuxValue &v = row.values.at(col);
    if (v.is_null) {
      continue;
    }
    if (!inf.found) {
      inf.found = true;
      inf.type = v.type;
      if (v.type == AuxType::ScaledDecimal) {
        inf.scale = std::get<ScaledDecimal>(v.payload).scale;
      } else if (v.type == AuxType::ScaledDecimal128) {
        inf.scale = std::get<core::ScaledDecimal128>(v.payload).scale;
      }
    }
    return;
  }
}

ColumnData BuildColumn(const std::vector<AuxRow> &rows, std::size_t col, const Inferred &inf) {
  const std::size_t n = rows.size();
  ColumnData out;
  out.type = inf.type;
  out.scale = inf.type == AuxType::ScaledDecimal || inf.type == AuxType::ScaledDecimal128 ? inf.scale : 0;
  bool any_null = false;
  std::vector<uint8_t> nulls(n, 0);
  for (std::size_t r = 0; r < n; ++r) {
    if (rows[r].values.at(col).is_null) {
      nulls[r] = 1;
      any_null = true;
    }
  }
  auto numeric = [&](auto &vec, auto get) {
    vec.assign(n, {});
    for (std::size_t r = 0; r < n; ++r) {
      if (!nulls[r]) {
        vec[r] = get(rows[r].values[col]);
      }
    }
  };
  switch (inf.type) {
  case AuxType::Boolean: {
    std::vector<uint8_t> v;
    numeric(v, [](const AuxValue &x) { return static_cast<uint8_t>(std::get<bool>(x.payload)); });
    out.values = std::move(v);
    break;
  }
  case AuxType::Int64: {
    std::vector<int64_t> v;
    numeric(v, [](const AuxValue &x) { return std::get<int64_t>(x.payload); });
    out.values = std::move(v);
    break;
  }
  case AuxType::UInt64: {
    std::vector<uint64_t> v;
    numeric(v, [](const AuxValue &x) { return std::get<uint64_t>(x.payload); });
    out.values = std::move(v);
    break;
  }
  case AuxType::ScaledDecimal: {
    std::vector<int64_t> v;
    numeric(v, [&](const AuxValue &x) {
      const auto &d = std::get<ScaledDecimal>(x.payload);
      if (d.scale != inf.scale) {
        throw std::invalid_argument("FromMutationBatch: ScaledDecimal scale differs within a column");
      }
      return d.value;
    });
    out.values = std::move(v);
    break;
  }
  case AuxType::ScaledDecimal128: {
    std::vector<__int128> v;
    numeric(v, [&](const AuxValue &x) {
      const auto &d = std::get<core::ScaledDecimal128>(x.payload);
      if (d.scale != inf.scale) {
        throw std::invalid_argument("FromMutationBatch: ScaledDecimal128 scale differs within a column");
      }
      return d.value;
    });
    out.values = std::move(v);
    break;
  }
  case AuxType::Double: {
    std::vector<double> v;
    numeric(v, [](const AuxValue &x) { return std::get<double>(x.payload); });
    out.values = std::move(v);
    break;
  }
  case AuxType::Date: {
    std::vector<int32_t> v;
    numeric(v, [](const AuxValue &x) { return std::get<AuxDate>(x.payload).days_since_epoch; });
    out.values = std::move(v);
    break;
  }
  case AuxType::String: {
    StringColumnData s;
    s.offsets.reserve(n + 1);
    s.offsets.push_back(0);
    std::size_t total = 0;
    for (std::size_t r = 0; r < n; ++r) {
      if (!nulls[r]) {
        total += std::get<std::string>(rows[r].values[col].payload).size();
      }
    }
    s.bytes.reserve(total);
    for (std::size_t r = 0; r < n; ++r) {
      if (!nulls[r]) {
        s.bytes += std::get<std::string>(rows[r].values[col].payload);
      }
      s.offsets.push_back(s.bytes.size());
    }
    out.values = std::move(s);
    break;
  }
  }
  if (any_null) {
    out.null_flags = std::move(nulls);
  }
  return out;
}

} // namespace

ColumnarBatch FromMutationBatch(const core::MutationBatch &batch,
                                const std::vector<int32_t> &attnums) {
  ColumnarBatch out;
  out.table = batch.table;
  out.kind = batch.kind;
  out.row_count = std::max(batch.old_rows.size(), batch.new_rows.size());
  if (!batch.old_rows.empty() && !batch.new_rows.empty() &&
      batch.old_rows.size() != batch.new_rows.size()) {
    throw std::invalid_argument("FromMutationBatch: old_rows and new_rows differ in size");
  }
  const bool has_old = batch.kind != core::MutationKind::Insert;
  const bool has_new = batch.kind != core::MutationKind::Delete;
  for (std::size_t c = 0; c < attnums.size(); ++c) {
    Inferred inf;
    Infer(batch.old_rows, c, inf);
    if (!inf.found) {
      Infer(batch.new_rows, c, inf);
    }
    if (!inf.found) { // all NULL: the type the NULLs declare
      const auto &rows = batch.old_rows.empty() ? batch.new_rows : batch.old_rows;
      if (!rows.empty()) {
        inf.type = rows.front().values.at(c).type;
      }
    }
    if (has_old) {
      out.old_rows.columns.push_back(BuildColumn(batch.old_rows, c, inf));
    }
    if (has_new) {
      out.new_rows.columns.push_back(BuildColumn(batch.new_rows, c, inf));
    }
  }
  if (has_old) {
    out.old_rows.attnums = attnums;
  }
  if (has_new) {
    out.new_rows.attnums = attnums;
  }
  for (core::ColumnId col : batch.updated_columns) {
    out.updated_attnums.push_back(col < attnums.size() ? attnums[col] : kOutsideProjection);
  }
  return out;
}

namespace {

std::size_t ColumnBytes(const ColumnData &c) {
  std::size_t b = c.null_flags.size();
  std::visit(
      [&](const auto &v) {
        using V = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<V, StringColumnData>) {
          b += v.offsets.size() * sizeof(uint64_t) + v.bytes.size();
        } else {
          b += v.size() * sizeof(typename V::value_type);
        }
      },
      c.values);
  return b;
}

std::size_t SetBytes(const ColumnSet &s) {
  std::size_t b = s.attnums.size() * sizeof(int32_t);
  for (const auto &c : s.columns) {
    b += ColumnBytes(c);
  }
  return b;
}

} // namespace

std::size_t ByteSize(const ColumnarBatch &batch) {
  return SetBytes(batch.old_rows) + SetBytes(batch.new_rows) +
         batch.updated_attnums.size() * sizeof(int32_t);
}

} // namespace accelerator_codegen::aux_state::runtime
