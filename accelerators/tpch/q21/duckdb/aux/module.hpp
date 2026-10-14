#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_query_module.hpp"
#include "prims/flat_hash_map.hpp"

namespace accelerator_codegen::aux_state::query {

// ── Q21 layout ──────────────────────────────────────────────────────────────
struct TpchQ21Layout {
  static constexpr const char *kAccelerator = "tpch_q21_aux";
  static constexpr core::PartitionId kSupplierMetadata = 0;
  static constexpr core::PartitionId kOrderSummaries = 1;
  // Tiny partition holding only nation_name -> nationkeys; decoded on every
  // hit to resolve the query parameter without touching supplier metadata.
  static constexpr core::PartitionId kNationNameIndex = 2;
  static constexpr core::PartitionId kNationBase = 1000;
  // A bounded hash namespace for parameter-keyed, already-finalized results.
  // The actual nation name is retained in the image and verified on lookup.
  static constexpr core::PartitionId kNationNameBase = 2000000;
  static constexpr core::PartitionId kNationNameSlots = 1048576;

  static constexpr std::array<core::PartitionId, 3> FixedPartitions() {
    return {kSupplierMetadata, kOrderSummaries, kNationNameIndex};
  }
};

// ── Config ──────────────────────────────────────────────────────────────────
struct TpchQ21Config {
  core::TableId supplier_table = 0;
  core::TableId orders_table = 0;
  core::TableId lineitem_table = 0;
  core::TableId nation_table = 0;
};

// ── Typed bulk-scan contract ────────────────────────────────────────────────
struct TpchQ21SupplierInput {
  int64_t suppkey = 0;
  std::string name;
  int32_t nationkey = 0;
};

struct TpchQ21NationInput {
  int32_t nationkey = 0;
  std::string name;
};

struct TpchQ21OrderInput {
  int64_t orderkey = 0;
  char orderstatus = 0;
};

struct TpchQ21LineitemInput {
  int64_t orderkey = 0;
  int64_t suppkey = 0;
  int32_t commitdate = 0;
  int32_t receiptdate = 0;
};

class TpchQ21InitialScan {
public:
  virtual ~TpchQ21InitialScan() = default;
  virtual void ScanSuppliers(
      const std::function<void(const TpchQ21SupplierInput &)> &) = 0;
  virtual void ScanNations(
      const std::function<void(const TpchQ21NationInput &)> &) = 0;
  virtual void ScanOrders(
      const std::function<void(const TpchQ21OrderInput &)> &) = 0;
  virtual void ScanLineitems(
      const std::function<void(const TpchQ21LineitemInput &)> &) = 0;

  // Optional SQL pushdown for Build: for every fulfilled ('F') order that can
  // contribute (>= 2 suppliers, exactly one supplier with a late item), one
  // call per (orderkey, suppkey) with COUNT(*) and the late-item count.
  // Point lookup of one order's current (including the open transaction)
  // status and per-supplier (count, late count). Returns false when
  // unsupported; `found` is false when the order row does not exist.
  struct OrderLookup {
    bool found = false;
    char status = 0;
    std::vector<std::array<int64_t, 3>> suppliers; // suppkey, total, late
  };
  virtual bool LookupOrderSummary(int64_t orderkey, OrderLookup &out) {
    (void)orderkey;
    (void)out;
    return false;
  }

  virtual bool ScanContributingOrderSuppliers(
      const std::function<void(int64_t orderkey, int64_t suppkey,
                               int64_t total, int64_t late)> &consumer) {
    (void)consumer;
    return false;
  }
};

// ── State structures ────────────────────────────────────────────────────────
struct SupplierOrderInfo {
  int64_t suppkey = 0;
  int64_t total_count = 0; // lineitems of this supplier on this order
  int64_t late_count = 0;  // late ones (receiptdate > commitdate)
};

struct OrderSummary {
  char orderstatus = 0;
  // Orders have a handful of lineitems: a linear small vector beats a map.
  std::vector<SupplierOrderInfo> suppliers;

