#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_query_module.hpp"
#include "accelerator_codegen/stateful/prims/sharded_sorted_map.hpp"
#include "tpch/q3/layout.hpp"

namespace accelerator_codegen::aux_state::query {

struct TpchQ3Config {
  core::TableId customer_table = 0;
  core::TableId orders_table = 0;
  core::TableId lineitem_table = 0;
};

struct TpchQ3CustomerInput {
  int64_t key = 0;
  std::string mktsegment;
};

struct TpchQ3OrderInput {
  int64_t key = 0;
  int32_t orderdate_days = 0;
  int64_t custkey = 0;
  int32_t shippriority = 0;
};

struct TpchQ3LineitemInput {
  int64_t orderkey = 0;
  int64_t extended_price_raw = 0;
  int32_t extended_price_scale = 2;
  int64_t discount_raw = 0;
  int32_t discount_scale = 2;
  int32_t shipdate_days = 0;
};

class TpchQ3InitialScan {
public:
  virtual ~TpchQ3InitialScan() = default;
  virtual void ScanCustomers(
      const std::function<void(const TpchQ3CustomerInput &)> &) = 0;
  virtual void ScanOrders(
      const std::function<void(const TpchQ3OrderInput &)> &) = 0;
  virtual void ScanLineitems(
      const std::function<void(const TpchQ3LineitemInput &)> &) = 0;

  // orders ⨝ customer lookup for RF1 lineitems / orders Insert when the
  // dense order_segment side does not yet know orderkey.
  struct OrderContext {
    core::PartitionId segment = 0;
    int32_t orderdate_days = 0;
    int32_t shippriority = 0;
    int64_t custkey = 0;
  };
  virtual bool LookupOrderContext(int64_t orderkey, OrderContext &out) {
    (void)orderkey;
    (void)out;
    return false;
  }

  // Point lookups on the same connection (the transaction's own writes are
  // visible). Return false when unsupported.
  virtual bool ScanOrdersByCustomer(
      int64_t custkey,
      const std::function<void(const TpchQ3OrderInput &)> &consumer) {
    (void)custkey;
    (void)consumer;
    return false;
  }
  virtual bool ScanLineitemsByOrders(
      const std::vector<int64_t> &orderkeys,
      const std::function<void(const TpchQ3LineitemInput &)> &consumer) {
    (void)orderkeys;
    (void)consumer;
    return false;
  }

  // Optional: DuckDB join filtered to one market segment for RebuildPartition.
  virtual bool ScanSegmentJoin(
      const std::string &mktsegment,
      const std::function<void(const TpchQ3OrderInput &,
                               const TpchQ3LineitemInput &)> &consumer) {
    (void)mktsegment;
    (void)consumer;
    return false;
  }
};

// Packed live records. Encode still writes a fixed wire format.
// TPC-H dates are DuckDB days since 1970 (~8000-11000 for 1992-1998); orderkeys
// fit uint32 through at least SF100.
#pragma pack(push, 1)
struct TpchQ3ShipdateBucket {
  uint16_t shipdate = 0;
  int64_t suffix_revenue = 0;
};
#pragma pack(pop)

static_assert(sizeof(TpchQ3ShipdateBucket) == 10,
              "Q3 shipdate bucket must stay 10 bytes packed");

inline uint16_t PackTpchQ3Date(int32_t days) {
  if (days < 0 || days > static_cast<int32_t>(std::numeric_limits<uint16_t>::max()))
    throw std::runtime_error("TPC-H Q3 date exceeds uint16 packed range");
  return static_cast<uint16_t>(days);
}

inline uint32_t PackTpchQ3Orderkey(int64_t orderkey) {
  if (orderkey < 0 ||
      orderkey > static_cast<int64_t>(std::numeric_limits<uint32_t>::max()))
    throw std::runtime_error("TPC-H Q3 orderkey exceeds uint32 packed range");
  return static_cast<uint32_t>(orderkey);
}

inline uint8_t PackTpchQ3Shippriority(int32_t shippriority) {
  if (shippriority < 0 ||
      shippriority > static_cast<int32_t>(std::numeric_limits<uint8_t>::max()))
    throw std::runtime_error("TPC-H Q3 shippriority exceeds uint8 packed range");
  return static_cast<uint8_t>(shippriority);
}

// ---------------------------------------------------------------------------
// Persistent (copy-on-write) hash-sharded sorted maps keyed by uint32 `key`
// (core::ShardedSortedMap, 2^18 shards: 1024 top slots x 256 leaves per mid).
// A published index is never mutated: a write copies the top array, the
// touched mid arrays and the touched leaves (a few hundred entries);
// everything else stays shared.
// ---------------------------------------------------------------------------
struct TpchQ3KeyOf {
  template <typename E> const uint32_t &operator()(const E &e) const {
    return e.key;
  }
};
template <typename E>
using TpchQ3ShardIndex =
    core::ShardedSortedMap<uint32_t, E, TpchQ3KeyOf,
                           core::ShardedSortedMapHash<uint32_t>, 18, 8>;

// Per-order record (every order whose customer is in a tracked segment).
// `bound` is an upper bound on the order's revenue for ANY cutoff: the sum of
// its positive per-shipdate revenues. It is the sort key of the order's entry
// in the segment index (valid iff indexed()).
struct TpchQ3OrderRec {
  uint32_t key = 0;
  uint16_t orderdate = 0;
  uint8_t seg_flags = 0; // low 7 bits: segment partition id; 0x80: indexed
  uint8_t shippriority = 0;
  int64_t bound = 0;

