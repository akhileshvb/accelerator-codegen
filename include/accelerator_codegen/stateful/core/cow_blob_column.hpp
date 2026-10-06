#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace accelerator_codegen::aux_state::core {

// Copy-on-write column of variable-length byte strings indexed by a
// non-negative int64 key (the string counterpart of ChunkedCowArray).
//
// A chunk holds 2^kShift slots as two uint32 arrays (start, length) plus one
// append-only byte arena, so a stored string costs 8 B + its bytes and there
// is no per-string allocation. A slot that was never set is "absent"
// (Get returns false), which also distinguishes absent from the empty string.
//
//   * Copy-construct / assign: O(#directory entries) pointer copies; chunks are
//     shared between copies.
//   * Set / Erase clone ONLY the touched chunk when it is shared, otherwise they
//     append in place. Overwritten bytes become garbage that is compacted once it
//     exceeds half of the arena (and 64 KB), so repeated updates stay O(chunk).
//   * Bulk loads into an unshared column append in O(1) per string.
//   * Const reads never allocate; a returned view stays valid as long as the
//     column version it was read from is alive and unmodified.
// A chunk's arena is limited to 4 GB (Set throws std::length_error beyond that).
template <unsigned kShift = 12> class ChunkedCowBlobColumn {
public:
  static constexpr std::size_t kChunkSize = std::size_t{1} << kShift;
  static constexpr std::size_t kMask = kChunkSize - 1;

  static bool Fits(int64_t key) { return key >= 0; }

  bool Get(int64_t key, std::string_view &out) const {
    if (key < 0)
      return false;
    const auto id = static_cast<std::size_t>(key);
    const auto ci = id >> kShift;
    if (ci >= dir_.size() || !dir_[ci])
      return false;
    const Chunk &c = *dir_[ci];
    const auto slot = id & kMask;
    if (c.len[slot] == kAbsent)
      return false;
    out = std::string_view(c.bytes.data() + c.start[slot], c.len[slot]);
    return true;
  }
  bool Has(int64_t key) const {
    std::string_view unused;
    return Get(key, unused);
  }

  // Store `value` at key (key >= 0). Returns false for a negative key.
  bool Set(int64_t key, std::string_view value) {
    if (key < 0)
      return false;
    if (value.size() >= kAbsent)
      throw std::length_error("blob column value too large");
    Chunk &c = MutableChunk(key);
    const auto slot = static_cast<std::size_t>(key) & kMask;
    if (c.len[slot] != kAbsent)
      c.garbage += c.len[slot];
    if (c.bytes.size() + value.size() >= kAbsent)
      throw std::length_error("blob column chunk arena exceeds 4 GB");
    c.start[slot] = static_cast<uint32_t>(c.bytes.size());
    c.len[slot] = static_cast<uint32_t>(value.size());
    c.bytes.insert(c.bytes.end(), value.begin(), value.end());
    if (c.garbage > 65536 && c.garbage * 2 > c.bytes.size())
      Compact(c);
    return true;
  }
  // Mark the slot absent (no-op when it already is).
  void Erase(int64_t key) {
    if (!Has(key))
      return;
    Chunk &c = MutableChunk(key);
    const auto slot = static_cast<std::size_t>(key) & kMask;
    c.garbage += c.len[slot];
    c.len[slot] = kAbsent;
  }

  void Clear() { dir_.clear(); }
  std::size_t NumChunks() const { return dir_.size(); }
  std::size_t AllocatedChunks() const {
    std::size_t n = 0;
    for (const auto &c : dir_)
      n += c != nullptr;
    return n;
  }
  std::size_t MemoryBytes() const {
    std::size_t n = dir_.capacity() * sizeof(dir_[0]);
    for (const auto &c : dir_)
      if (c)
        n += c->bytes.capacity() + 2 * kChunkSize * sizeof(uint32_t);
    return n;
  }

private:
  static constexpr uint32_t kAbsent = 0xFFFFFFFFu;
  struct Chunk {
    std::vector<uint32_t> start = std::vector<uint32_t>(kChunkSize, 0);
    std::vector<uint32_t> len = std::vector<uint32_t>(kChunkSize, kAbsent);
    std::vector<char> bytes;
    std::size_t garbage = 0;
  };

  Chunk &MutableChunk(int64_t key) {
    const auto ci = static_cast<std::size_t>(key) >> kShift;
    if (dir_.size() <= ci)
      dir_.resize(ci + 1);
    auto &c = dir_[ci];
    if (!c)
      c = std::make_shared<Chunk>();
    else if (c.use_count() > 1)
      c = std::make_shared<Chunk>(*c);
    return *c;
  }
  static void Compact(Chunk &c) {
    std::vector<char> fresh;
    fresh.reserve(c.bytes.size() - c.garbage);
    for (std::size_t slot = 0; slot < kChunkSize; ++slot) {
      if (c.len[slot] == kAbsent)
        continue;
      const auto at = static_cast<uint32_t>(fresh.size());
      fresh.insert(fresh.end(), c.bytes.begin() + c.start[slot],
                   c.bytes.begin() + c.start[slot] + c.len[slot]);
      c.start[slot] = at;
    }
    c.bytes = std::move(fresh);
    c.garbage = 0;
  }

  std::vector<std::shared_ptr<Chunk>> dir_;
};

} // namespace accelerator_codegen::aux_state::core
