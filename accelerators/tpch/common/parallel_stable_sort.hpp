#pragma once

// Stable sort of a large vector on the build phase's threads: equal slices are
// sorted in parallel, then merged pairwise in rounds. The result is the same
// at any thread count.

#include <algorithm>
#include <cstddef>
#include <vector>

#include "substrate/aux/core/parallel_slices.hpp"

namespace accelerator_codegen::aux_state::query {

// Elements per sorted slice: below ~100 us a slice is not worth a pool task.
template <class T, class Less>
void ParallelStableSort(const core::AuxEngineServices &services, std::vector<T> &values,
                        Less less, std::size_t grain) {
  const std::size_t n = values.size();
  const std::size_t slices = core::SliceCount(services, n, grain);
  const auto bound = [&](std::size_t slice) { return n * slice / slices; };
  core::RunSlices(services, slices, n, [&](std::size_t, std::size_t begin, std::size_t end) {
    std::stable_sort(values.begin() + begin, values.begin() + end, less);
  });
  for (std::size_t width = 1; width < slices; width *= 2) {
    const std::size_t merges = (slices + 2 * width - 1) / (2 * width);
    services.ParallelFor(merges, [&](std::size_t m) {
      const std::size_t first = m * 2 * width;
      const std::size_t mid = std::min(first + width, slices);
      const std::size_t last = std::min(first + 2 * width, slices);
      if (mid < last)
        std::inplace_merge(values.begin() + bound(first), values.begin() + bound(mid),
                           values.begin() + bound(last), less);
    });
  }
}

} // namespace accelerator_codegen::aux_state::query
