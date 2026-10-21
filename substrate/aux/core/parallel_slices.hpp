// SCAFFOLDING
// Chunked fork-join on top of AuxEngineServices::ParallelFor, for the data-
// proportional loops of module reads (Execute), builds and maintenance.
//
// The range [0, n) is cut into contiguous slices of at least `grain` items, at
// most WorkerCount() of them, so a small range is one slice run inline and a
// serial run (WorkerCount() == 1) takes exactly the serial code path. Slices
// run on pool threads: they must not call services.StateStore() or
// CurrentVisibility() (reader visibility is per thread); read the snapshot
// first and let the slices borrow its images. In maintenance slices also never
// call the engine (do the lookups first) and write only state they own.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::core {

// Test-only: ACCEL_AUX_TEST_GRAIN=<g> caps every grain at g so tiny ranges split.
inline std::size_t TestGrainCap() {
  static const std::size_t cap = [] {
    const char *value = std::getenv("ACCEL_AUX_TEST_GRAIN");
    return value ? static_cast<std::size_t>(std::strtoull(value, nullptr, 10)) : std::size_t{0};
  }();
  return cap;
}

inline std::size_t SliceCount(const AuxEngineServices &services, std::size_t n,
                              std::size_t grain) {
  if (const std::size_t cap = TestGrainCap(); cap != 0) {
    grain = std::min(grain, cap);
  }
  return std::max<std::size_t>(1, std::min(services.WorkerCount(), n / std::max<std::size_t>(1, grain)));
}

// Runs fn(slice, begin, end) for each of `slices` even slices of [0, n).
template <class Fn>
void RunSlices(const AuxEngineServices &services, std::size_t slices, std::size_t n, Fn &&fn) {
  if (slices == 1) {
    fn(std::size_t{0}, std::size_t{0}, n);
    return;
  }
  services.ParallelFor(slices, [&](std::size_t slice) {
    fn(slice, n * slice / slices, n * (slice + 1) / slices);
  });
}

// RunSlices with SliceCount(n, grain) slices.
template <class Fn>
void ParallelSlices(const AuxEngineServices &services, std::size_t n, std::size_t grain,
                    Fn &&fn) {
  RunSlices(services, SliceCount(services, n, grain), n, std::forward<Fn>(fn));
}

// Runs fn(i) for each i in [0, n), in ParallelSlices slices of at least `grain`
// indices: for loops of independent items, or the parallel_for hooks of
// copy-on-write transactions.
template <class Fn>
void ParallelEach(const AuxEngineServices &services, std::size_t n, std::size_t grain, Fn &&fn) {
  ParallelSlices(services, n, grain, [&](std::size_t, std::size_t begin, std::size_t end) {
    for (std::size_t i = begin; i < end; ++i) {
      fn(i);
    }
  });
}

// ParallelEach over n heavy items (one contiguous run of items per worker) when
// `total_work` (in the caller's unit) reaches `work_threshold`, else inline on the
// calling thread. A `grain` counts items; this threshold counts the whole work.
template <class Fn>
void ParallelEachWhen(const AuxEngineServices &services, std::size_t n, std::size_t total_work,
                      std::size_t work_threshold, Fn &&fn) {
  ParallelEach(services, n, total_work < work_threshold ? std::max<std::size_t>(1, n) : 1,
               std::forward<Fn>(fn));
}

// Start offsets of the runs of equal key(items[i]) in `items`, then items.size()
// as the end sentinel (so run r is [starts[r], starts[r + 1]) and an empty input
// has no runs: starts == {0}).
template <class Items, class KeyFn>
std::vector<std::size_t> RunStarts(const Items &items, KeyFn &&key) {
  std::vector<std::size_t> starts;
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i == 0 || !(key(items[i]) == key(items[i - 1]))) {
      starts.push_back(i);
    }
  }
  starts.push_back(items.size());
  return starts;
}

// Folds parts[1..] into parts[0] pairwise over adjacent parts, one tree level
// at a time with each level's merges in parallel: merge(Part &into, Part &&from)
// always has `from` right after `into`, so an associative merge (also an
// order-keeping one, like appending) gives the serial chain's result, and the
// tree's shape depends on parts.size() only.
template <class Part, class Merge>
void MergeAdjacent(const AuxEngineServices &services, std::vector<Part> &parts, Merge &&merge) {
  for (std::size_t stride = 1; stride < parts.size(); stride *= 2) {
    const std::size_t pairs = (parts.size() - stride + 2 * stride - 1) / (2 * stride);
    services.ParallelFor(pairs, [&](std::size_t pair) {
      const std::size_t into = pair * 2 * stride;
      merge(parts[into], std::move(parts[into + stride]));
    });
  }
}

// Slice 0 accumulates straight into `acc`; the others into fresh Acc values
// that are merged pairwise (MergeAdjacent) and then into `acc`, so an
// associative merge gives the serial result at any thread count.
//   body(Acc &, begin, end)   merge(Acc &into, Acc &&from)
template <class Acc, class Body, class Merge>
void ParallelReduce(const AuxEngineServices &services, std::size_t n, std::size_t grain, Acc &acc,
                    Body &&body, Merge &&merge) {
  const std::size_t slices = SliceCount(services, n, grain);
  std::vector<Acc> partials(slices - 1);
  RunSlices(services, slices, n, [&](std::size_t slice, std::size_t begin, std::size_t end) {
    body(slice == 0 ? acc : partials[slice - 1], begin, end);
  });
  MergeAdjacent(services, partials, merge);
  if (!partials.empty()) {
    merge(acc, std::move(partials[0]));
  }
}

} // namespace accelerator_codegen::aux_state::core
