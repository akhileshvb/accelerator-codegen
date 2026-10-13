#pragma once
//
// CowFenwick<Cell>
// PURPOSE    Copy-on-write Fenwick (binary indexed) tree over the whole int32 range
//            with an arbitrary per-node Cell (struct of sums, small grid, ...).
// COMPLEXITY Update touches <= 32 nodes; VisitPrefix reads <= 32 nodes, independent of
//            data size.
// MEMORY     <= 32 nodes per populated position, each shared_ptr<Cell> (cell bytes +
//            ~16 B); only touched nodes exist.
// COW/THREADS Copy = shard pointer copies; Update clones touched nodes once per version.
//            Mutate a private copy only.
// USE WHEN   prefix/range aggregates over int32 positions (days, ids) of multi-field
//            cells. For a plain 64-ary count tree use RadixRangeTree.
//

#include <cstddef>
#include <cstdint>
#include <memory>

#include "accelerator_codegen/stateful/prims/flat_hash_map.hpp"

namespace accelerator_codegen::aux_state::core {

// Copy-on-write Fenwick (binary indexed) tree over int32 positions (days,
// ids, ...), with an arbitrary per-node `Cell` (a struct of sums, a small
// grid, ...). A prefix over [INT32_MIN, pos] visits at most 32 nodes whatever
// the number of populated positions, so range aggregates cost O(log) node
// reads independent of the data size.
//
//   * Positions are the whole int32 range; only nodes that were ever updated
//     exist (sparse), so state size is O(populated positions x 32) cells.
//   * Cell must be copy-constructible; a node is a shared_ptr<Cell>.
//   * Copying the tree copies shard pointers only. Update() clones one shard
//     (pointer table) and then each touched node at most once per copy
//     (nodes still shared with another version are cloned, private ones are
//     mutated in place), so a write batch costs O(touched nodes x cell size).
//   * Reads never allocate. Same threading rule as ShardedCowHashMap: mutate a
//     private copy only.
template <typename Cell> class CowFenwick {
public:
  static constexpr int64_t kDomain = int64_t{1} << 32; // indexes 1 .. 2^32

  // mutate(Cell &) for every node covering `pos`.
  template <typename F> void Update(int32_t pos, F &&mutate) {
    for (int64_t node = Index(pos); node <= kDomain; node += node & -node)
      mutate(MutableNode(node));
  }

  // visit(const Cell &) for the nodes that partition [INT32_MIN, pos]. `pos`
  // is any int64: below the int32 range the prefix is empty, above it is all.
  template <typename F> void VisitPrefix(int64_t pos, F &&visit) const {
    if (pos < INT32_MIN)
      return;
    if (pos > INT32_MAX)
      pos = INT32_MAX;
    for (int64_t node = Index(static_cast<int32_t>(pos)); node > 0;
         node -= node & -node) {
      const auto *cell = nodes_.Find(node);
      if (cell && *cell)
        visit(**cell);
    }
  }

  std::size_t MemoryBytes(std::size_t cell_bytes) const {
    return nodes_.memory_bytes() + nodes_.size() * (cell_bytes + 16);
  }
  std::size_t NodeCount() const { return nodes_.size(); }

private:
  static int64_t Index(int32_t pos) {
    return static_cast<int64_t>(pos) - static_cast<int64_t>(INT32_MIN) + 1;
  }
  Cell &MutableNode(int64_t node) {
    auto &slot = nodes_[node];
    if (!slot)
      slot = std::make_shared<Cell>();
    else if (slot.use_count() > 1)
      slot = std::make_shared<Cell>(*slot);
    return *slot;
  }

  ShardedCowHashMap<std::shared_ptr<Cell>> nodes_;
};

} // namespace accelerator_codegen::aux_state::core
