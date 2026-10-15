// SCAFFOLDING
// Wire encoding of ColumnarBatch, query arguments and result rows (wire.hpp).
#include "substrate/aux/runtime/wire.hpp"

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace accelerator_codegen::aux_state::runtime {

using core::AuxDate;
using core::AuxRow;
using core::AuxType;
using core::AuxValue;
using core::MutationKind;
using core::ScaledDecimal;

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "the wire format is little-endian");

namespace {

constexpr uint8_t kWireVersion = 1;
constexpr uint8_t kLastAuxType = static_cast<uint8_t>(AuxType::ScaledDecimal128);
constexpr uint8_t kLastMutationKind = static_cast<uint8_t>(MutationKind::Update);

[[noreturn]] void Malformed(const char *what) {
  throw std::runtime_error(std::string("wire: malformed input: ") + what);
}

// ---- writing ----

template <typename T> void Put(std::string &out, T value) {
  out.append(reinterpret_cast<const char *>(&value), sizeof(T));
}

template <typename T> void PutArray(std::string &out, const std::vector<T> &values) {
  out.append(reinterpret_cast<const char *>(values.data()), values.size() * sizeof(T));
}

void PutBytes(std::string &out, std::string_view bytes) {
  Put<uint64_t>(out, bytes.size());
  out.append(bytes);
}

// ---- reading ----

class Reader {
public:
  explicit Reader(std::string_view &in) : in_(in) {}

  template <typename T> T Get() {
    T value;
    std::memcpy(&value, Take(sizeof(T)), sizeof(T));
    return value;
  }

  // `count` elements of T, copied into a vector. The size is checked against
  // the remaining input first, so a corrupt count cannot trigger a huge allocation.
  template <typename T> std::vector<T> GetArray(uint64_t count) {
    if (count > in_.size() / sizeof(T)) {
      Malformed("truncated");
    }
    std::vector<T> values(count);
    std::memcpy(values.data(), Take(count * sizeof(T)), count * sizeof(T));
    return values;
  }

  std::string_view GetBytes() {
    uint64_t size = Get<uint64_t>();
    return std::string_view(Take(size), size);
  }

  // A u64 element count whose elements take at least `min_element_size` bytes each.
  uint64_t GetCount(std::size_t min_element_size) {
    uint64_t count = Get<uint64_t>();
    if (count > in_.size() / min_element_size) {
      Malformed("count exceeds input");
    }
    return count;
  }

  uint8_t GetType() {
    uint8_t type = Get<uint8_t>();
    if (type > kLastAuxType) {
      Malformed("unknown type");
    }
    return type;
  }

private:
  const char *Take(uint64_t size) {
    if (size > in_.size()) {
      Malformed("truncated");
    }
    const char *data = in_.data();
    in_.remove_prefix(size);
    return data;
  }

