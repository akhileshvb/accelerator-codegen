// SCAFFOLDING
// Parallel build: the engine's partitioned scan fills one partial per
// partition on pool threads, and the partials are merged in partition order.
//
// The engine's scan (`scan(partitions, sink)`, e.g. the DuckDB or Postgres
// ScanPartitioned bound to a table and columns) guarantees:
//  - the rows of partition p are a fixed set (fixed row-group / block range),
//    delivered to sink(p, batch) in a fixed order;
//  - sink calls of one partition never overlap; different partitions run
//    concurrently on pool threads, so `consume` must touch only its partial
//    (and no engine API);
//  - with one partition it is the engine's serial scan on the calling thread.
// A batch offers size() and Row(i), a view with typed accessors (Int, Date,
// Decimal, Text, Char). Results are identical at any partition count when
// `merge` is associative (integers, fixed-point sums, maps of those). A partial
// that holds a floating-point sum passes kFloatBuildPartitions (a function of
// the data, not of the thread count): the result is then bit-identical at any
// build thread count, threads only decide how many partitions run at once.
#pragma once

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::core {

inline constexpr std::size_t kFloatBuildPartitions = 16;

// Buckets rows are dealt into by builds that merge bucket by bucket.
inline constexpr std::size_t kBuildBuckets = 64;

// Partitions of a build scan: one for a serial build, else enough to keep the
// workers busy past uneven partitions.
inline std::size_t BuildPartitions(const AuxEngineServices &services) {
  const std::size_t workers = services.WorkerCount();
  return workers <= 1 ? 1 : 4 * workers;
}

// One typed row as the one-row batch a partitioned scan hands to the sink.
template <class Input> struct SingleRowBatch {
  const Input &input;
  std::size_t size() const { return 1; }
  const Input &Row(std::size_t) const { return input; }
};

// Rows dealt out to a fixed number of buckets; merging appends bucket by bucket.
template <class Row> using Buckets = std::vector<std::vector<Row>>;

template <class Row> void AppendBuckets(Buckets<Row> &into, Buckets<Row> &&from) {
  for (std::size_t b = 0; b < into.size(); ++b) {
    into[b].insert(into[b].end(), std::make_move_iterator(from[b].begin()),
                   std::make_move_iterator(from[b].end()));
    from[b] = {};
  }
}

// Partitions = `partitions`, by default services.WorkerCount() (the build cap).
//   make()                          a new empty Partial
//   scan(partitions, sink)          the engine's partitioned scan
//   consume(Partial &, row)         adds one row
//   merge(Partial &into, Partial &&from)
template <class Partial, class Make, class Scan, class Consume, class Merge>
Partial PartitionedBuild(const AuxEngineServices &services, Make &&make, Scan &&scan,
                         Consume &&consume, Merge &&merge, std::size_t partitions = 0) {
  partitions = std::max<std::size_t>(1, partitions ? partitions : services.WorkerCount());
  std::vector<Partial> partials;
  partials.reserve(partitions);
  for (std::size_t p = 0; p < partitions; ++p) {
    partials.push_back(make());
  }
  scan(partitions, [&](std::size_t partition, const auto &batch) {
    Partial &partial = partials[partition];
    for (std::size_t row = 0; row < batch.size(); ++row) {
      consume(partial, batch.Row(row));
    }
  });
  for (std::size_t p = 1; p < partitions; ++p) {
    merge(partials[0], std::move(partials[p]));
  }
  return std::move(partials[0]);
}

// Partial merge for std::vector partials: appends in partition order.
template <class Row> void AppendRows(std::vector<Row> &into, std::vector<Row> &&from) {
  into.insert(into.end(), std::make_move_iterator(from.begin()),
              std::make_move_iterator(from.end()));
  from = {};
}

// All rows of a partitioned scan in partition order: the scan decodes them on
// pool threads, the caller applies them to the state serially.
template <class Input, class Scan>
std::vector<Input> CollectRows(const AuxEngineServices &services, Scan &&scan) {
  using Rows = std::vector<Input>;
  return PartitionedBuild<Rows>(
      services, [] { return Rows{}; },
      [&](std::size_t partitions, auto &&sink) {
        scan(partitions, [&](std::size_t partition, const Input &input) {
          sink(partition, SingleRowBatch<Input>{input});
        });
      },
      [](Rows &into, const Input &input) { into.push_back(input); }, AppendRows<Input>,
      BuildPartitions(services));
}

} // namespace accelerator_codegen::aux_state::core
