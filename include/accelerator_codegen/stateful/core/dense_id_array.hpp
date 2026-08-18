#pragma once

#include <cstdint>
#include <vector>

namespace accelerator_codegen::aux_state::core {

// Chunked dense array indexed by a non-negative integer id (TPC-H keys).
// Missing keys read as `absent`. Chunks are 64K slots so the structure grows
// with max(id) rather than with a hash table.
template <typename T>
struct DenseIdArray {
  static constexpr std::size_t kShift = 16;
  static constexpr std::size_t kSize = 1u << kShift;
  static constexpr std::size_t kMask = kSize - 1;
  // 4096 chunks × 64K slots = 256M ids. Covers TPC-H SF20 orderkeys (~120M)
  // without letting a garbage key allocate terabytes.
  static constexpr std::size_t kMaxChunks = 4096;
  static constexpr int64_t kMaxId =
      static_cast<int64_t>(kMaxChunks) << kShift;

  std::vector<std::vector<T>> chunks;

  static bool Fits(int64_t key) { return key >= 0 && key < kMaxId; }

  void Clear() { chunks.clear(); }

  std::size_t SlotCount() const {
    std::size_t n = 0;
    for (const auto &chunk : chunks)
      n += chunk.size();
    return n;
  }

  bool Ensure(int64_t key, T fill = T{}) {
    if (!Fits(key))
      return false;
    const auto id = static_cast<std::size_t>(key);
    const auto chunk_idx = id >> kShift;
    const auto slot = id & kMask;
    if (chunks.size() <= chunk_idx)
      chunks.resize(chunk_idx + 1);
    auto &chunk = chunks[chunk_idx];
    if (chunk.size() <= slot)
      chunk.resize(slot + 1, fill);
    return true;
  }

  T Get(int64_t key, T absent = T{}) const {
    if (!Fits(key))
      return absent;
    const auto id = static_cast<std::size_t>(key);
    const auto chunk_idx = id >> kShift;
    if (chunk_idx >= chunks.size())
      return absent;
    const auto &chunk = chunks[chunk_idx];
    const auto slot = id & kMask;
    if (slot >= chunk.size())
      return absent;
    return chunk[slot];
  }

  bool Set(int64_t key, T value, T fill = T{}) {
    if (!Ensure(key, fill))
      return false;
    const auto id = static_cast<std::size_t>(key);
    chunks[id >> kShift][id & kMask] = value;
    return true;
  }
};

} // namespace accelerator_codegen::aux_state::core
