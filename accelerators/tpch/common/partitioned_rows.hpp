// Helpers of the Postgres modules' PartitionedBuild builds.
#pragma once

#include <cstddef>
#include <iterator>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::query {

// Partitions of a build scan: one for a serial build, else enough to keep the
// workers busy past uneven partitions.
inline std::size_t BuildPartitions(const core::AuxEngineServices &services) {
  const std::size_t workers = services.WorkerCount();
  return workers <= 1 ? 1 : 4 * workers;
}

// One typed input row as the one-row batch PartitionedBuild's consume sees.
template <class Input> struct OneRowBatch {
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

} // namespace accelerator_codegen::aux_state::query
