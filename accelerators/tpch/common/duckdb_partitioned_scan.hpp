// Partitioned typed table scan offered by the DuckDB InitialScan of a query,
// for parallel builds (core::PartitionedBuild, backends/duckdb/engine/build_scan.hpp).
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "backends/duckdb/engine/build_scan.hpp"
#include "substrate/aux/core/aux_types.hpp"
#include "substrate/aux/core/partitioned_build.hpp"

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

// The rows of `table` decoded by `decode(row)` on the pool, in table order
// whatever the thread count: partitions are contiguous and merge in order.
template <class Row, class Decode>
std::vector<Row> CollectTable(const core::AuxEngineServices &services,
                              PartitionedTableScan &source, const std::string &table,
                              const std::vector<scan::BuildColumn> &columns, Decode &&decode) {
  return core::PartitionedBuild<std::vector<Row>>(
      services, [] { return std::vector<Row>{}; },
      [&](std::size_t partitions, auto &&sink) {
        source.ScanTable(services, table, columns, partitions, sink);
      },
      [&](std::vector<Row> &rows, const auto &row) { rows.push_back(decode(row)); },
      core::AppendRows<Row>, core::BuildPartitions(services));
}

} // namespace accelerator_codegen::aux_state::query
