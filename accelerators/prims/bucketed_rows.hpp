#pragma once
//
// BucketedRows<Row>
// PURPOSE    Partial of a partitioned scan that deals each row into one of kRowBuckets
//            buckets as it arrives. Merging only links the pieces, so a bulk load can
//            afterwards work bucket by bucket on the pool: no per-partition structure
//            sized by the key space and no merge chain.
// COMPLEXITY Add O(1); Absorb O(pieces); Take(bucket) O(rows of that bucket).
// MEMORY     O(rows seen) plus kRowBuckets vector headers per partition.
// THREADS    A partial is private to its scan partition until merged. Take of
//            different buckets may run concurrently.
// USE WHEN   rows are grouped by a hash or range of a key (orders, pages, ...) and each
//            group is then built independently. Rows of a bucket come back in scan
//            order (partition order, table order inside a partition).
//

#include <cstddef>
#include <iterator>
#include <utility>
#include <vector>

namespace accelerator_codegen::aux_state::core {

inline constexpr unsigned kRowBucketBits = 12;
inline constexpr std::size_t kRowBuckets = std::size_t{1} << kRowBucketBits;

template <class Row> class BucketedRows {
public:
  void Add(std::size_t bucket, const Row &row) { mine[bucket].push_back(row); }

  void Absorb(BucketedRows &&from) {
    absorbed.push_back(std::move(from.mine));
    for (auto &pieces : from.absorbed)
      absorbed.push_back(std::move(pieces));
  }

  // The rows of `bucket` in scan order; the pieces give them up.
  std::vector<Row> Take(std::size_t bucket) {
    std::size_t total = mine[bucket].size();
    for (const auto &pieces : absorbed)
      total += pieces[bucket].size();
    std::vector<Row> rows = std::move(mine[bucket]);
    rows.reserve(total);
    for (auto &pieces : absorbed) {
      rows.insert(rows.end(), std::make_move_iterator(pieces[bucket].begin()),
                  std::make_move_iterator(pieces[bucket].end()));
      std::vector<Row>().swap(pieces[bucket]);
    }
    return rows;
  }

private:
  using Buckets = std::vector<std::vector<Row>>;
  Buckets mine = Buckets(kRowBuckets);
  std::vector<Buckets> absorbed;
};

} // namespace accelerator_codegen::aux_state::core
