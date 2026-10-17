#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_query_module.hpp"
#include "prims/chunked_cow_array.hpp"
#include "prims/any_key_cow_array.hpp"
#include "prims/cow_sorted_blocks.hpp"
#include "prims/flat_hash_map.hpp"
#include "prims/string_blob.hpp"

namespace accelerator_codegen::aux_state::query {

// ── Q16 layout ──────────────────────────────────────────────────────────────
struct TpchQ16Layout {
  static constexpr const char *kAccelerator = "tpch_q16_aux";
  static constexpr core::PartitionId kState = 0;
};

// ── Config ──────────────────────────────────────────────────────────────────
struct TpchQ16Config {
  core::TableId part_table = 0;
  core::TableId partsupp_table = 0;
  core::TableId supplier_table = 0;
};

// ── Typed bulk-scan contract ────────────────────────────────────────────────
struct TpchQ16SupplierInput {
  int64_t suppkey = 0;
  std::string comment;
};

struct TpchQ16PartInput {
  int64_t partkey = 0;
  std::string brand;
  std::string type;
  int32_t size = 0;
};

struct TpchQ16PartsuppInput {
  int64_t partkey = 0;
  int64_t suppkey = 0;
};

class TpchQ16InitialScan {
public:
  virtual ~TpchQ16InitialScan() = default;
  virtual void ScanSuppliers(
      const std::function<void(const TpchQ16SupplierInput &)> &) = 0;
  // Every row, split into `partitions` fixed row ranges: sink(partition, row)
  // runs concurrently for different partitions, one thread per partition.
  virtual void ScanPartsPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ16PartInput &)> &sink) = 0;
  virtual void ScanPartsuppPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ16PartsuppInput &)> &sink) = 0;
};

// ---------------------------------------------------------------------------
// State layout
//
// Q16 counts DISTINCT non-complaint suppliers per (p_brand, p_type, p_size)
// group.  Instead of per-(group, supplier) reference counts (a hash entry per
// partsupp row) the state keeps the facts and one number per group:
//   * part -> group id (4 B/part), groups = dictionary-encoded
//     (brand id, type id, size) triples (a size is any int32: no 1..50 limit);
//   * the suppliers of each part (partkey-indexed, up to 4 inline = 40 B/part
//     ~ 10 B/row, the rest in an overflow map);
//   * per group the sorted member parts (8 B/part, core::CowSortedBlocks) and
//     the distinct eligible supplier count;
//   * complaint[suppkey]: the supplier row exists and its comment LIKE
//     '%Customer%Complaints%' (a supplier without a row is not excluded).
// A write batch applies its point edits to the facts and then recomputes the
// count of only the groups it touched (one pass over that group's member parts,
// O(group size x 4)); a supplier whose complaint flag flips finds its groups by
// one in-memory pass over the supplier lists (no table scan, no rebuild).
// Execute walks only the groups whose p_size is in the IN list (bitmap of the
// NOT LIKE verdict per distinct type), so it is independent of the table sizes.
//
// Memory: ~55 B/part (SF100 20M parts: ~1.1 GB, SF1000: ~11 GB; was
// ~12-15 GB at SF100).  Any int64 key works (AnyKeyCowArray).
// ---------------------------------------------------------------------------
constexpr uint32_t kTpchQ16NoGroup = 0xFFFFFFFFu;
constexpr std::size_t kTpchQ16InlineSuppliers = 4;

struct TpchQ16Suppliers {
  int64_t suppkey[kTpchQ16InlineSuppliers] = {0, 0, 0, 0};
  uint32_t count = 0;
};

struct TpchQ16GroupKey {
  uint32_t brand_id = 0;
  uint32_t type_id = 0;
  int32_t size = 0;
  bool operator==(const TpchQ16GroupKey &o) const {
    return brand_id == o.brand_id && type_id == o.type_id && size == o.size;
  }
};
struct TpchQ16GroupKeyHash {
  std::size_t operator()(const TpchQ16GroupKey &k) const {
    uint64_t h = (static_cast<uint64_t>(k.brand_id) << 32) | k.type_id;
    h = (h ^ static_cast<uint64_t>(static_cast<uint32_t>(k.size))) *
        0x9E3779B97F4A7C15ULL;
    return static_cast<std::size_t>(h ^ (h >> 29));
  }
};

// Append-only table of the groups seen so far (cloned when a new group appears).
struct TpchQ16GroupTable {
  std::vector<TpchQ16GroupKey> keys; // by group id
  std::unordered_map<TpchQ16GroupKey, uint32_t, TpchQ16GroupKeyHash> ids;
  std::unordered_map<int32_t, std::vector<uint32_t>> by_size;
};

using TpchQ16Members = core::CowSortedBlocks<int64_t, std::less<int64_t>, 1024>;

