// Partitioned typed table scan offered by the DuckDB InitialScan of a query,
// for parallel builds (core::PartitionedBuild, backends/duckdb/engine/build_scan.hpp).
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "backends/duckdb/engine/build_scan.hpp"
#include "substrate/aux/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::query {

namespace scan = ::accelerator_codegen::aux_state::engine::duckdb::scan;

// Adds to a counter of an index that parallel build partitions share: integer
// addition commutes, so the result is the serial one.
template <class T> void AtomicAdd(T &value, T delta) {
  __atomic_fetch_add(&value, delta, __ATOMIC_RELAXED);
}

class PartitionedTableScan {
public:
  using Sink = std::function<void(std::size_t, const scan::ChunkBatch &)>;

  virtual ~PartitionedTableScan() = default;

  // Streams `table` as typed batches (see scan::ScanPartitioned): sink(partition,
  // batch), one thread per partition, partitions concurrent.
  virtual void ScanTable(const core::AuxEngineServices &services, const std::string &table,
                         const std::vector<scan::BuildColumn> &columns,
                         std::size_t partitions, const Sink &sink) = 0;
};

} // namespace accelerator_codegen::aux_state::query
