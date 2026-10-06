#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace accelerator_codegen::aux_state::core {

// Ordered set of fixed-width keys kept as a directory of sorted blocks, with
// copy-on-write sharing (an order-statistics-free B+tree with two levels).
//
//   * Keys are unique under the strict weak order `Less` (an insert of an
//     equivalent key is a no-op; erase removes it).
//   * Blocks hold <= kMaxBlock keys (16 B keys: 64 KB blocks at the default
//     4096); a freshly bulk-built set fills blocks to 3/4, so the footprint is
//     ~1.35 x sizeof(Key) per key, with no per-key node or pointer.
//   * Copy-construct / assign copies only the directory (one shared_ptr and one
//     key per block, i.e. n / 3000 entries); blocks are shared between copies.
//   * Insert / Erase are O(log #blocks + block size): they clone only the one
//     touched block when it is shared (use_count() > 1) and write it in place
//     otherwise, so a write batch pays one block copy per touched block, not
//     O(n). Full blocks split, nearly empty neighbours merge.
//   * ForEach walks the keys in order and stops when the callback returns false:
//     the top-k / "everything above a threshold" scan costs O(output).
//   * Versions are independent (mutating one never changes what another sees).
//     Same threading rule as ChunkedCowArray: mutate only a private copy.
template <typename Key, typename Less, std::size_t kMaxBlock = 4096>
class CowSortedBlocks {
public:
  CowSortedBlocks() = default;

  // Bulk build from keys already sorted under Less (unique). Consumes `keys`
  // block by block (the vector is freed as it is drained to bound the peak).
  static CowSortedBlocks FromSorted(std::vector<Key> &&keys) {
    CowSortedBlocks out;
    const std::size_t fill = kMaxBlock * 3 / 4;
    out.size_ = keys.size();
    for (std::size_t at = 0; at < keys.size(); at += fill) {
      const std::size_t end = std::min(keys.size(), at + fill);
      auto block = std::make_shared<Block>();
      block->keys.assign(std::make_move_iterator(keys.begin() + at),
                         std::make_move_iterator(keys.begin() + end));
      out.firsts_.push_back(block->keys.front());
      out.blocks_.push_back(std::move(block));
    }
    std::vector<Key>().swap(keys);
    return out;
  }

  std::size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  std::size_t NumBlocks() const { return blocks_.size(); }

  bool Contains(const Key &key) const {
    if (blocks_.empty())
      return false;
    const auto &keys = blocks_[BlockFor(key)]->keys;
    const auto it = std::lower_bound(keys.begin(), keys.end(), key, Less());
    return it != keys.end() && !Less()(key, *it);
  }

  void Insert(const Key &key) {
    if (blocks_.empty()) {
      auto block = std::make_shared<Block>();
      block->keys.push_back(key);
      firsts_.push_back(key);
      blocks_.push_back(std::move(block));
      size_ = 1;
      return;
    }
    const std::size_t bi = BlockFor(key);
    Block &block = Own(bi);
    const auto it = std::lower_bound(block.keys.begin(), block.keys.end(), key, Less());
    if (it != block.keys.end() && !Less()(key, *it))
      return; // already present
    block.keys.insert(it, key);
    ++size_;
    firsts_[bi] = block.keys.front();
    if (block.keys.size() > kMaxBlock) {
      auto upper = std::make_shared<Block>();
      const std::size_t half = block.keys.size() / 2;
      upper->keys.assign(block.keys.begin() + half, block.keys.end());
      block.keys.resize(half);
      firsts_.insert(firsts_.begin() + bi + 1, upper->keys.front());
      blocks_.insert(blocks_.begin() + bi + 1, std::move(upper));
    }
  }

  bool Erase(const Key &key) {
    if (blocks_.empty())
      return false;
    const std::size_t bi = BlockFor(key);
    {
      const auto &keys = blocks_[bi]->keys;
      const auto it = std::lower_bound(keys.begin(), keys.end(), key, Less());
      if (it == keys.end() || Less()(key, *it))
        return false;
    }
    Block &block = Own(bi);
    block.keys.erase(std::lower_bound(block.keys.begin(), block.keys.end(), key, Less()));
    --size_;
    if (block.keys.empty()) {
      blocks_.erase(blocks_.begin() + bi);
      firsts_.erase(firsts_.begin() + bi);
      return true;
    }
    firsts_[bi] = block.keys.front();
    // Merge a small block into its right neighbour when both fit in 3/4 block.
    if (block.keys.size() < kMaxBlock / 4 && bi + 1 < blocks_.size() &&
        block.keys.size() + blocks_[bi + 1]->keys.size() <= kMaxBlock * 3 / 4) {
      const auto &next = blocks_[bi + 1]->keys; // read-only: no clone needed
      block.keys.insert(block.keys.end(), next.begin(), next.end());
      blocks_.erase(blocks_.begin() + bi + 1);
      firsts_.erase(firsts_.begin() + bi + 1);
    }
    return true;
  }

  // fn(const Key&) -> bool (false stops the walk), in ascending Less order.
  template <typename F> void ForEach(F &&fn) const {
    for (const auto &block : blocks_)
      for (const Key &key : block->keys)
        if (!fn(key))
          return;
  }

  std::size_t MemoryBytes() const {
    std::size_t n = blocks_.capacity() * sizeof(blocks_[0]) +
                    firsts_.capacity() * sizeof(Key);
    for (const auto &block : blocks_)
      n += block->keys.capacity() * sizeof(Key);
    return n;
  }
  // Blocks whose payload is shared with another copy.
  std::size_t SharedBlocks() const {
    std::size_t n = 0;
    for (const auto &block : blocks_)
      n += block.use_count() > 1;
    return n;
  }

private:
  struct Block {
    std::vector<Key> keys;
  };

  // Index of the block that holds (or would hold) `key`: the last block whose
  // first key is not greater than it (block 0 for keys below everything).
  std::size_t BlockFor(const Key &key) const {
    const auto it = std::upper_bound(firsts_.begin(), firsts_.end(), key, Less());
    return it == firsts_.begin() ? 0 : static_cast<std::size_t>(it - firsts_.begin()) - 1;
  }
  Block &Own(std::size_t bi) {
    auto &block = blocks_[bi];
    if (block.use_count() > 1)
      block = std::make_shared<Block>(*block);
    return *block;
  }

  std::vector<std::shared_ptr<Block>> blocks_;
  std::vector<Key> firsts_; // firsts_[i] == blocks_[i]->keys.front()
  std::size_t size_ = 0;
};

} // namespace accelerator_codegen::aux_state::core