struct TpchQ16State {
  std::shared_ptr<core::StringDict> brands = std::make_shared<core::StringDict>();
  std::shared_ptr<core::StringDict> types = std::make_shared<core::StringDict>();
  std::shared_ptr<TpchQ16GroupTable> group_table =
      std::make_shared<TpchQ16GroupTable>();
  core::AnyKeyCowArray<uint32_t> part_group{kTpchQ16NoGroup};
  core::AnyKeyCowArray<TpchQ16Suppliers, 10> suppliers_of;
  core::ShardedCowHashMap<std::vector<int64_t>> supplier_overflow;
  core::AnyKeyCowArray<uint8_t> complaint;
  core::ChunkedCowArray<std::shared_ptr<TpchQ16Members>> members;
  core::ChunkedCowArray<int64_t> distinct;

  int64_t SupplierAt(const TpchQ16Suppliers &slots, int64_t partkey,
                     uint32_t i) const;

  // ── point edits (Build and incremental maintenance) ──
  // Each edit records the groups whose count may change in `dirty`.
  void SetPart(int64_t partkey, const TpchQ16PartInput &part);
  void ErasePart(int64_t partkey);
  void AddSupplier(int64_t partkey, int64_t suppkey);
  bool RemoveSupplier(int64_t partkey, int64_t suppkey); // false: not present
  // Returns true if the flag changed.
  bool SetComplaint(int64_t suppkey, bool complaint);
  // Groups containing any part that lists one of `suppliers` become dirty.
  void MarkGroupsOfSuppliers(const core::AuxEngineServices &services,
                             const core::FlatHashMap<uint8_t> &suppliers);
  // Recompute `distinct` of every dirty group.
  void RecomputeDirty(const core::AuxEngineServices &services);
  // Recompute every group (after a bulk load).
  void RecomputeAll(const core::AuxEngineServices &services);

  std::size_t MemoryBytes() const;

  std::vector<uint32_t> dirty;
  bool track_dirty = true; // off during a bulk build (RecomputeAll follows)

private:
  uint32_t GroupId(const TpchQ16PartInput &part);
  int64_t CountDistinct(uint32_t gid) const;
  std::vector<int64_t> CountDistinctOf(
      const core::AuxEngineServices &services, std::size_t n,
      const std::function<uint32_t(std::size_t)> &group_at) const;
  void MarkDirty(uint32_t gid) {
    if (track_dirty)
      dirty.push_back(gid);
  }
  void RemoveFromGroup(int64_t partkey, uint32_t gid);
  void AddToGroup(int64_t partkey, uint32_t gid);
};

struct TpchQ16StateImage {
  std::shared_ptr<const TpchQ16State> state;
};

// ── Query arguments ─────────────────────────────────────────────────────────
// std::nullopt = SQL NULL.  `sizes` are the IN-list items (NULL items drop out).
struct TpchQ16Args {
  std::optional<std::string> brand;
  std::optional<std::string> type_pattern;
  std::vector<int64_t> sizes;
};

void TpchQ16FinalizeAndEmit(const TpchQ16State &state, const TpchQ16Args &args,
                            core::AuxRowSink &sink);

// ── Captured row changes ────────────────────────────────────────────────────
struct TpchQ16PartEvent {
  int8_t sign = 0;
  TpchQ16PartInput row;
};
struct TpchQ16PartsuppEvent {
  int8_t sign = 0;
  TpchQ16PartsuppInput row;
};
struct TpchQ16SupplierEvent {
  int8_t sign = 0;
  int64_t suppkey = 0;
  bool complaint = false; // meaningful for sign > 0 (new image)
};

// ── TxnBuffer ───────────────────────────────────────────────────────────────
class TpchQ16TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;
  bool RequiresRebuild() const { return requires_rebuild; }
  void MarkRebuild() { requires_rebuild = true; }
  uint64_t EventCount() const {
    return parts.size() + partsupps.size() + suppliers.size();
  }

  std::vector<TpchQ16PartEvent> parts;
  std::vector<TpchQ16PartsuppEvent> partsupps;
  std::vector<TpchQ16SupplierEvent> suppliers;

private:
  bool requires_rebuild = false;
};

// ── Module ──────────────────────────────────────────────────────────────────
class TpchQ16Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = TpchQ16Layout::kAccelerator;

  explicit TpchQ16Module(TpchQ16Config config,
                         TpchQ16InitialScan *initial_scan = nullptr)
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
  TpchQ16State BuildState(core::AuxEngineServices &) const;
  std::shared_ptr<const TpchQ16State>
  LoadState(core::AuxEngineServices &) const;
  void EmitResult(const TpchQ16State &, const core::QueryArguments &,
                  core::AuxRowSink &) const;

  TpchQ16Config config;
  TpchQ16InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
