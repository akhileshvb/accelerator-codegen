#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_query_module.hpp"

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
};

// ── State structures ────────────────────────────────────────────────────────
struct SupplierOrderInfo {
  int64_t total_count = 0;  // number of lineitems for this supplier on this order
  int64_t late_count = 0;   // number of late lineitems (receiptdate > commitdate)
};

struct OrderSummary {
  char orderstatus = 0;
  std::unordered_map<int64_t, SupplierOrderInfo> suppliers;
};

struct TpchQ21State {
  // Supplier metadata: suppkey -> {name, nationkey}
  std::unordered_map<int64_t, std::string> supplier_names;
  std::unordered_map<int64_t, int32_t> supplier_nations;

  // Nation name to nationkey mapping
  std::unordered_map<std::string, std::vector<int32_t>> nation_name_to_keys;

  // Per-order summaries for maintenance. The base map is shared across
  // published images; RF/point writes land in `order_overlay` so ApplyDelta
  // does not copy the SF-scale table.
  using OrderSummaryMap = std::unordered_map<int64_t, OrderSummary>;
  std::shared_ptr<OrderSummaryMap> order_summaries =
      std::make_shared<OrderSummaryMap>();
  OrderSummaryMap order_overlay;

  const OrderSummary *FindOrder(int64_t orderkey) const {
    const auto overlay_it = order_overlay.find(orderkey);
    if (overlay_it != order_overlay.end())
      return &overlay_it->second;
    if (!order_summaries)
      return nullptr;
    const auto it = order_summaries->find(orderkey);
    return it == order_summaries->end() ? nullptr : &it->second;
  }

  OrderSummary &MutableOrder(int64_t orderkey) {
    const auto overlay_it = order_overlay.find(orderkey);
    if (overlay_it != order_overlay.end())
      return overlay_it->second;
    if (order_summaries) {
      const auto it = order_summaries->find(orderkey);
      if (it != order_summaries->end())
        return order_overlay[orderkey] = it->second;
    }
    return order_overlay[orderkey];
  }

  // Per-nation supplier numwait: nationkey -> (suppkey -> numwait)
  std::unordered_map<int32_t, std::unordered_map<int64_t, int64_t>> nation_numwait;
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

struct TpchQ21StateImage {
  TpchQ21ImageKind kind = TpchQ21ImageKind::SupplierMetadata;
  int32_t nationkey = 0;
  // Set only for parameter-keyed read images; guards hash collisions.
  std::string nation_name;
  std::shared_ptr<const TpchQ21State> state;

  // Nation images carry their already-materialized, ordered result.  This
  // keeps the hit path independent of the global supplier metadata map and
  // avoids reconstructing/sorting rows after a partition read.
  std::vector<TpchQ21OutputRow> output_rows;
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
  std::string old_name;
  std::string new_name;
  int32_t old_nationkey = 0;
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

// Exposed for extract-diff / verification (used by module.cpp build/repair).
std::unordered_map<int64_t, int64_t>
TpchQ21ComputeOrderContributions(const OrderSummary &order);

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
  // ReadHit: caller must pass the same VisibilityToken pinned in Execute.
  bool LoadStateForNations(core::AuxEngineServices &services,
                            const core::VisibilityToken &visibility,
                            const std::vector<int32_t> &nationkeys,
                            TpchQ21State &state) const;
  void EmitResult(const TpchQ21State &state,
                  core::AuxRowSink &sink) const;

  TpchQ21Config config;
  TpchQ21InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query