  SupplierOrderInfo *FindSupplier(int64_t suppkey) {
    for (auto &s : suppliers)
      if (s.suppkey == suppkey)
        return &s;
    return nullptr;
  }
};

struct TpchQ21Supplier {
  std::shared_ptr<const std::string> name;
  int32_t nationkey = 0;
};

struct TpchQ21SupplierOverlay {
  bool present = false;
  TpchQ21Supplier supplier;
};

// Supplier directory: shared immutable base + small copy-on-write overlay.
struct TpchQ21Dir {
  std::shared_ptr<const std::unordered_map<int64_t, TpchQ21Supplier>> base;
  core::ShardedCowHashMap<TpchQ21SupplierOverlay, 7> overlay;

  const TpchQ21Supplier *Find(int64_t suppkey) const {
    if (const auto *ov = overlay.Find(suppkey))
      return ov->present ? &ov->supplier : nullptr;
    if (!base)
      return nullptr;
    const auto it = base->find(suppkey);
    return it == base->end() ? nullptr : &it->second;
  }
};

// Order summaries (only orders that can contribute / were touched) and the
// per-supplier numwait totals. Base + copy-on-write overlay.
struct TpchQ21Core {
  using OrderMap = std::unordered_map<int64_t, OrderSummary>;
  std::shared_ptr<const OrderMap> base;
  core::ShardedCowHashMap<OrderSummary, 7> overlay;
  // suppkey -> numwait (independent of whether the supplier row exists).
  core::ShardedCowHashMap<int64_t, 7> numwait;

