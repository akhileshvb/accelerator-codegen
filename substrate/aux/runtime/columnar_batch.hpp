// SCAFFOLDING
// C5: the captured change of one statement on one table, columnar and shared.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "substrate/aux/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::runtime {

// Variable-length bytes: row r is bytes[offsets[r], offsets[r + 1]).
// offsets.size() == row_count + 1. 64-bit offsets: no 4 GiB limit.
struct StringColumnData {
  std::vector<uint64_t> offsets;
  std::string bytes;
  std::string_view At(std::size_t row) const {
    return std::string_view(bytes).substr(offsets[row], offsets[row + 1] - offsets[row]);
  }
};

// One captured column. `type` decides which storage alternative is used:
//   Boolean -> vector<uint8_t>; Int64 -> vector<int64_t>; UInt64 -> vector<uint64_t>;
//   ScaledDecimal -> vector<int64_t> (unscaled, `scale` applies to the column);
//   ScaledDecimal128 -> vector<__int128> (likewise);
//   Double -> vector<double>; Date -> vector<int32_t> (days since 1970-01-01);
//   String -> StringColumnData.
struct ColumnData {
  core::AuxType type = core::AuxType::Int64;
  int32_t scale = 0;               // ScaledDecimal only
  std::vector<uint8_t> null_flags; // empty: no NULLs; else 1 = NULL, one per row
  std::variant<std::vector<uint8_t>, std::vector<int64_t>, std::vector<uint64_t>, std::vector<__int128>,
               std::vector<double>, std::vector<int32_t>, StringColumnData>
      values;

  bool IsNull(std::size_t row) const { return !null_flags.empty() && null_flags[row] != 0; }
  // The cell as a core::AuxValue (for row views).
  core::AuxValue Value(std::size_t row) const;
};

// A set of columns over the same rows. `attnums[i]` is the 0-based physical
// attribute number of the base table that columns[i] holds.
struct ColumnSet {
  std::vector<int32_t> attnums;
  std::vector<ColumnData> columns;
  // Position of `attnum` in columns, or -1.
  int Find(int32_t attnum) const;
};

// One statement's change on one table (an engine may split a statement into
// several batches; their order is preserved). Inserts fill `new_rows`, deletes
// `old_rows`, updates both (row i of old_rows and new_rows is the same row).
// The captured columns are the union of the columns any registered
// accelerator watches on the table (RuntimeHost::CaptureColumns).
struct ColumnarBatch {
  core::TableId table = 0;
  core::MutationKind kind = core::MutationKind::Insert;
  std::size_t row_count = 0;
  ColumnSet old_rows;
  ColumnSet new_rows;
  // Update only: attnums the statement assigned (empty: unknown, all).
  // kOutsideProjection: some column that was not captured.
  std::vector<int32_t> updated_attnums;
};

constexpr int32_t kOutsideProjection = -1;

using ColumnarBatchRef = std::shared_ptr<const ColumnarBatch>;

// The row view (core::MutationBatch) a module expects: rows in `projection` order
// (TableDependency::mutation_attnums; empty: every captured column in
// attnum order). `updated_columns` is mapped into projection positions; an
// updated column outside the projection becomes position projection.size(),
// so the module sees that none of its columns changed.
// Throws std::invalid_argument if a projected attnum was not captured.
core::MutationBatch ToMutationBatch(const ColumnarBatch &batch,
                                    const std::vector<int32_t> &projection);

// Builds a ColumnarBatch from a row-form MutationBatch whose rows hold
// `attnums` (for engines and tests that produce rows). Column types come
// from the first non-NULL value of each column (else the type its NULLs
// declare). An updated position outside `attnums` becomes kOutsideProjection.
ColumnarBatch FromMutationBatch(const core::MutationBatch &batch,
                                const std::vector<int32_t> &attnums);

// Bytes held by the batch's buffers (memory accounting).
std::size_t ByteSize(const ColumnarBatch &batch);

} // namespace accelerator_codegen::aux_state::runtime
