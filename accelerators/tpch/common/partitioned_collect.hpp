// Rows of a partitioned build scan gathered as they are, in partition order.
#pragma once

#include <cstddef>
#include <iterator>
#include <utility>
#include <vector>

#include "substrate/aux/core/partitioned_build.hpp"
#include "tpch/common/partitioned_rows.hpp"

namespace accelerator_codegen::aux_state::query {

// scan(partitions, sink): the engine's partitioned scan, calling sink(partition, row).
template <class Input, class Scan>
std::vector<Input> CollectRows(const core::AuxEngineServices &services, std::size_t partitions,
                               Scan &&scan) {
  return core::PartitionedBuild<std::vector<Input>>(
      services, [] { return std::vector<Input>{}; },
      [&](std::size_t parts, auto &&sink) {
        scan(parts, [&](std::size_t partition, const Input &input) {
          sink(partition, OneRowBatch<Input>{input});
        });
      },
      [](std::vector<Input> &into, const Input &input) { into.push_back(input); },
      [](std::vector<Input> &into, std::vector<Input> &&from) {
        into.insert(into.end(), std::make_move_iterator(from.begin()),
                    std::make_move_iterator(from.end()));
      },
      partitions);
}

} // namespace accelerator_codegen::aux_state::query
