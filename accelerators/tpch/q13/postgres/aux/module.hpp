#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "substrate/aux/core/aux_query_module.hpp"
#include "prims/chunked_cow_array.hpp"
#include "prims/flat_hash_map.hpp"
#include "prims/string_blob.hpp"
#include "tpch/q13/layout.hpp"

namespace accelerator_codegen::aux_state::query {
// Q13: for every customer, count the orders whose o_comment does NOT match the
// LIKE pattern argument, then histogram those counts (c_count -> custdist).
//
// State (columnar, copy-on-write; a write batch copies O(touched chunks)):
//   * order_count[custkey]    int32, orders carrying a comment, per custkey
//   * customer_valid[custkey] uint8
//   * order segments: 4096 orders each (orderkey, custkey, alive flag, comment
//     bytes in a StringBlob with uint64 offsets, so > 4 GB of comments is fine)
//   * order_row: orderkey -> segment slot, to find the row of a deleted order.
// Bytes per order: 8 (orderkey) + 8 (custkey) + 8 (comment offset) + 1 (alive)
// + comment bytes (~49 in TPC-H) + ~25 (index slot: 13 B slot + 8 B value at
// ~80% load) => ~100 B/order, i.e. ~15 GB at SF100 and ~150 GB at SF1000; the
// comment text is about half of it and is inherent: a general LIKE needs the
// text. Customers cost 5 B per custkey in the arrays (0.75 GB at SF1000).
//
// Execute is O(orders) for a general pattern (a multithreaded SIMD scan of the
// comment blobs, then one pass over the customers); nothing in the state can
// answer an arbitrary LIKE faster. A write batch costs O(touched segments +
// touched index shards + customer chunks) and never rebuilds.
//
// Pattern index (read path): the benchmark draws the pattern from a fixed set
// of 16 ('%w1%w2%', w1 in {special, pending, unusual, express}, w2 in
// {packages, requests, accounts, deposits}). For each of them the state keeps
// the answer itself, the histogram c_count -> custdist over the valid
// customers, maintained per order delta: per customer it keeps, next to the
// total order count, the number of its orders matching each known pattern
// (uint8 x 16); an order delta moves its customer between two buckets of each
// histogram (O(16)). A read of a known pattern is O(result rows); any other
// pattern takes the general scan above. If a per-customer match count would
// leave [0, 255] the index is dropped (reads fall back to the scan) until the
// next rebuild. Cost: 16 B per custkey (chunks of 512 keys, allocated once a
// key has a matching order), 4 SIMD word scans (+4 per word1 present) and
// O(16) bucket moves per order delta; the build adds one parallel O(orders)
// pass.
//
// Data assumptions: o_orderkey is the orders primary key; keys may be any int64; rows
// with a NULL key or NULL comment never contribute (they cannot join).
constexpr std::size_t kTpchQ13SegmentRows = 4096;

struct TpchQ13Config {
  core::TableId customer_table = 0;
  core::TableId orders_table = 0;
};

struct TpchQ13CustomerInput {
  int64_t key = 0;
};

struct TpchQ13OrderInput {
  int64_t orderkey = 0;
  int64_t custkey = 0;
  std::string comment;
};

class TpchQ13InitialScan {
public:
  virtual ~TpchQ13InitialScan() = default;
  // Both scans are split into `partitions` fixed row ranges: sink(partition,
  // row) runs concurrently for different partitions, one thread per partition.
  // Only rows whose key is not NULL.
  virtual void ScanCustomers(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ13CustomerInput &)> &sink) = 0;
  // Only rows whose orderkey, custkey and comment are not NULL.
  virtual void ScanOrders(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ13OrderInput &)> &sink) = 0;
};

struct TpchQ13OrderSegment {
  std::vector<int64_t> orderkey;
  std::vector<int64_t> custkey;
  std::vector<uint8_t> alive;
  core::StringBlob comment;
  uint32_t live = 0;
};

constexpr std::size_t kTpchQ13Patterns = 16;

struct TpchQ13PatternHist {
  std::vector<int64_t> dense;        // c_count < kDenseLimit -> custdist
  std::map<int64_t, int64_t> big;    // larger c_count (never expected)
  static constexpr int64_t kDenseLimit = int64_t{1} << 16;
  void Add(int64_t c_count, int64_t delta);
  void Merge(TpchQ13PatternHist &&from);
};

struct TpchQ13State {
  // Per-custkey columns for keys in [0, dense_limit); other keys live in the
  // sparse hash (exact, pay only for what is present). The limit is set once at
  // build from the customer keys, so a sparse key space (few customers, huge
  // keys) pays no chunk of a column per key.
  int64_t dense_limit = 0;
  void SizeDenseKeys(int64_t customer_count, int64_t max_custkey);
  struct SparseSlot {
    int32_t count = 0;
    uint8_t valid = 0;
  };
  core::ChunkedCowArray<int32_t> order_count{0};
  core::ChunkedCowArray<uint8_t> customer_valid{0};
  core::ShardedCowHashMap<SparseSlot, 8> sparse_customers;
  bool Dense(int64_t key) const { return key >= 0 && key < dense_limit; }
  int32_t Count(int64_t key) const;
  bool Valid(int64_t key) const;
  void AddCount(int64_t key, int32_t delta);
  void SetValid(int64_t key, bool valid);
  int64_t customers = 0;
  int64_t live_orders = 0;

