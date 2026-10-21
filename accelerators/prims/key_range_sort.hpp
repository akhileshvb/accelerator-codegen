#pragma once
//
// PartitionPieces<Row>, SortIntoKeyBuckets, PieceOffsets, string key helpers
// PURPOSE    Sorts the rows of a partitioned scan on the pool: a sample of the keys cuts
//            them into key-range buckets (one counting scatter per scan piece), then each
//            bucket is stably sorted on its own. The sorted rows are the concatenation
//            of the buckets, so the caller can fill its arrays bucket by bucket.
// COMPLEXITY O(rows log(rows / buckets)) work, no serial merge of sorted runs.
// MEMORY     A second copy of the rows while scattered; the pieces are emptied.
// THREADS    One piece (scatter) or bucket (sort) per task through `Parallelism`.
//            Rows with equal keys keep scan order; the result does not depend on the
//            thread count or on how many pieces the scan produced.
// USE WHEN   a build needs one globally sorted sequence of the scanned rows.
//

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string_view>
#include <utility>
#include <vector>

#include "prims/parallelism.hpp"

namespace accelerator_codegen::aux_state::core {

inline constexpr std::size_t kSortBuckets = 4096;
inline constexpr std::size_t kSamplesPerPiece = 64;

template <class Row> using PieceList = std::vector<std::vector<Row>>;

// The rows of one scan partition; merging links the pieces in partition order.
template <class Row> struct PartitionPieces {
  std::vector<Row> rows;
  PieceList<Row> merged;

  void Add(Row row) { rows.push_back(std::move(row)); }
  void Absorb(PartitionPieces &&from) {
    merged.push_back(std::move(from.rows));
    for (auto &piece : from.merged)
      merged.push_back(std::move(piece));
  }
  PieceList<Row> Take() && {
    PieceList<Row> all;
    all.push_back(std::move(rows));
    for (auto &piece : merged)
      all.push_back(std::move(piece));
    return all;
  }
};

// key(row) -> uint64 must be monotone in the order `less` sorts by.
template <class Row, class Key, class Less>
PieceList<Row> SortIntoKeyBuckets(const Parallelism &parallelism, PieceList<Row> pieces,
                                  Key key, Less less) {
  std::vector<uint64_t> sample;
  for (const auto &piece : pieces) {
    const std::size_t stride = std::max<std::size_t>(1, piece.size() / kSamplesPerPiece);
    for (std::size_t i = 0; i < piece.size(); i += stride)
      sample.push_back(key(piece[i]));
  }
  std::sort(sample.begin(), sample.end());
  std::vector<uint64_t> splitters;
  for (std::size_t b = 1; b < kSortBuckets && !sample.empty(); ++b)
    splitters.push_back(sample[b * sample.size() / kSortBuckets]);
  splitters.erase(std::unique(splitters.begin(), splitters.end()), splitters.end());
  const std::size_t buckets = splitters.size() + 1;

  struct Scattered {
    std::vector<Row> rows;
    std::vector<std::size_t> start; // bucket b is rows[start[b], start[b + 1])
  };
  std::vector<Scattered> scattered(pieces.size());
  parallelism.For(pieces.size(), [&](std::size_t p) {
    auto &piece = pieces[p];
    auto &out = scattered[p];
    std::vector<uint16_t> bucket(piece.size());
    out.start.assign(buckets + 1, 0);
    for (std::size_t i = 0; i < piece.size(); ++i) {
      bucket[i] = static_cast<uint16_t>(
          std::upper_bound(splitters.begin(), splitters.end(), key(piece[i])) -
          splitters.begin());
      ++out.start[bucket[i] + 1];
    }
    for (std::size_t b = 0; b < buckets; ++b)
      out.start[b + 1] += out.start[b];
    std::vector<std::size_t> next(out.start.begin(), out.start.end() - 1);
    out.rows.resize(piece.size());
    for (std::size_t i = 0; i < piece.size(); ++i)
      out.rows[next[bucket[i]]++] = std::move(piece[i]);
    std::vector<Row>().swap(piece);
  });

  PieceList<Row> sorted(buckets);
  parallelism.For(buckets, [&](std::size_t b) {
    auto &out = sorted[b];
    for (auto &from : scattered)
      out.insert(out.end(), std::make_move_iterator(from.rows.begin() + from.start[b]),
                 std::make_move_iterator(from.rows.begin() + from.start[b + 1]));
    std::stable_sort(out.begin(), out.end(), less);
  });
  return sorted;
}

// Start offset of every piece in the concatenation, then the total.
template <class Row> std::vector<std::size_t> PieceOffsets(const PieceList<Row> &pieces) {
  std::vector<std::size_t> at(pieces.size() + 1, 0);
  for (std::size_t i = 0; i < pieces.size(); ++i)
    at[i + 1] = at[i] + pieces[i].size();
  return at;
}

// A sort key for strings: the 8 bytes after the first `skip`, which every string shares
// (SharedPrefixLength), so numbered names ("Supplier#000000042") still spread out.
inline uint64_t StringPrefixKey(std::string_view s, std::size_t skip) {
  uint64_t key = 0;
  for (std::size_t i = 0; i < 8; ++i)
    key = key << 8 | (skip + i < s.size() ? static_cast<unsigned char>(s[skip + i]) : 0);
  return key;
}

// Length of the prefix shared by all strings: that of the smallest and the largest.
template <class Row, class NameOf>
std::size_t SharedPrefixLength(const Parallelism &parallelism, const PieceList<Row> &pieces,
                               NameOf name_of) {
  struct Extremes {
    std::string_view lo, hi;
    bool any = false;
  };
  std::vector<Extremes> per_piece(pieces.size());
  parallelism.For(pieces.size(), [&](std::size_t p) {
    auto &e = per_piece[p];
    for (const auto &row : pieces[p]) {
      const std::string_view name = name_of(row);
      if (!e.any || name < e.lo)
        e.lo = name;
      if (!e.any || name > e.hi)
        e.hi = name;
      e.any = true;
    }
  });
  Extremes all;
  for (const auto &e : per_piece) {
    if (!e.any)
      continue;
    if (!all.any || e.lo < all.lo)
      all.lo = e.lo;
    if (!all.any || e.hi > all.hi)
      all.hi = e.hi;
    all.any = true;
  }
  std::size_t n = 0;
  while (n < all.lo.size() && n < all.hi.size() && all.lo[n] == all.hi[n])
    ++n;
  return n;
}

} // namespace accelerator_codegen::aux_state::core
