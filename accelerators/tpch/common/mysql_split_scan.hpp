// The serial and partitioned scans of one MySQL table, over one SplitTable
// and one row decoder; shared by the per-query plugin scan classes.
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "backends/mysql/api/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::mysql::split_scan {

// consumer(input) for every row, in the order of the whole-table statement.
template <class Input, class Decode>
void Serial(const detail::SplitTable &table, Decode decode,
            const std::function<void(const Input &)> &consumer) {
  detail::ScanTypedTuplesDecoded(detail::SelectSql(table), decode,
                                 [&](Input &&input) { consumer(input); });
}

// sink(partition, input) for every row; partitions run concurrently.
template <class Input, class Decode>
void Partitioned(const core::AuxEngineServices &services, const detail::SplitTable &table,
                 std::size_t partitions, Decode decode,
                 const std::function<void(std::size_t, const Input &)> &sink) {
  detail::ScanTypedTuplesPartitioned(
      services, table, partitions,
      [&](std::size_t partition, const std::vector<std::string> &cols) {
        sink(partition, decode(cols));
      });
}

} // namespace accelerator_codegen::aux_state::engine::mysql::split_scan