  std::string_view &in_;
};

// ---- AuxValue ----

void EncodeValue(const AuxValue &value, std::string &out) {
  Put<uint8_t>(out, static_cast<uint8_t>(value.type));
  Put<uint8_t>(out, value.is_null);
  if (value.is_null) {
    return;
  }
  switch (value.type) {
  case AuxType::Boolean:
    Put<uint8_t>(out, std::get<bool>(value.payload));
    return;
  case AuxType::Int64:
    Put(out, std::get<int64_t>(value.payload));
    return;
  case AuxType::UInt64:
    Put(out, std::get<uint64_t>(value.payload));
    return;
  case AuxType::ScaledDecimal: {
    const auto &decimal = std::get<ScaledDecimal>(value.payload);
    Put(out, decimal.value);
    Put(out, decimal.scale);
    return;
  }
  case AuxType::ScaledDecimal128: {
    const auto &decimal = std::get<core::ScaledDecimal128>(value.payload);
    Put(out, decimal.value);
    Put(out, decimal.scale);
    return;
  }
  case AuxType::Double:
    Put(out, std::get<double>(value.payload));
    return;
  case AuxType::Date:
    Put(out, std::get<AuxDate>(value.payload).days_since_epoch);
    return;
  case AuxType::String:
    PutBytes(out, std::get<std::string>(value.payload));
    return;
  }
  throw std::logic_error("wire: unknown AuxType");
}

AuxValue DecodeValue(Reader &reader) {
  auto type = static_cast<AuxType>(reader.GetType());
  uint8_t is_null = reader.Get<uint8_t>();
  if (is_null > 1) {
    Malformed("null flag");
  }
  if (is_null) {
    return AuxValue::Null(type);
  }
  switch (type) {
  case AuxType::Boolean: {
    uint8_t flag = reader.Get<uint8_t>();
    if (flag > 1) {
      Malformed("boolean");
    }
    return AuxValue::Of<bool>(type, flag != 0);
  }
  case AuxType::Int64:
    return AuxValue::Of<int64_t>(type, reader.Get<int64_t>());
  case AuxType::UInt64:
    return AuxValue::Of<uint64_t>(type, reader.Get<uint64_t>());
  case AuxType::ScaledDecimal: {
    int64_t unscaled = reader.Get<int64_t>();
    return AuxValue::Of<ScaledDecimal>(type, ScaledDecimal{unscaled, reader.Get<int32_t>()});
  }
  case AuxType::ScaledDecimal128: {
    __int128 unscaled = reader.Get<__int128>();
    return AuxValue::Of<core::ScaledDecimal128>(type, core::ScaledDecimal128{unscaled, reader.Get<int32_t>()});
  }
  case AuxType::Double:
    return AuxValue::Of<double>(type, reader.Get<double>());
  case AuxType::Date:
    return AuxValue::Of<AuxDate>(type, AuxDate{reader.Get<int32_t>()});
  case AuxType::String:
    return AuxValue::Of<std::string>(type, std::string(reader.GetBytes()));
  }
  Malformed("unknown type");
}

// u64 count | AuxValue[count]
std::vector<AuxValue> DecodeValues(Reader &reader) {
  uint64_t count = reader.GetCount(2);
  std::vector<AuxValue> values;
  values.reserve(count);
  for (uint64_t i = 0; i < count; ++i) {
    values.push_back(DecodeValue(reader));
  }
  return values;
}

// ---- columns ----

void EncodeColumn(const ColumnData &column, std::size_t row_count, std::string &out) {
  bool sized = column.null_flags.empty() || column.null_flags.size() == row_count;
  std::visit(
      [&](const auto &values) {
        if constexpr (std::is_same_v<std::decay_t<decltype(values)>, StringColumnData>) {
          sized = sized && values.offsets.size() == row_count + 1;
        } else {
          sized = sized && values.size() == row_count;
        }
      },
      column.values);
  if (!sized) {
    throw std::invalid_argument("EncodeBatch: column size differs from row_count");
  }
  Put<uint8_t>(out, static_cast<uint8_t>(column.type));
  Put(out, column.scale);
  Put<uint8_t>(out, !column.null_flags.empty());
  PutArray(out, column.null_flags);
  std::visit(
      [&](const auto &values) {
        using Values = std::decay_t<decltype(values)>;
        if constexpr (std::is_same_v<Values, StringColumnData>) {
          PutArray(out, values.offsets);
          PutBytes(out, values.bytes);
        } else {
          PutArray(out, values);
        }
      },
      column.values);
}

void CheckStringColumn(const StringColumnData &strings) {
  if (strings.offsets.empty() || strings.offsets.front() != 0 || strings.offsets.back() != strings.bytes.size()) {
    Malformed("string offsets");
  }
  for (std::size_t i = 1; i < strings.offsets.size(); ++i) {
    if (strings.offsets[i] < strings.offsets[i - 1]) {
      Malformed("string offsets");
    }
  }
}

ColumnData DecodeColumn(Reader &reader, std::size_t row_count) {
  ColumnData column;
  column.type = static_cast<AuxType>(reader.GetType());
  column.scale = reader.Get<int32_t>();
  uint8_t has_nulls = reader.Get<uint8_t>();
  if (has_nulls > 1) {
    Malformed("null flag");
  }
  if (has_nulls) {
    column.null_flags = reader.GetArray<uint8_t>(row_count);
  }
  switch (column.type) {
  case AuxType::Boolean:
    column.values = reader.GetArray<uint8_t>(row_count);
    break;
  case AuxType::Int64:
  case AuxType::ScaledDecimal:
    column.values = reader.GetArray<int64_t>(row_count);
    break;
  case AuxType::UInt64:
    column.values = reader.GetArray<uint64_t>(row_count);
    break;
  case AuxType::ScaledDecimal128:
    column.values = reader.GetArray<__int128>(row_count);
    break;
  case AuxType::Double:
    column.values = reader.GetArray<double>(row_count);
    break;
  case AuxType::Date:
    column.values = reader.GetArray<int32_t>(row_count);
    break;
  case AuxType::String: {
    StringColumnData strings;
    strings.offsets = reader.GetArray<uint64_t>(row_count + 1);
    strings.bytes = std::string(reader.GetBytes());
    CheckStringColumn(strings);
    column.values = std::move(strings);
    break;
  }
  }
  return column;
}

// ---- column sets ----

void EncodeColumnSet(const ColumnSet &set, std::size_t row_count, std::string &out) {
  Put<uint64_t>(out, set.columns.size());
  PutArray(out, set.attnums);
  for (const auto &column : set.columns) {
    EncodeColumn(column, row_count, out);
  }
}

ColumnSet DecodeColumnSet(Reader &reader, std::size_t row_count) {
  ColumnSet set;
  uint64_t count = reader.GetCount(sizeof(int32_t));
  set.attnums = reader.GetArray<int32_t>(count);
  set.columns.reserve(count);
  for (uint64_t i = 0; i < count; ++i) {
    set.columns.push_back(DecodeColumn(reader, row_count));
  }
  return set;
}

} // namespace

