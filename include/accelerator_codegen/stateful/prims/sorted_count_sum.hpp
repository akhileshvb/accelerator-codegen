#pragma once
//
// SortedCountSum
// PURPOSE    Copy-on-write ordered multiset of int64 keys answering exactly 'count and
//            sum of keys strictly above x'.
// COMPLEXITY Add O(log #blocks + block) after the first touch; SuffixAbove O(log
//            #blocks + block + #blocks).
// MEMORY     16 B per distinct key (int64 key + int64 multiplicity) plus a 24 B summary per block.
// COW/THREADS Copy = O(#blocks) pointers; mutate a private copy only.
// USE WHEN   you need rank/suffix aggregates of a multiset of numbers. For prefix sums
//            over a bounded integer domain use RadixRangeTree; over int32 positions
//            with per-node structs use CowFenwick.
//

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace accelerator_codegen::aux_state::core {

// Copy-on-write ordered multiset of int64 keys, answering "how many keys (and
// what is their sum) lie strictly above x" exactly.
//
// Layout: a directory of blocks, each block holding up to kMaxBlock sorted
// distinct keys with their multiplicities, plus a per-block (count, sum)
// summary kept in the directory.
//
//   * Copy: O(#blocks) pointer copies; blocks are shared between copies.
//   * Add(key, n): clones only the touched block when it is shared (O(block)),
//     splits it when it grows past kMaxBlock, drops it when it empties.
//     Costs O(log #blocks + block) after the first touch in a copy.
//   * SuffixAbove(x): O(log #blocks) to find the block, one partial block scan
//     and a sum of the summaries of the blocks after it (O(#blocks), ~n/512
//     adds; distinct keys are bounded by the key domain, not the row count).
//   * Counts and sums are exact (int64 counts, int128 sums).
// Same threading rule as ChunkedCowArray: concurrent const reads of a published
// copy are fine; mutate only a private copy.
class SortedCountSum {
public:
  using Int128 = __int128;
  static constexpr std::size_t kMaxBlock = 1024;

  struct Totals {
    int64_t count = 0;
    Int128 sum = 0;
  };

  // Adds n (possibly negative) copies of `key`. Returns false, changing
  // nothing, if that would make the multiplicity of `key` negative.
  bool Add(int64_t key, int64_t n) {
    if (n == 0)
      return true;
    if (dir_.empty()) {
      if (n < 0)
        return false;
      auto block = std::make_shared<Block>();
      block->keys.push_back(key);
      block->counts.push_back(n);
      dir_.push_back(MakeRef(std::move(block)));
      return true;
    }
    std::size_t idx = BlockFor(key);
    auto &block_ptr = dir_[idx].block;
    auto &keys = block_ptr->keys;
    auto pos = std::lower_bound(keys.begin(), keys.end(), key);
    const bool found = pos != keys.end() && *pos == key;
    const std::size_t at = static_cast<std::size_t>(pos - keys.begin());
    if (found ? block_ptr->counts[at] + n < 0 : n < 0)
      return false;
    if (block_ptr.use_count() > 1)
      block_ptr = std::make_shared<Block>(*block_ptr);
    Block &block = *block_ptr;
    if (found) {
      block.counts[at] += n;
      if (block.counts[at] == 0) {
        block.keys.erase(block.keys.begin() + at);
        block.counts.erase(block.counts.begin() + at);
      }
    } else {
      block.keys.insert(block.keys.begin() + at, key);
      block.counts.insert(block.counts.begin() + at, n);
    }
    dir_[idx].count += n;
    dir_[idx].sum += static_cast<Int128>(key) * n;
    if (block.keys.empty()) {
      dir_.erase(dir_.begin() + idx);
    } else if (block.keys.size() > kMaxBlock) {
      Split(idx);
    } else {
      dir_[idx].min_key = block.keys.front();
    }
    return true;
  }

  // Count and sum of the keys strictly greater than `key`.
  Totals SuffixAbove(int64_t key) const {
    Totals out;
    // First block whose smallest key is above `key`: fully included.
    auto first_above = std::upper_bound(
        dir_.begin(), dir_.end(), key,
        [](int64_t k, const Ref &r) { return k < r.min_key; });
    for (auto it = first_above; it != dir_.end(); ++it) {
      out.count += it->count;
      out.sum += it->sum;
    }
    if (first_above != dir_.begin()) {
      const Block &block = *(first_above - 1)->block;
      auto from = std::upper_bound(block.keys.begin(), block.keys.end(), key);
      for (std::size_t i = static_cast<std::size_t>(from - block.keys.begin());
           i < block.keys.size(); ++i) {
        out.count += block.counts[i];
        out.sum += static_cast<Int128>(block.keys[i]) * block.counts[i];
      }
    }
    return out;
  }

  Totals Total() const {
    Totals out;
    for (const auto &r : dir_) {
      out.count += r.count;
      out.sum += r.sum;
    }
    return out;
  }

  std::size_t NumBlocks() const { return dir_.size(); }
  std::size_t MemoryBytes() const {
    std::size_t n = dir_.capacity() * sizeof(Ref);
    for (const auto &r : dir_)
      n += r.block->keys.capacity() * 2 * sizeof(int64_t);
    return n;
  }

private:
  struct Block {
    std::vector<int64_t> keys;   // sorted, distinct
    std::vector<int64_t> counts; // multiplicity of keys[i], > 0
  };
  struct Ref {
    int64_t min_key = 0;
    int64_t count = 0;
    Int128 sum = 0;
    std::shared_ptr<Block> block;
  };

  static Ref MakeRef(std::shared_ptr<Block> block) {
    Ref r;
    r.min_key = block->keys.front();
    for (std::size_t i = 0; i < block->keys.size(); ++i) {
      r.count += block->counts[i];
      r.sum += static_cast<Int128>(block->keys[i]) * block->counts[i];
    }
    r.block = std::move(block);
    return r;
  }

  // The block that holds (or would hold) `key`.
  std::size_t BlockFor(int64_t key) const {
    auto it = std::upper_bound(
        dir_.begin(), dir_.end(), key,
        [](int64_t k, const Ref &r) { return k < r.min_key; });
    return it == dir_.begin() ? 0 : static_cast<std::size_t>(it - dir_.begin()) - 1;
  }

  void Split(std::size_t idx) {
    Block &block = *dir_[idx].block;
    const std::size_t half = block.keys.size() / 2;
    auto upper = std::make_shared<Block>();
    upper->keys.assign(block.keys.begin() + half, block.keys.end());
    upper->counts.assign(block.counts.begin() + half, block.counts.end());
    block.keys.resize(half);
    block.counts.resize(half);
    Ref low = MakeRef(dir_[idx].block);
    Ref high = MakeRef(std::move(upper));
    dir_[idx] = std::move(low);
    dir_.insert(dir_.begin() + idx + 1, std::move(high));
  }

  std::vector<Ref> dir_;
};

} // namespace accelerator_codegen::aux_state::core
