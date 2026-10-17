// SCAFFOLDING
// Small helpers shared by query modules.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "substrate/aux/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::core {

inline std::size_t MutationRowCount(const MutationBatch &mutation) {
  switch (mutation.kind) {
  case MutationKind::Insert:
    return mutation.new_rows.size();
  case MutationKind::Delete:
    return mutation.old_rows.size();
  case MutationKind::Update:
    return std::max(mutation.old_rows.size(), mutation.new_rows.size());
  }
  return 0;
}

// Smallest power of two >= n (1 for 0).
inline uint32_t NextPowerOf2(uint32_t n) {
  if (n == 0) {
    return 1;
  }
  --n;
  n |= n >> 1;
  n |= n >> 2;
  n |= n >> 4;
  n |= n >> 8;
  n |= n >> 16;
  return n + 1;
}

// For each requested partition the matching image of `found` (or nullptr).
// Stores return found partitions in request order, but the interface does not
// promise it: the in-order walk is the fast path, anything else is matched by
// sorted lookup.
inline void MatchSnapshot(const std::vector<PartitionId> &requested,
                          const std::vector<StateImage> &found,
                          std::vector<const StateImage *> &out) {
  out.assign(requested.size(), nullptr);
  std::size_t at = 0;
  for (std::size_t slot = 0; slot < requested.size(); ++slot) {
    if (at < found.size() && found[at].partition == requested[slot]) {
      out[slot] = &found[at++];
    }
  }
  if (at == found.size()) {
    return;
  }
  std::vector<const StateImage *> sorted;
  sorted.reserve(found.size());
  for (const auto &image : found) {
    sorted.push_back(&image);
  }
  std::sort(sorted.begin(), sorted.end(),
            [](const StateImage *l, const StateImage *r) { return l->partition < r->partition; });
  for (std::size_t slot = 0; slot < requested.size(); ++slot) {
    const auto it = std::lower_bound(
        sorted.begin(), sorted.end(), requested[slot],
        [](const StateImage *image, PartitionId id) { return image->partition < id; });
    out[slot] = it != sorted.end() && (*it)->partition == requested[slot] ? *it : nullptr;
  }
}

} // namespace accelerator_codegen::aux_state::core