void EncodeBatch(const ColumnarBatch &batch, std::string &out) {
  if (batch.old_rows.attnums.size() != batch.old_rows.columns.size() ||
      batch.new_rows.attnums.size() != batch.new_rows.columns.size()) {
    throw std::invalid_argument("EncodeBatch: attnums and columns differ in size");
  }
  Put(out, kWireVersion);
  Put<uint64_t>(out, batch.table);
  Put<uint8_t>(out, static_cast<uint8_t>(batch.kind));
  Put<uint64_t>(out, batch.row_count);
  EncodeColumnSet(batch.old_rows, batch.row_count, out);
  EncodeColumnSet(batch.new_rows, batch.row_count, out);
  Put<uint64_t>(out, batch.updated_attnums.size());
  PutArray(out, batch.updated_attnums);
}

ColumnarBatch DecodeBatch(std::string_view &in) {
  Reader reader(in);
  if (reader.Get<uint8_t>() != kWireVersion) {
    Malformed("unsupported version");
  }
  ColumnarBatch batch;
  batch.table = reader.Get<uint64_t>();
  uint8_t kind = reader.Get<uint8_t>();
  if (kind > kLastMutationKind) {
    Malformed("unknown mutation kind");
  }
  batch.kind = static_cast<MutationKind>(kind);
  batch.row_count = reader.Get<uint64_t>();
  batch.old_rows = DecodeColumnSet(reader, batch.row_count);
  batch.new_rows = DecodeColumnSet(reader, batch.row_count);
  batch.updated_attnums = reader.GetArray<int32_t>(reader.Get<uint64_t>());
  return batch;
}

void EncodeArguments(const core::QueryArguments &args, std::string &out) {
  Put<uint64_t>(out, args.values.size());
  for (const auto &value : args.values) {
    EncodeValue(value, out);
  }
}

core::QueryArguments DecodeArguments(std::string_view &in) {
  Reader reader(in);
  core::QueryArguments args;
  args.values = DecodeValues(reader);
  return args;
}

void EncodeRows(const std::vector<AuxRow> &rows, std::string &out) {
  Put<uint64_t>(out, rows.size());
  for (const auto &row : rows) {
    Put<uint64_t>(out, row.values.size());
    for (const auto &value : row.values) {
      EncodeValue(value, out);
    }
  }
}

std::vector<AuxRow> DecodeRows(std::string_view &in) {
  Reader reader(in);
  std::vector<AuxRow> rows(reader.GetCount(sizeof(uint64_t)));
  for (auto &row : rows) {
    row.values = DecodeValues(reader);
  }
  return rows;
}

} // namespace accelerator_codegen::aux_state::runtime
