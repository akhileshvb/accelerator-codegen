#pragma once
//
// StringBlob / StringDict
// PURPOSE    Append-only string storage (one byte blob + uint64 offsets) and an
//            interning dictionary with stable ids and lexicographic ranks.
// COMPLEXITY Append amortised O(len); View O(1); StringDict Find/Intern O(len)
//            expected.
// MEMORY     StringBlob: 8 B + bytes per string. StringDict: + 4 B hash + ~8 B table
//            slot per distinct string.
// COW/THREADS Single writer; views are invalidated by Append. Not COW: copying
//            duplicates the bytes.
// USE WHEN   dictionary-encoding low-cardinality strings, or holding a build-time
//            string column. For versioned (COW) strings use ChunkedBlobMap or
//            KeyedStringColumn.
//

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace accelerator_codegen::aux_state::core {

// Append-only string storage: one contiguous byte blob plus uint64 offsets
// (n+1 entries, offsets()[0] == 0). 8 B/string + bytes; no per-string
// allocation. uint64 everywhere, so blobs > 4 GB work. Ids are append order and
// stable. The blob/offsets vectors double when they grow (transient 2x peak):
// call Reserve(n_strings, n_bytes) up front for large loads. Views returned by
// View() are invalidated by Append. data()/offsets() feed
// sql_like::MatchBlobBitmap directly.
class StringBlob {
public:
  StringBlob() : off_(1, 0) {}
  void Reserve(std::size_t n_strings, std::size_t n_bytes) {
    off_.reserve(n_strings + 1);
    data_.reserve(n_bytes);
  }
  // Returns the new string's index.
  uint64_t Append(std::string_view s) {
    data_.insert(data_.end(), s.begin(), s.end());
    off_.push_back(data_.size());
    return off_.size() - 2;
  }
  std::size_t size() const { return off_.size() - 1; }
  std::size_t bytes() const { return data_.size(); }
  std::string_view View(uint64_t i) const {
    return {data_.data() + off_[i], static_cast<std::size_t>(off_[i + 1] - off_[i])};
  }
  uint64_t Length(uint64_t i) const { return off_[i + 1] - off_[i]; }
  const char *data() const { return data_.data(); }
  const uint64_t *offsets() const { return off_.data(); }
  std::size_t MemoryBytes() const { return data_.capacity() + off_.capacity() * 8; }
  void Clear() {
    data_.clear();
    off_.assign(1, 0);
  }

private:
  std::vector<char> data_;
  std::vector<uint64_t> off_;
};

// Dictionary encoding: string -> dense uint32 id (insertion order, stable, never
// reused; no erase). Backed by a StringBlob plus an open-addressing table of
// ids (4 B/slot, load <= 1/2) and a cached 32-bit hash per id for cheap rehash.
// Up to 2^32-2 distinct strings. Ids are NOT value-ordered; SortedRanks()
// computes rank-of-id (rank order == byte-wise lexicographic order) on demand in
// O(n log n) -- recompute after new Intern()s.
class StringDict {
public:
  static constexpr uint32_t kNone = 0xFFFFFFFFu;

  uint32_t Find(std::string_view s) const {
    if (table_.empty())
      return kNone;
    const uint64_t h = Hash(s);
    const std::size_t mask = table_.size() - 1;
    for (std::size_t i = static_cast<std::size_t>(h) & mask;; i = (i + 1) & mask) {
      const uint32_t id = table_[i];
      if (id == kNone)
        return kNone;
      if (hashes_[id] == static_cast<uint32_t>(h >> 32) && blob_.View(id) == s)
        return id;
    }
  }
  // Existing id, or a newly assigned one.
  uint32_t Intern(std::string_view s) {
    const uint32_t f = Find(s);
    if (f != kNone)
      return f;
    if (blob_.size() >= kNone) // ids are 32-bit; kNone is reserved
      throw std::length_error("StringDict: more than 2^32 - 1 distinct strings");
    if ((blob_.size() + 1) * 2 > table_.size())
      Grow();
    const uint64_t h = Hash(s);
    const auto id = static_cast<uint32_t>(blob_.Append(s));
    hashes_.push_back(static_cast<uint32_t>(h >> 32));
    Place(id, h);
    return id;
  }
  std::string_view Get(uint32_t id) const { return blob_.View(id); }
  std::size_t size() const { return blob_.size(); }
  const StringBlob &blob() const { return blob_; }
  std::size_t MemoryBytes() const {
    return blob_.MemoryBytes() + table_.capacity() * 4 + hashes_.capacity() * 4;
  }
  // rank[id] = position of the string among all distinct strings in byte order.
  std::vector<uint32_t> SortedRanks() const {
    std::vector<uint32_t> order(size());
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(),
              [&](uint32_t a, uint32_t b) { return blob_.View(a) < blob_.View(b); });
    std::vector<uint32_t> rank(size());
    for (std::size_t r = 0; r < order.size(); ++r)
      rank[order[r]] = static_cast<uint32_t>(r);
    return rank;
  }

private:
  static uint64_t Hash(std::string_view s) {
    uint64_t h = 0x9E3779B97F4A7C15ULL ^ s.size();
    std::size_t i = 0;
    for (; i + 8 <= s.size(); i += 8) {
      uint64_t w;
      std::memcpy(&w, s.data() + i, 8);
      h = (h ^ w) * 0x9FB21C651E98DF25ULL;
      h ^= h >> 29;
    }
    uint64_t w = 0;
    std::memcpy(&w, s.data() + i, s.size() - i);
    h = (h ^ w) * 0x9FB21C651E98DF25ULL;
    h ^= h >> 32;
    h *= 0xD6E8FEB86659FD93ULL;
    h ^= h >> 32;
    return h;
  }
  void Place(uint32_t id, uint64_t h) {
    const std::size_t mask = table_.size() - 1;
    std::size_t i = static_cast<std::size_t>(h) & mask;
    while (table_[i] != kNone)
      i = (i + 1) & mask;
    table_[i] = id;
  }
  void Grow() {
    const std::size_t cap = table_.empty() ? 16 : table_.size() * 2;
    table_.assign(cap, kNone);
    // Rehash needs the low hash bits again: recompute from the string (the
    // cached 32 bits are the high half, used only as a match filter).
    for (uint32_t id = 0; id < blob_.size(); ++id)
      Place(id, Hash(blob_.View(id)));
  }
  StringBlob blob_;
  std::vector<uint32_t> table_;
  std::vector<uint32_t> hashes_;
};

} // namespace accelerator_codegen::aux_state::core
