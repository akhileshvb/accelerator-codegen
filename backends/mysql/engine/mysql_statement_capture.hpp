// GENERATED BACKEND — S1 capture: the trigger capture calls of one statement,
// decoded into typed ColumnarBatches.
#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <string_view>
#include <vector>

#include "substrate/aux/core/aux_types.hpp"
#include "substrate/aux/runtime/columnar_batch.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {

// One mysql_accel_capture_row call. Its arguments: table id, kind (0 insert,
// 1 delete, 2 update), one type character per column ('b' boolean, 'i' int64,
// 'u' uint64, 'f' double, 'n' exact decimal, 'd' date, 's' string), then the
// old row's values (delete, update) and the new row's (insert, update).
struct CapturedRow {
  struct Cell {
    const char *data = nullptr; // nullptr: SQL NULL
    std::size_t length = 0;
  };
  core::TableId table = 0;
  core::MutationKind kind = core::MutationKind::Insert;
  std::string_view types;
  std::vector<Cell> cells;
};

// Throws std::runtime_error if the call is malformed.
CapturedRow DecodeCapturedRow(std::size_t arg_count, char **args, const unsigned long *lengths);

// The rows a statement's triggers captured, grouped into batches: consecutive
// rows of one table and kind share a batch.
class StatementCapture {
public:
  // Adds `row`, keeping only the columns `watched` (attnums; empty: all).
  // Throws std::runtime_error if the row does not decode.
  void Add(const CapturedRow &row, const std::vector<int32_t> &watched);
  bool Empty() const { return closed.empty() && open.row_count == 0; }
  // The batches, in capture order; leaves the capture empty.
  std::vector<runtime::ColumnarBatchRef> Take();

private:
  void Close();

  runtime::ColumnarBatch open; // row_count == 0: none
  std::set<int32_t> changed;   // open update: attnums that differ between old and new
  std::vector<runtime::ColumnarBatchRef> closed;
};

} // namespace accelerator_codegen::aux_state::engine::mysql