  core::PartitionId segment() const { return seg_flags & 0x7F; }
  bool indexed() const { return (seg_flags & 0x80) != 0; }
};
static_assert(sizeof(TpchQ3OrderRec) == 16, "Q3 order record must stay 16 bytes");

// Customer record. segment != 0: tracked customer. segment == 0: the key is
// not a tracked customer (absent, deleted or unknown segment); then
// orphan_orders counts the orders that reference it (they are untracked), so a
// later insert / segment change knows whether it has to look them up.
struct TpchQ3CustRec {
  uint32_t key = 0;
  uint32_t orphan_orders = 0;
  uint8_t segment = 0;
};

// Side partition: orderkey -> order record, custkey -> segment.
struct TpchQ3Side {
  TpchQ3ShardIndex<TpchQ3OrderRec> orders;
  TpchQ3ShardIndex<TpchQ3CustRec> custs;
};

// ---------------------------------------------------------------------------
// Segment partition: orders with >= 1 lineitem, grouped into 16-day orderdate
// bins; each bin is a list of immutable chunks sorted by (bound desc,
// orderkey asc). A chunk owns its entries and their shipdate buckets.
// Query: only bins that can hold an order with orderdate < cutoff < maxship are
// visited, and a best-first merge over the bins stops as soon as the next
// bound cannot beat the current 10th result.
// ---------------------------------------------------------------------------
constexpr uint32_t kTpchQ3BinShift = 4;
constexpr std::size_t kTpchQ3ChunkMax = 256;
constexpr std::size_t kTpchQ3ChunkTarget = 128;

struct TpchQ3Entry {
  int64_t bound = 0;
  uint32_t orderkey = 0;
  uint32_t bucket_begin = 0;
  uint16_t orderdate = 0;
  uint16_t bucket_count = 0;
  uint16_t maxship = 0;
  uint8_t shippriority = 0;
  uint8_t pad = 0;
};

struct TpchQ3Chunk {
  std::vector<TpchQ3Entry> entries;
  std::vector<TpchQ3ShipdateBucket> pool;
  uint16_t max_maxship = 0;
};

struct TpchQ3BinList {
  std::vector<std::shared_ptr<const TpchQ3Chunk>> chunks;
  uint16_t max_maxship = 0;
};

struct TpchQ3Segment {
  core::PartitionId segment = 0;
  std::map<uint16_t, std::shared_ptr<const TpchQ3BinList>> bins;
};

enum class TpchQ3ImageKind : uint8_t {
  OrderSegmentSide,
  SegmentIntervalIndex,
};

struct TpchQ3StateImage {
  TpchQ3ImageKind kind = TpchQ3ImageKind::OrderSegmentSide;
  core::PartitionId segment = 0;
  std::shared_ptr<const TpchQ3Side> side;
  std::shared_ptr<const TpchQ3Segment> state;
};

struct TpchQ3OutputRow {
  int64_t orderkey = 0;
  int64_t revenue_raw = 0;
  int32_t orderdate_days = 0;
  int32_t shippriority = 0;
};

struct TpchQ3LineitemDelta {
  core::PartitionId segment = 0;
  int64_t orderkey = 0;
  int32_t shipdate_day = 0;
  int64_t revenue_raw = 0;
  int8_t sign = 0;
};

struct TpchQ3OrderDelta {
  int64_t orderkey = 0;
  int32_t orderdate_days = 0;
  int64_t custkey = 0;
  int32_t shippriority = 0;
  int8_t sign = 0; // +1 insert, -1 delete
};

struct TpchQ3CustomerDelta {
  enum class Kind : uint8_t { Insert, Delete, Move };
  Kind kind = Kind::Insert;
  int64_t custkey = 0;
  core::PartitionId segment = 0; // new segment; 0 = not a tracked segment
};

std::vector<uint8_t> EncodeTpchQ3OrderSegmentSide(const TpchQ3Side &side);
std::vector<uint8_t> EncodeTpchQ3SegmentIntervalIndex(const TpchQ3Segment &);
bool DecodeTpchQ3OrderSegmentSide(const std::vector<uint8_t> &, TpchQ3Side &);
bool DecodeTpchQ3SegmentIntervalIndex(const std::vector<uint8_t> &,
                                      core::PartitionId segment,
                                      TpchQ3Segment &);

int64_t TpchQ3RevenueRaw(int64_t extended_price_raw, int32_t extended_scale,
                         int64_t discount_raw, int32_t discount_scale);
core::PartitionId TpchQ3PartitionFromMktsegment(const std::string &segment);
std::string TpchQ3MktsegmentFromPartition(core::PartitionId partition);
int32_t TpchQ3DaysFromYmd(int32_t year, unsigned month, unsigned day);

void FinalizeTpchQ3OrderSummary(std::vector<TpchQ3ShipdateBucket> &buckets,
                                const std::map<int32_t, int64_t> &day_revenue);
int64_t TpchQ3RevenueAboveCutoff(const TpchQ3Entry &,
                                 const TpchQ3ShipdateBucket *buckets,
                                 int32_t cutoff_days);

std::vector<TpchQ3OutputRow> ComposeTpchQ3TopK(const TpchQ3Segment &,
                                               int32_t cutoff_days);

class TpchQ3TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  const std::vector<TpchQ3LineitemDelta> &Deltas() const;
  const std::vector<TpchQ3OrderDelta> &OrderDeltas() const;
  void MarkRebuild();
  void AddDelta(TpchQ3LineitemDelta delta);
  void AddOrderDelta(TpchQ3OrderDelta delta);
  const std::vector<TpchQ3CustomerDelta> &CustomerDeltas() const;
  void AddCustomerDelta(TpchQ3CustomerDelta delta);
  // A mutation that provably leaves the state unchanged (keeps the buffer
  // pending so the coordinator does not escalate to a full invalidation).
  void MarkNoop();

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<TpchQ3LineitemDelta> deltas;
  std::vector<TpchQ3OrderDelta> order_deltas;
  std::vector<TpchQ3CustomerDelta> customer_deltas;
};

class TpchQ3Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = core::TpchQ3Layout::kAccelerator;

  explicit TpchQ3Module(TpchQ3Config config,
                        TpchQ3InitialScan *initial_scan = nullptr)
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
  void ExecuteFallback(core::AuxEngineServices &, const core::QueryArguments &,
                       core::AuxRowSink &) override;
  std::vector<core::CalibrationWrite> CalibrationWrites() const override;

private:
  struct BuiltState {
    TpchQ3Side side;
    std::map<core::PartitionId, std::shared_ptr<const TpchQ3Segment>> segments;
  };

  BuiltState BuildState(core::AuxEngineServices &) const;
  bool LoadSegment(core::AuxEngineServices &, core::PartitionId,
                   std::shared_ptr<const TpchQ3Segment> &) const;
  void EmitResult(const std::vector<TpchQ3OutputRow> &, core::AuxRowSink &) const;

  TpchQ3Config config;
  TpchQ3InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
