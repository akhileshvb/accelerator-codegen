// Partitioned typed table scan offered by the DuckDB InitialScan of a query,
// for parallel builds (core::PartitionedBuild, backends/duckdb/engine/build_scan.hpp).
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "backends/duckdb/engine/build_scan.hpp"
#include "substrate/aux/core/aux_types.hpp"
#include "substrate/aux/core/partitioned_build.hpp"
#include "tpch/common/dealt_rows.hpp"

namespace accelerator_codegen::aux_state::query {

namespace scan = ::accelerator_codegen::aux_state::engine::duckdb::scan;

// Adds to a counter of an index that parallel build partitions share: integer
// addition commutes, so the result is the serial one.
template <class T> void AtomicAdd(T &value, T delta) {
  __atomic_fetch_add(&value, delta, __ATOMIC_RELAXED);
}

class PartitionedTableScan {
public:
  using Sink = std::function<void(std::size_t, const scan::ChunkBatch &)>;

  virtual ~PartitionedTableScan() = default;

  // Streams `table` as typed batches (see scan::ScanPartitioned): sink(partition,
  // batch), one thread per partition, partitions concurrent.
  virtual void ScanTable(const core::AuxEngineServices &services, const std::string &table,
                         const std::vector<scan::BuildColumn> &columns,
                         std::size_t partitions, const Sink &sink) = 0;
};

// The rows of `table` decoded by `decode(row)` on the pool, in table order
// whatever the thread count: partitions are contiguous and merge in order.
template <class Row, class Decode>
std::vector<Row> CollectTable(const core::AuxEngineServices &services,
                              PartitionedTableScan &source, const std::string &table,
                              const std::vector<scan::BuildColumn> &columns, Decode &&decode) {
  return core::PartitionedBuild<std::vector<Row>>(
      services, [] { return std::vector<Row>{}; },
      [&](std::size_t partitions, auto &&sink) {
        source.ScanTable(services, table, columns, partitions, sink);
      },
      [&](std::vector<Row> &rows, const auto &row) { rows.push_back(decode(row)); },
      core::AppendRows<Row>, core::BuildPartitions(services));
}

// The rows of `table` dealt to `buckets` by `deal(rows, row)` on the pool; partitions
// stay separate (no row is copied to merge them).
template <class Row, class Deal>
DealtRows<Row> DealTable(const core::AuxEngineServices &services, PartitionedTableScan &source,
                         const std::string &table, const std::vector<scan::BuildColumn> &columns,
                         std::size_t buckets, Deal &&deal) {
  return core::PartitionedBuild<DealtRows<Row>>(
      services, [buckets] { return DealtRows<Row>(buckets); },
      [&](std::size_t partitions, auto &&sink) {
        source.ScanTable(services, table, columns, partitions, sink);
      },
      [&](DealtRows<Row> &rows, const auto &row) { deal(rows, row); },
      [](DealtRows<Row> &into, DealtRows<Row> &&from) { into.Merge(std::move(from)); },
      core::BuildPartitions(services));
}

// A sorted-map builder with the largest key it was fed.
template <class Map> struct ScannedMap {
  typename Map::Builder builder;
  int64_t max_key;
};

// The rows of `table` that `make(row, element)` turns into elements of a map, dealt by
// shard group and added bucket by bucket on the pool to the builder `make_builder(rows)`
// returns (a builder of any width: a bucket never spans two of its shards).
// `visit(bucket, element)` sees every added element.
template <class Map, class MakeBuilder, class Make, class Visit>
ScannedMap<Map> ScanIntoBuilder(const core::AuxEngineServices &services,
                                PartitionedTableScan &source, const std::string &table,
                                const std::vector<scan::BuildColumn> &columns,
                                MakeBuilder &&make_builder, Make &&make, Visit &&visit) {
  using Element = typename Map::Element;
  auto dealt = DealTable<Element>(
      services, source, table, columns, kDealBuckets,
      [&](DealtRows<Element> &rows, const auto &row) {
        Element element;
        if (make(row, element))
          rows.Add(Map::ShardOf(element.key, kDealBucketBits), element.key, element);
      });
  ScannedMap<Map> out{make_builder(dealt.Size()), dealt.MaxKey()};
  ParallelBucketGroups(services, kDealBuckets, out.builder.shard_count(), [&](std::size_t bucket) {
    dealt.ForEach(bucket, [&](const Element &element) {
      out.builder.Add(element);
      visit(bucket, element);
    });
  });
  return out;
}

} // namespace accelerator_codegen::aux_state::query
