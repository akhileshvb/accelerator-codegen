// Rows of a partitioned build scan dealt into buckets by the caller's key, for builds that then
// work bucket by bucket on the pool.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <utility>
#include <vector>

#include "substrate/aux/core/parallel_slices.hpp"

namespace accelerator_codegen::aux_state::query {

// Buckets build rows are dealt to before a pass on the pool: 2^kDealBucketBits.
inline constexpr unsigned kDealBucketBits = 10;
inline constexpr std::size_t kDealBuckets = std::size_t{1} << kDealBucketBits;

// A scan partial that merges by linking: partials keep their rows where they were decoded, so
// merging costs O(1) however many rows they hold. Bucket b of a partition holds the rows that
// partition dealt to b; ForEach visits them in partition order.
template <class Row> class DealtRows {
public:
  explicit DealtRows(std::size_t buckets) : buckets_(buckets) { runs_.emplace_back(buckets); }

  std::size_t BucketCount() const { return buckets_; }

  // Deals `row`, whose key is `key`, to `bucket`.
  void Add(std::size_t bucket, int64_t key, Row row) {
    max_key_ = std::max(max_key_, key);
    runs_.front()[bucket].push_back(std::move(row));
  }

  // The largest key dealt, -1 when no row was.
  int64_t MaxKey() const { return max_key_; }

  void Merge(DealtRows &&from) {
    max_key_ = std::max(max_key_, from.max_key_);
    for (auto &run : from.runs_) {
      runs_.push_back(std::move(run));
    }
    from.runs_.clear();
  }

  std::size_t RunCount() const { return runs_.size(); }
  std::vector<Row> &Rows(std::size_t run, std::size_t bucket) { return runs_[run][bucket]; }

  std::size_t Size() const {
    std::size_t n = 0;
    for (const auto &run : runs_) {
      for (const auto &bucket : run) {
        n += bucket.size();
      }
    }
    return n;
  }

  std::size_t BucketSize(std::size_t bucket) const {
    std::size_t n = 0;
    for (const auto &run : runs_) {
      n += run[bucket].size();
    }
    return n;
  }

  // fn(partition, row) for every row of `bucket`, partition by partition. The rows are
  // released as they are visited, on the visiting thread: visit each bucket once.
  template <class Fn> void ForEachWithPartition(std::size_t bucket, Fn &&fn) {
    for (std::size_t run = 0; run < runs_.size(); ++run) {
      for (const auto &row : runs_[run][bucket]) {
        fn(run, row);
      }
      runs_[run][bucket] = {};
    }
  }

  // fn(row) for every row of `bucket`, partition by partition, released like
  // ForEachWithPartition does.
  template <class Fn> void ForEach(std::size_t bucket, Fn &&fn) {
    ForEachWithPartition(bucket, [&](std::size_t, const Row &row) { fn(row); });
  }

  // Moves the rows of `bucket` out, in partition order.
  std::vector<Row> TakeBucket(std::size_t bucket) {
    std::vector<Row> out;
    out.reserve(BucketSize(bucket));
    for (auto &run : runs_) {
      auto &rows = run[bucket];
      out.insert(out.end(), std::make_move_iterator(rows.begin()),
                 std::make_move_iterator(rows.end()));
      rows = {};
    }
    return out;
  }

  // Frees the bucket tables of all partitions on the pool, for rows that were all taken: the
  // tables hold one (empty) vector per partition and bucket, too many to free serially.
  void Release(const core::AuxEngineServices &services) {
    core::ParallelEach(services, runs_.size(), 1,
                       [&](std::size_t run) { std::vector<std::vector<Row>>().swap(runs_[run]); });
  }

private:
  std::size_t buckets_;
  int64_t max_key_ = -1;
  std::vector<std::vector<std::vector<Row>>> runs_; // [partition][bucket]
};

// fn(bucket) for every bucket in [0, buckets), one task per group of consecutive buckets
// (`groups` of them, for buckets that must not run concurrently with their group mates).
template <class Fn>
void ParallelBucketGroups(const core::AuxEngineServices &services, std::size_t buckets,
                          std::size_t groups, Fn &&fn) {
  groups = std::clamp<std::size_t>(groups, 1, buckets);
  core::ParallelEach(services, groups, 1, [&](std::size_t group) {
    for (std::size_t bucket = buckets * group / groups; bucket < buckets * (group + 1) / groups;
         ++bucket) {
      fn(bucket);
    }
  });
}

// fn(bucket, row) for every row, the buckets on the pool and each bucket's rows in partition
// order; the rows are released as they are visited.
template <class Row, class Fn>
void ParallelForEachRow(const core::AuxEngineServices &services, DealtRows<Row> &rows,
                        Fn &&fn) {
  core::ParallelEach(services, rows.BucketCount(), 1, [&](std::size_t bucket) {
    rows.ForEach(bucket, [&](const Row &row) { fn(bucket, row); });
  });
}

} // namespace accelerator_codegen::aux_state::query
