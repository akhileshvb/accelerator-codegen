#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_query_module.hpp"

namespace accelerator_codegen::aux_state::query {

// ── Q17 layout ──────────────────────────────────────────────────────────────
// Maintenance uses small pages (1024 partkeys) of per-part (quantity -> count,
// price sum) buckets, so a lineitem/part delta updates state locally with no
// SQL; reads use one AttrParts partition per (brand, container) holding the
// running total (O(1) read).
struct TpchQ17Layout {
  static constexpr const char *kAccelerator = "tpch_q17_aux";
  static constexpr core::PartitionId kPartStatsChunkBase = 0;
  // (brand, container) -> partition by a 64-bit hash into [2^30, 2^31): the
  // store keys partitions as int32 (state/partition_key.hpp), and part chunks
  // (partkey >> 10) stay below 2^30 for partkeys below 2^40.  Two pairs in one
  // slot (~5e-4 for TPC-H's 1000 pairs) cost a fallback, never a wrong answer:
  // the image carries its brand/container and a read checks them.
  static constexpr core::PartitionId kAttrPartsBase = core::PartitionId{1} << 30;
  static constexpr core::PartitionId kAttrPartsSlots = core::PartitionId{1} << 30;
  static constexpr std::size_t kPartChunkShift = 10;
  static constexpr std::size_t kPartChunkSize = 1u << kPartChunkShift;

  static constexpr core::PartitionId PartStatsChunkPartition(int64_t partkey) {
    if (partkey < 0)
      return kPartStatsChunkBase;
    return kPartStatsChunkBase +
           static_cast<core::PartitionId>(partkey >> kPartChunkShift);
  }

  static constexpr int64_t ChunkIndexFromPartition(core::PartitionId partition) {
    if (partition < kPartStatsChunkBase)
      return -1;
    return static_cast<int64_t>(partition - kPartStatsChunkBase);
  }
};

// ── Config ──────────────────────────────────────────────────────────────────
struct TpchQ17Config {
  core::TableId part_table = 0;
  core::TableId lineitem_table = 0;
};

// ── Typed bulk-scan contract ────────────────────────────────────────────────
struct TpchQ17PartInput {
  int64_t key = 0;
  std::string brand;
  std::string container;
};

struct TpchQ17LineitemInput {
  int64_t partkey = 0;
  int64_t quantity_raw = 0;       // decimal(15,2) raw (×100)
  int64_t extendedprice_raw = 0;  // decimal(15,2) raw (×100)
};

struct TpchQ17PartQuantityAggregate {
  int64_t partkey = 0;
  int64_t quantity_raw = 0;
  int64_t extendedprice_sum_raw = 0;
  int64_t row_count = 0;
};

class TpchQ17InitialScan {
public:
  virtual ~TpchQ17InitialScan() = default;
  virtual void ScanParts(
      const std::function<void(const TpchQ17PartInput &)> &) = 0;
  virtual void ScanLineitems(
      const std::function<void(const TpchQ17LineitemInput &)> &) = 0;

  // Optional: SQL GROUP BY (partkey, quantity) with summed price.
  virtual bool ScanPartQuantityAggregates(
      const std::function<void(const TpchQ17PartQuantityAggregate &)> &consumer) {
    (void)consumer;
    return false;
  }

  // Optional: same aggregate for a single partkey (incremental pair rebuild).
  virtual bool ScanPartQuantityAggregatesForPart(
      int64_t partkey,
      const std::function<void(const TpchQ17PartQuantityAggregate &)> &consumer) {
    (void)partkey;
    (void)consumer;
    return false;
  }

  virtual bool ScanPartQuantityAggregatesForParts(
      const std::unordered_set<int64_t> &partkeys,
      const std::function<void(const TpchQ17PartQuantityAggregate &)>
          &consumer) {
    if (partkeys.size() == 1)
      return ScanPartQuantityAggregatesForPart(*partkeys.begin(), consumer);
    (void)partkeys;
    (void)consumer;
    return false;
  }
};

// ── Per-part statistics ─────────────────────────────────────────────────────
// One (quantity -> row count, price sum) bucket of a part, sorted by quantity.
// 16 bytes; the count lets a delete retire a bucket exactly (bag semantics).
struct TpchQ17QtyPricePair {
  int32_t quantity_raw = 0;
  int32_t row_count = 0;
  int64_t extendedprice_raw = 0;
};

// Working (build-time) stats of one part.
struct TpchQ17PartStats {
  int64_t sum_qty_raw = 0;
  int32_t count = 0;
  std::vector<TpchQ17QtyPricePair> qty_price_pairs;
  int64_t precomputed_contribution_raw = 0;

