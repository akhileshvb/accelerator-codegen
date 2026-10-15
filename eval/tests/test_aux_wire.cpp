// Tests of the wire encoding of ColumnarBatch, query arguments and result rows.
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "substrate/aux/runtime/wire.hpp"

using namespace accelerator_codegen::aux_state;
using namespace accelerator_codegen::aux_state::runtime;
using core::AuxDate;
using core::AuxRow;
using core::AuxType;
using core::AuxValue;
using core::MutationKind;
using core::ScaledDecimal;

namespace {

#define CHECK(cond)                                                                         \
  do {                                                                                      \
    if (!(cond)) {                                                                          \
      throw std::runtime_error(std::string(__FILE__ ":") + std::to_string(__LINE__) +       \
                               ": CHECK failed: " #cond);                                   \
    }                                                                                       \
  } while (0)

template <typename F> bool Throws(F &&f) {
  try {
    f();
  } catch (const std::runtime_error &) {
    return true;
  }
  return false;
}

const AuxType kAllTypes[] = {AuxType::Boolean, AuxType::Int64, AuxType::UInt64, AuxType::ScaledDecimal,
                             AuxType::Double,  AuxType::Date,  AuxType::String};

bool Same(const ColumnData &a, const ColumnData &b) {
  if (a.type != b.type || a.scale != b.scale || a.null_flags != b.null_flags ||
      a.values.index() != b.values.index()) {
    return false;
  }
  return std::visit(
      [&](const auto &values) {
        const auto &other = std::get<std::decay_t<decltype(values)>>(b.values);
        if constexpr (std::is_same_v<std::decay_t<decltype(values)>, StringColumnData>) {
          return values.offsets == other.offsets && values.bytes == other.bytes;
        } else {
          return values == other;
        }
      },
      a.values);
}

bool Same(const ColumnSet &a, const ColumnSet &b) {
  if (a.attnums != b.attnums || a.columns.size() != b.columns.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.columns.size(); ++i) {
    if (!Same(a.columns[i], b.columns[i])) {
      return false;
    }
  }
  return true;
}

bool Same(const ColumnarBatch &a, const ColumnarBatch &b) {
  return a.table == b.table && a.kind == b.kind && a.row_count == b.row_count &&
         Same(a.old_rows, b.old_rows) && Same(a.new_rows, b.new_rows) &&
         a.updated_attnums == b.updated_attnums;
}

bool Same(const AuxValue &a, const AuxValue &b) {
  return a.type == b.type && a.is_null == b.is_null && (a.is_null || a.payload == b.payload);
}

bool Same(const std::vector<AuxValue> &a, const std::vector<AuxValue> &b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (!Same(a[i], b[i])) {
      return false;
    }
  }
  return true;
}

// A column of `type` with `rows` rows; every third row is NULL when `with_nulls`.
ColumnData MakeColumn(AuxType type, std::size_t rows, bool with_nulls) {
  ColumnData column;
  column.type = type;
  if (type == AuxType::ScaledDecimal) {
    column.scale = 2;
  }
  if (with_nulls) {
    column.null_flags.resize(rows);
    for (std::size_t r = 0; r < rows; ++r) {
      column.null_flags[r] = r % 3 == 0;
    }
  }
  switch (type) {
  case AuxType::Boolean: {
    std::vector<uint8_t> v(rows);
    for (std::size_t r = 0; r < rows; ++r) v[r] = r % 2;
    column.values = std::move(v);
    break;
  }
  case AuxType::Int64:
  case AuxType::ScaledDecimal: {
    std::vector<int64_t> v(rows);
    for (std::size_t r = 0; r < rows; ++r) v[r] = static_cast<int64_t>(r) * -7919;
    if (rows > 1) v[1] = std::numeric_limits<int64_t>::min();
    column.values = std::move(v);
    break;
  }
  case AuxType::UInt64: {
    std::vector<uint64_t> v(rows);
    for (std::size_t r = 0; r < rows; ++r) v[r] = std::numeric_limits<uint64_t>::max() - r;
    column.values = std::move(v);
    break;
  }
  case AuxType::Double: {
    std::vector<double> v(rows);
    for (std::size_t r = 0; r < rows; ++r) v[r] = static_cast<double>(r) / 3.0;
    column.values = std::move(v);
    break;
  }
  case AuxType::Date: {
    std::vector<int32_t> v(rows);
    for (std::size_t r = 0; r < rows; ++r) v[r] = static_cast<int32_t>(r) - 5000;
    column.values = std::move(v);
    break;
  }
  case AuxType::String: {
    StringColumnData s;
    s.offsets.push_back(0);
    for (std::size_t r = 0; r < rows; ++r) {
      s.bytes.append(std::string(r % 5, static_cast<char>('a' + r % 26)));  // includes empty
      s.offsets.push_back(s.bytes.size());
    }
    column.values = std::move(s);
    break;
  }
  }
  return column;
}

ColumnSet MakeSet(std::size_t rows, bool with_nulls) {
  ColumnSet set;
  for (AuxType type : kAllTypes) {
    set.attnums.push_back(static_cast<int32_t>(set.columns.size()) * 2);
    set.columns.push_back(MakeColumn(type, rows, with_nulls));
  }
  return set;
}

ColumnarBatch MakeBatch(MutationKind kind, std::size_t rows, bool with_nulls) {
  ColumnarBatch batch;
  batch.table = 0x1234567890ULL;
  batch.kind = kind;
  batch.row_count = rows;
  if (kind != MutationKind::Insert) batch.old_rows = MakeSet(rows, with_nulls);
  if (kind != MutationKind::Delete) batch.new_rows = MakeSet(rows, with_nulls);
  if (kind == MutationKind::Update) batch.updated_attnums = {2, 6, 11};
  return batch;
}

void CheckRoundTrip(const ColumnarBatch &batch) {
  std::string wire = "prefix";  // Encode appends
  EncodeBatch(batch, wire);
  std::string_view in(wire);
  in.remove_prefix(6);
  ColumnarBatch decoded = DecodeBatch(in);
  CHECK(in.empty());
  CHECK(Same(batch, decoded));
}

void TestBatchRoundTrip() {
  for (MutationKind kind : {MutationKind::Insert, MutationKind::Delete, MutationKind::Update}) {
    for (bool with_nulls : {false, true}) {
      CheckRoundTrip(MakeBatch(kind, 100, with_nulls));
    }
  }
}

void TestEveryTypeAlone() {
  for (AuxType type : kAllTypes) {
    for (bool with_nulls : {false, true}) {
      ColumnarBatch batch;
      batch.kind = MutationKind::Insert;
      batch.row_count = 10;
      batch.new_rows.attnums = {3};
      batch.new_rows.columns.push_back(MakeColumn(type, 10, with_nulls));
      CheckRoundTrip(batch);
    }
  }
}

void TestEmptyBatches() {
  CheckRoundTrip(ColumnarBatch{});
  CheckRoundTrip(MakeBatch(MutationKind::Insert, 0, false));
  CheckRoundTrip(MakeBatch(MutationKind::Update, 0, true));
}

void TestSeveralBatchesInOneBuffer() {
  ColumnarBatch a = MakeBatch(MutationKind::Insert, 5, true);
  ColumnarBatch b = MakeBatch(MutationKind::Update, 7, false);
  std::string wire;
  EncodeBatch(a, wire);
  EncodeBatch(b, wire);
  std::string_view in(wire);
  CHECK(Same(a, DecodeBatch(in)));
  CHECK(Same(b, DecodeBatch(in)));
  CHECK(in.empty());
}

void TestLargeBatch() {
  ColumnarBatch batch;
  batch.kind = MutationKind::Insert;
  batch.row_count = 1000000;
  batch.new_rows.attnums = {0, 1, 2, 3};
  for (AuxType type : {AuxType::Int64, AuxType::Double, AuxType::Date, AuxType::String}) {
    batch.new_rows.columns.push_back(MakeColumn(type, batch.row_count, true));
  }
  auto start = std::chrono::steady_clock::now();
  std::string wire;
  EncodeBatch(batch, wire);
  std::string_view in(wire);
  ColumnarBatch decoded = DecodeBatch(in);
  double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::cout << "  1e6 rows: " << wire.size() << " bytes, encode+decode " << seconds << " s\n";
  CHECK(Same(batch, decoded));
  CHECK(seconds < 1.0);
}

void TestMalformedBatch() {
  std::string wire;
  EncodeBatch(MakeBatch(MutationKind::Update, 20, true), wire);
  // Every strict prefix is truncated input.
  for (std::size_t n = 0; n < wire.size(); ++n) {
    std::string_view in(std::string_view(wire).substr(0, n));
    CHECK(Throws([&] { DecodeBatch(in); }));
  }
  auto corrupt = [&](std::size_t offset, uint8_t byte) {
    std::string bad = wire;
    bad[offset] = static_cast<char>(byte);
    std::string_view in(bad);
    return Throws([&] { DecodeBatch(in); });
  };
  CHECK(corrupt(0, 99));    // version
  CHECK(corrupt(9, 7));     // mutation kind
  CHECK(corrupt(18 + 7, 0xff));  // old_rows column count: absurd
  CHECK(corrupt(18 + 8 + 4 * 7, 42));  // first column type
}

void TestMalformedStringOffsets() {
  ColumnarBatch batch;
  batch.row_count = 2;
  batch.new_rows.attnums = {0};
  batch.new_rows.columns.push_back(MakeColumn(AuxType::String, 2, false));
  std::get<StringColumnData>(batch.new_rows.columns[0].values).offsets[2] = 99;
  std::string wire;
  EncodeBatch(batch, wire);
  std::string_view in(wire);
  CHECK(Throws([&] { DecodeBatch(in); }));
}

void TestEncodeRejectsInconsistentBatch() {
  ColumnarBatch batch = MakeBatch(MutationKind::Insert, 4, false);
  batch.row_count = 5;
  std::string wire;
  bool threw = false;
  try {
    EncodeBatch(batch, wire);
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  CHECK(threw);
}

std::vector<AuxValue> SampleValues() {
  return {AuxValue::Of<bool>(AuxType::Boolean, true),
          AuxValue::Of<bool>(AuxType::Boolean, false),
          AuxValue::Of<int64_t>(AuxType::Int64, std::numeric_limits<int64_t>::min()),
          AuxValue::Of<uint64_t>(AuxType::UInt64, std::numeric_limits<uint64_t>::max()),
          AuxValue::Of<ScaledDecimal>(AuxType::ScaledDecimal, ScaledDecimal{-12345, 2}),
          AuxValue::Of<double>(AuxType::Double, -0.125),
          AuxValue::Of<AuxDate>(AuxType::Date, AuxDate{-10957}),
          AuxValue::Of<std::string>(AuxType::String, std::string("a\0b", 3)),
          AuxValue::Of<std::string>(AuxType::String, std::string()),
          AuxValue::Null(AuxType::Boolean),
          AuxValue::Null(AuxType::Int64),
          AuxValue::Null(AuxType::UInt64),
          AuxValue::Null(AuxType::ScaledDecimal),
          AuxValue::Null(AuxType::Double),
          AuxValue::Null(AuxType::Date),
          AuxValue::Null(AuxType::String)};
}

void TestArguments() {
  core::QueryArguments args;
  args.values = SampleValues();
  std::string wire;
  EncodeArguments(args, wire);
  std::string_view in(wire);
  CHECK(Same(DecodeArguments(in).values, args.values));
  CHECK(in.empty());

  std::string empty;
  EncodeArguments({}, empty);
  std::string_view empty_in(empty);
  CHECK(DecodeArguments(empty_in).values.empty());

  for (std::size_t n = 0; n < wire.size(); ++n) {
    std::string_view cut(std::string_view(wire).substr(0, n));
    CHECK(Throws([&] { DecodeArguments(cut); }));
  }
}

void TestRows() {
  std::vector<AuxRow> rows(3);
  rows[0].values = SampleValues();
  rows[2].values = {AuxValue::Of<int64_t>(AuxType::Int64, 7)};  // rows[1] is empty
  std::string wire;
  EncodeRows(rows, wire);
  std::string_view in(wire);
  auto decoded = DecodeRows(in);
  CHECK(in.empty());
  CHECK(decoded.size() == rows.size());
  for (std::size_t i = 0; i < rows.size(); ++i) {
    CHECK(Same(decoded[i].values, rows[i].values));
  }
  for (std::size_t n = 0; n < wire.size(); ++n) {
    std::string_view cut(std::string_view(wire).substr(0, n));
    CHECK(Throws([&] { DecodeRows(cut); }));
  }
  std::string bad = wire;
  bad[8 + 8] = 99;  // type of the first value
  std::string_view bad_in(bad);
  CHECK(Throws([&] { DecodeRows(bad_in); }));
}

} // namespace

int main() {
  const std::vector<std::pair<const char *, void (*)()>> tests = {
      {"batch round trip", TestBatchRoundTrip},
      {"every type alone", TestEveryTypeAlone},
      {"empty batches", TestEmptyBatches},
      {"several batches in one buffer", TestSeveralBatchesInOneBuffer},
      {"1e6-row batch", TestLargeBatch},
      {"malformed batch", TestMalformedBatch},
      {"malformed string offsets", TestMalformedStringOffsets},
      {"encode rejects inconsistent batch", TestEncodeRejectsInconsistentBatch},
      {"arguments", TestArguments},
      {"rows", TestRows},
  };
  int failed = 0;
  for (const auto &[name, fn] : tests) {
    try {
      fn();
      std::cout << "PASS " << name << "\n";
    } catch (const std::exception &e) {
      std::cout << "FAIL " << name << ": " << e.what() << "\n";
      ++failed;
    }
  }
  return failed == 0 ? 0 : 1;
}