  const OrderSummary *FindOrder(int64_t orderkey) const {
    if (const auto *ov = overlay.Find(orderkey))
      return ov;
    if (!base)
      return nullptr;
    const auto it = base->find(orderkey);
    return it == base->end() ? nullptr : &it->second;
  }
  // Staging owner only: copy the base summary into the overlay on first touch.
  OrderSummary &MutableOrder(int64_t orderkey) {
    const bool inserted = overlay.Find(orderkey) == nullptr;
    auto &slot = overlay[orderkey];
    if (inserted && base) {
      const auto it = base->find(orderkey);
      if (it != base->end())
        slot = it->second;
    }
    return slot;
  }
};

struct TpchQ21NationIndex {
  std::unordered_map<std::string, std::vector<int32_t>> nation_name_to_keys;
};

// Finalized answer rows of one nation, ordered (numwait DESC, name ASC) and
// merged by s_name. Names are shared with the supplier directory.
struct TpchQ21ImageRow {
  std::shared_ptr<const std::string> name;
  int64_t numwait = 0;
};
struct TpchQ21NationRows {
  std::vector<TpchQ21ImageRow> rows;
};

// Logical whole state (Build result, Encode/Decode, fallback).
struct TpchQ21State {
  std::shared_ptr<TpchQ21Dir> dir = std::make_shared<TpchQ21Dir>();
  std::shared_ptr<TpchQ21Core> core = std::make_shared<TpchQ21Core>();
  std::shared_ptr<TpchQ21NationIndex> nation_index =
      std::make_shared<TpchQ21NationIndex>();
  std::map<int32_t, std::shared_ptr<const TpchQ21NationRows>> nations;
};

// ── Output row ──────────────────────────────────────────────────────────────
struct TpchQ21OutputRow {
  std::string name;
  int64_t numwait = 0;
};

enum class TpchQ21ImageKind : uint8_t {
  SupplierMetadata,
  OrderSummaries,
  NationNameIndex,
  Nation
};

// Each image owns only the piece it publishes (never the whole state).
struct TpchQ21StateImage {
  TpchQ21ImageKind kind = TpchQ21ImageKind::SupplierMetadata;
  int32_t nationkey = 0;
  // Set only for parameter-keyed read images; guards hash collisions.
  std::string nation_name;
  std::shared_ptr<const TpchQ21Dir> dir;                // SupplierMetadata
  std::shared_ptr<const TpchQ21Core> core;              // OrderSummaries
  std::shared_ptr<const TpchQ21NationIndex> nation_index; // NationNameIndex
  std::shared_ptr<const TpchQ21NationRows> rows;        // Nation
};

// ── Delta ───────────────────────────────────────────────────────────────────
struct TpchQ21LineitemDelta {
  int64_t orderkey = 0;
  int64_t suppkey = 0;
  int32_t commitdate = 0;
  int32_t receiptdate = 0;
  int8_t sign = 0;
};

struct TpchQ21OrderDelta {
  int64_t orderkey = 0;
  char old_orderstatus = 0;
  char new_orderstatus = 0;
};

struct TpchQ21SupplierDelta {
  int64_t suppkey = 0;
  bool had_old = false; // supplier row removed / replaced
  bool has_new = false; // supplier row inserted / replacing
  std::string new_name;
  int32_t new_nationkey = 0;
};

struct TpchQ21NationDelta {
  int32_t nationkey = 0;
  std::string old_name;
  std::string new_name;
};

// ── Encode / Decode ─────────────────────────────────────────────────────────
std::vector<uint8_t> EncodeTpchQ21SupplierMetadata(const TpchQ21State &state);
std::vector<uint8_t> EncodeTpchQ21OrderSummaries(const TpchQ21State &state);
std::vector<uint8_t> EncodeTpchQ21NationNameIndex(const TpchQ21State &state);
std::vector<uint8_t> EncodeTpchQ21Nation(int32_t nationkey,
                                          const TpchQ21State &state);
bool DecodeTpchQ21SupplierMetadata(const std::vector<uint8_t> &bytes,
                                    TpchQ21State &state);
bool DecodeTpchQ21OrderSummaries(const std::vector<uint8_t> &bytes,
                                  TpchQ21State &state);
bool DecodeTpchQ21NationNameIndex(const std::vector<uint8_t> &bytes,
                                   TpchQ21State &state);
bool DecodeTpchQ21Nation(const std::vector<uint8_t> &bytes, int32_t nationkey,
                          TpchQ21State &state);

// ── TxnBuffer ───────────────────────────────────────────────────────────────
class TpchQ21TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  const std::vector<TpchQ21LineitemDelta> &LineitemDeltas() const;
  const std::vector<TpchQ21OrderDelta> &OrderDeltas() const;
  const std::vector<TpchQ21SupplierDelta> &SupplierDeltas() const;
  const std::vector<TpchQ21NationDelta> &NationDeltas() const;
  void MarkRebuild();
  void AddLineitemDelta(TpchQ21LineitemDelta delta);
  void AddOrderDelta(TpchQ21OrderDelta delta);
  void AddSupplierDelta(TpchQ21SupplierDelta delta);
  void AddNationDelta(TpchQ21NationDelta delta);

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<TpchQ21LineitemDelta> lineitem_deltas;
  std::vector<TpchQ21OrderDelta> order_deltas;
  std::vector<TpchQ21SupplierDelta> supplier_deltas;
  std::vector<TpchQ21NationDelta> nation_deltas;
};

// ── Module ──────────────────────────────────────────────────────────────────
class TpchQ21Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = TpchQ21Layout::kAccelerator;

  explicit TpchQ21Module(TpchQ21Config config,
                          TpchQ21InitialScan *initial_scan = nullptr)
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
  TpchQ21State BuildState(core::AuxEngineServices &services) const;
  bool LoadNationRows(
      core::AuxEngineServices &services, const std::vector<int32_t> &nationkeys,
      std::vector<std::shared_ptr<const TpchQ21NationRows>> &out) const;
  void EmitMerged(
      const std::vector<std::shared_ptr<const TpchQ21NationRows>> &nations,
      core::AuxRowSink &sink) const;

  TpchQ21Config config;
  TpchQ21InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query