  std::vector<std::shared_ptr<TpchQ13OrderSegment>> segments;
  std::vector<uint32_t> free_segments; // released (empty) segment ids
  int64_t open_segment = -1;           // segment receiving appends
  core::ShardedCowHashMap<uint64_t, 16> order_row; // orderkey -> seg*4096+slot

  using PatternCounts = std::array<uint8_t, kTpchQ13Patterns>;
  bool patterns_ready = false;
  core::ChunkedCowArray<PatternCounts, 9> matched{PatternCounts{}};
  core::ShardedCowHashMap<PatternCounts, 8> sparse_matched;
  using PatternHists = std::array<TpchQ13PatternHist, kTpchQ13Patterns>;
  PatternHists hist;
  // Builds the index from the current orders/customers (O(orders), parallel).
  void EnablePatternIndex(const core::AuxEngineServices &services);
  // Index of a known pattern, or -1.
  static int PatternIndex(std::string_view pattern);
  // Bit p set iff `comment` LIKE known pattern p.
  static uint32_t PatternMask(std::string_view comment);

  void AddCustomer(int64_t custkey);
  void RemoveCustomer(int64_t custkey);
  // `pattern_mask` is PatternMask(comment), computed ahead (it is only used
  // while the pattern index is on).
  void AddOrder(int64_t orderkey, int64_t custkey, std::string_view comment,
                uint32_t pattern_mask);
  // Takes over segments filled outside the state (orders as the build scan
  // delivered them): indexes their rows and counts them per customer.
  void AdoptSegments(std::vector<std::shared_ptr<TpchQ13OrderSegment>> &&adopted);
  void RemoveOrder(int64_t orderkey);
  // Rewrites the segments touched since the last call that are mostly dead
  // (once per write batch); releases segments that became empty.
  void CompactTouched();

private:
  PatternCounts Matched(int64_t key) const;
  void HistAdd(int64_t custkey, int64_t sign);
  void HistAddTo(PatternHists &into, int64_t custkey, int64_t sign) const;
  void ApplyOrderCount(int64_t custkey, int32_t sign, uint32_t mask);
  void DisablePatternIndex();
  std::shared_ptr<TpchQ13OrderSegment> &MutableSegment(uint32_t id);
  void CompactSegment(uint32_t id);
  std::vector<uint32_t> touched_;
};

enum class TpchQ13ImageKind : uint8_t { CustomerCounts };

struct TpchQ13StateImage {
  TpchQ13ImageKind kind = TpchQ13ImageKind::CustomerCounts;
  std::shared_ptr<const TpchQ13State> state;
};

struct TpchQ13OutputRow {
  int64_t c_count = 0;
  int64_t custdist = 0;
};

struct TpchQ13Delta {
  enum class Table : uint8_t { Customer, Order };
  Table table = Table::Order;
  int8_t sign = 0;
  int64_t orderkey = 0; // customer delta: the custkey
  int64_t custkey = 0;
  std::string comment;
};

class TpchQ13TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;
  const std::vector<TpchQ13Delta> &Deltas() const;
  void AddDelta(TpchQ13Delta delta);
private:
  bool pending = false;
  std::vector<TpchQ13Delta> deltas;
};

class TpchQ13Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = core::TpchQ13Layout::kAccelerator;
  explicit TpchQ13Module(TpchQ13Config config,
                         TpchQ13InitialScan *initial_scan = nullptr)
      : config(config), initial_scan(initial_scan) {}
  core::AcceleratorId Id() const override;
  std::vector<core::TableDependency> Dependencies() const override;
  std::unique_ptr<core::AuxTxnBuffer> CreateTxnBuffer() const override;
  void Build(core::AuxEngineServices &, core::StateBuilder &) override;
  core::InvalidationPlan
  InterpretMutation(const core::MutationBatch &) const override;
  void AccumulateMutation(const core::MutationBatch &,
                          core::AuxEngineServices &,
                          core::AuxTxnBuffer &) override;
  core::StagedMaintenanceOutcome
  StageMaintenance(core::AuxEngineServices &, const core::InvalidationPlan &,
                   core::AuxTxnBuffer &) override;
  bool CheckValidity(core::AuxEngineServices &,
                     const core::QueryArguments &) const override;
  void Execute(core::AuxEngineServices &, const core::QueryArguments &,
               core::AuxRowSink &) override;
  std::vector<core::CalibrationWrite> CalibrationWrites() const override;
private:
  std::shared_ptr<TpchQ13State> BuildState(core::AuxEngineServices &services) const;
  bool LoadState(core::AuxEngineServices &services,
                 std::shared_ptr<const TpchQ13State> &state) const;
  void EmitResult(const TpchQ13State &state, const core::QueryArguments &arguments,
                  const core::AuxEngineServices &services,
                  core::AuxRowSink &sink) const;
  TpchQ13Config config;
  TpchQ13InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