  void RecomputeContribution();
};

// Published per-part entry. The pair buckets are shared and immutable, so
// copying a page (copy-on-write) copies pointers, not buckets.
struct TpchQ17PartEntry {
  int64_t partkey = 0;
  // Partition id of the part's (brand, container) AttrParts image; 0 = the
  // part has no row in `part` (lineitems only).
  core::PartitionId attr_partition = 0;
  int64_t sum_qty_raw = 0;
  int64_t contribution_raw = 0;
  int32_t count = 0;
  std::shared_ptr<const std::vector<TpchQ17QtyPricePair>> pairs;
};

// A small page of parts (kPartChunkSize partkeys), sorted by partkey.
struct TpchQ17PartStatsChunk {
  int64_t chunk_index = 0;
  std::vector<TpchQ17PartEntry> parts;
};

// One (brand, container): the running total of its parts' contributions.
struct TpchQ17AttrParts {
  std::string brand;
  std::string container;
  int64_t total_contribution_raw = 0;
  // Another (brand, container) hashes to this partition: the total mixes both
  // pairs, so reads of either fall back to SQL.
  bool shared = false;
};

// In-memory build aggregate (not stored as a single partition).
struct TpchQ17State {
  std::unordered_map<std::string, std::unordered_set<int64_t>> attribute_index;
  std::unordered_map<int64_t, TpchQ17PartStats> part_stats;
  std::unordered_map<int64_t, std::string> part_attr_keys;

  static std::string MakeAttrKey(const std::string &brand,
                                 const std::string &container);
  static void SplitAttrKey(const std::string &key, std::string &brand,
                           std::string &container);
};

enum class TpchQ17ImageKind : uint8_t { PartStatsChunk, AttrParts };

struct TpchQ17StateImage {
  TpchQ17ImageKind kind = TpchQ17ImageKind::AttrParts;
  int64_t chunk_index = 0;
  std::shared_ptr<const TpchQ17PartStatsChunk> chunk;
  std::shared_ptr<const TpchQ17AttrParts> attr_parts;
};

// ── Delta ───────────────────────────────────────────────────────────────────
struct TpchQ17LineitemDelta {
  int64_t partkey = 0;
  int64_t quantity_raw = 0;
  int64_t extendedprice_raw = 0;
  int8_t sign = 0;
};

// One part row appearing (sign +1) or disappearing (sign -1).
struct TpchQ17PartDelta {
  int64_t partkey = 0;
  std::string brand;
  std::string container;
  int8_t sign = 0;
};

// ── Encode / Decode (byte format retained for tooling) ──────────────────────
std::vector<uint8_t> EncodeTpchQ17AttributeIndex(const TpchQ17State &state);
std::vector<uint8_t> EncodeTpchQ17PartStats(int64_t partkey,
                                            const TpchQ17State &state);
bool DecodeTpchQ17AttributeIndex(const std::vector<uint8_t> &bytes,
                                 TpchQ17State &state);
bool DecodeTpchQ17PartStats(const std::vector<uint8_t> &bytes, int64_t partkey,
                            TpchQ17State &state);
bool FindTpchQ17PartKeys(
    const std::vector<uint8_t> &bytes, const std::string &brand,
    const std::string &container,
    std::unordered_set<int64_t> &partkeys);

// ── TxnBuffer ───────────────────────────────────────────────────────────────
class TpchQ17TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  const std::vector<TpchQ17LineitemDelta> &LineitemDeltas() const;
  const std::vector<TpchQ17PartDelta> &PartDeltas() const;
  bool HasPartRebuild() const;
  void MarkPartRebuild();
  void AddLineitemDelta(TpchQ17LineitemDelta delta);
  void AddPartDelta(TpchQ17PartDelta delta);

private:
  bool pending = false;
  bool part_rebuild = false;
  std::vector<TpchQ17LineitemDelta> lineitem_deltas;
  std::vector<TpchQ17PartDelta> part_deltas;
};

// ── Module ──────────────────────────────────────────────────────────────────
class TpchQ17Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = TpchQ17Layout::kAccelerator;

  explicit TpchQ17Module(TpchQ17Config config,
                         TpchQ17InitialScan *initial_scan = nullptr)
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
  TpchQ17State BuildState(core::AuxEngineServices &) const;
  void EmitResult(core::AuxEngineServices &, const std::string &brand,
                  const std::string &container, core::AuxRowSink &) const;

  TpchQ17Config config;
  TpchQ17InitialScan *initial_scan = nullptr;
  // Prebuilt read key (no per-read string construction).
  core::QueryKey hit_key{core::AcceleratorId(TpchQ17Layout::kAccelerator), {}};
};

} // namespace accelerator_codegen::aux_state::query
