#pragma once

// Adapts a per-row partitioned scan callback to core::PartitionedBuild's batch
// (size() and Row(i)), so a module's InitialScan can hand out typed rows.

#include <cstddef>

namespace accelerator_codegen::aux_state::query {

template <class T> struct TypedRowBatch {
  const T &row;
  std::size_t size() const { return 1; }
  const T &Row(std::size_t) const { return row; }
};

} // namespace accelerator_codegen::aux_state::query
