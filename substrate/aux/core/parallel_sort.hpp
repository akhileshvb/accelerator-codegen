// SCAFFOLDING
#pragma once

// Stable sort of a large vector on the build phase's threads: equal slices are
// sorted in parallel, then merged pairwise in rounds, each merge itself cut
// into pieces that run in parallel (so the last round, one merge of the whole
// vector, still uses every worker). The result is the same at any thread count.

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>
#include <type_traits>
#include <vector>

#include "substrate/aux/core/parallel_slices.hpp"

namespace accelerator_codegen::aux_state::core {

namespace sort_detail {

// Moves src[left_begin, left_end) and src[right_begin, right_end) to dst + out,
// merged (stable: left first on ties).
struct MergePiece {
  std::size_t left_begin, left_end, right_begin, right_end, out;
};

// Cuts the merge of the sorted runs [begin, mid) and [mid, end) into `pieces`
// independent merges. A piece starts at an element of the left run and at the
// first right element not less than it, so equal keys keep left-before-right.
template <class T, class Less>
void AppendMergePieces(const T *src, std::size_t begin, std::size_t mid, std::size_t end,
                       std::size_t pieces, Less &less, std::vector<MergePiece> &out) {
  pieces = std::clamp<std::size_t>(pieces, 1, std::max<std::size_t>(1, mid - begin));
  std::size_t left = begin, right = mid;
  for (std::size_t k = 1; k <= pieces; ++k) {
    const std::size_t next_left = begin + (mid - begin) * k / pieces;
    const std::size_t next_right =
        k == pieces ? end : std::lower_bound(src + mid, src + end, src[next_left], less) - src;
    out.push_back({left, next_left, right, next_right, begin + (left - begin) + (right - mid)});
    left = next_left;
    right = next_right;
  }
}

// One round: every run of `width` slices is merged with its right neighbour
// (an odd last run is carried over), src -> dst.
template <class T, class Less>
void MergeRound(const AuxEngineServices &services, const T *src, T *dst, std::size_t slices,
                std::size_t width, const std::vector<std::size_t> &bounds, Less &less) {
  const std::size_t merges = (slices + 2 * width - 1) / (2 * width);
  const std::size_t pieces = std::max<std::size_t>(1, services.WorkerCount() / merges);
  std::vector<MergePiece> tasks;
  for (std::size_t m = 0; m < merges; ++m) {
    const std::size_t first = m * 2 * width;
    const std::size_t mid = std::min(first + width, slices);
    const std::size_t last = std::min(first + 2 * width, slices);
    if (mid < last) {
      AppendMergePieces(src, bounds[first], bounds[mid], bounds[last], pieces, less, tasks);
    } else {
      tasks.push_back({bounds[first], bounds[last], bounds[last], bounds[last], bounds[first]});
    }
  }
  services.ParallelFor(tasks.size(), [&](std::size_t t) {
    const MergePiece &piece = tasks[t];
    std::merge(std::make_move_iterator(src + piece.left_begin), std::make_move_iterator(src + piece.left_end),
               std::make_move_iterator(src + piece.right_begin), std::make_move_iterator(src + piece.right_end),
               dst + piece.out, less);
  });
}

template <class T, class Less>
void InplaceMergeRounds(const AuxEngineServices &services, std::vector<T> &values, std::size_t slices,
                        const std::vector<std::size_t> &bounds, Less &less) {
  for (std::size_t width = 1; width < slices; width *= 2) {
    const std::size_t merges = (slices + 2 * width - 1) / (2 * width);
    services.ParallelFor(merges, [&](std::size_t m) {
      const std::size_t first = m * 2 * width;
      const std::size_t mid = std::min(first + width, slices);
      const std::size_t last = std::min(first + 2 * width, slices);
      if (mid < last)
        std::inplace_merge(values.begin() + bounds[first], values.begin() + bounds[mid],
                           values.begin() + bounds[last], less);
    });
  }
}

} // namespace sort_detail

// `grain`: elements per sorted slice; below ~100 us a slice is not worth a pool task.
template <class T, class Less>
void ParallelStableSort(const AuxEngineServices &services, std::vector<T> &values,
                        Less less, std::size_t grain) {
  const std::size_t n = values.size();
  const std::size_t slices = SliceCount(services, n, grain);
  std::vector<std::size_t> bounds(slices + 1);
  for (std::size_t s = 0; s <= slices; ++s) {
    bounds[s] = n * s / slices;
  }
  RunSlices(services, slices, n, [&](std::size_t, std::size_t begin, std::size_t end) {
    std::stable_sort(values.begin() + begin, values.begin() + end, less);
  });
  if constexpr (!std::is_default_constructible_v<T>) {
    sort_detail::InplaceMergeRounds(services, values, slices, bounds, less);
  } else if (slices > 1) {
    // Merge rounds ping-pong between the vector and a scratch array (default-
    // initialized, so a trivial T costs nothing until the rounds write it).
    const std::unique_ptr<T[]> scratch(new T[n]);
    T *from = values.data();
    T *to = scratch.get();
    for (std::size_t width = 1; width < slices; width *= 2) {
      sort_detail::MergeRound(services, from, to, slices, width, bounds, less);
      std::swap(from, to);
    }
    if (from != values.data()) {
      ParallelSlices(services, n, grain, [&](std::size_t, std::size_t begin, std::size_t end) {
        std::move(from + begin, from + end, values.data() + begin);
      });
    }
  }
}

} // namespace accelerator_codegen::aux_state::core
