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
#include "accelerator_codegen/stateful/core/query_layouts.hpp"

namespace accelerator_codegen::aux_state::query {

// ── Q18 layout (inlined here because query_layouts.hpp is not writable) ─────
struct TpchQ18Layout {
  static constexpr const char *kAccelerator = "tpch_q18_aux";
  static constexpr core::PartitionId kOrderQuantitySums = 0;

  static constexpr std::array<core::PartitionId, 1> Partitions() {
    return {kOrderQuantitySums};
  }
};

// ── Config ──────────────────────────────────────────────────────────────────
struct TpchQ18Config {
  core::TableId lineitem_table = 0;
  core::TableId orders_table = 0;
  core::TableId customer_table = 0;
};

// ── Typed bulk-scan contract ────────────────────────────────────────────────
struct TpchQ18LineitemInput {
  int64_t orderkey = 0;
  int64_t quantity_raw = 0;
};

struct TpchQ18OrderInput {
  int64_t key = 0;
  int64_t custkey = 0;
  int32_t orderdate_days = 0;
  int64_t totalprice_raw = 0;
};

struct TpchQ18CustomerInput {
  int64_t key = 0;
  std::string name;
};

class TpchQ18InitialScan {
public:
  virtual ~TpchQ18InitialScan() = default;
  virtual void ScanLineitems(
      const std::function<void(const TpchQ18LineitemInput &)> &) = 0;
  virtual void ScanOrders(
      const std::function<void(const TpchQ18OrderInput &)> &) = 0;
  virtual void ScanCustomers(
      const std::function<void(const TpchQ18CustomerInput &)> &) = 0;
};

// ── State structures ────────────────────────────────────────────────────────
struct TpchQ18State {
  // Chunked dense orderkey-indexed data: chunk c holds keys [c<<16, (c+1)<<16).
  // Grows one 64K-slot chunk at a time (no SF-hardcoded mega-array).
  static constexpr std::size_t kChunkShift = 16;
  static constexpr std::size_t kChunkSize = 1u << kChunkShift;
  static constexpr std::size_t kChunkMask = kChunkSize - 1;

  // Quantity sums — maintained incrementally across lineitem mutations
  std::vector<std::vector<int64_t>> quantity_chunks;

  // Colocated order metadata — set once during BuildState / rebuild;
  // orders mutations trigger rebuild so these stay consistent.
  std::vector<std::vector<int64_t>> custkey_chunks;
  std::vector<std::vector<int32_t>> orderdate_chunks;
  std::vector<std::vector<int64_t>> totalprice_chunks;

  // Active orderkeys: every orderkey that has both a nonzero quantity sum
  // AND stored metadata.  Built during BuildState / decode; kept stable
  // across incremental lineitem-delta maintenance (rebuild on new keys).
  std::vector<int64_t> active_orderkeys;

  // ── Quantity sum accessors ──────────────────────────────────────────────
  void AddOrderQuantity(int64_t orderkey, int64_t delta);
  void SetOrderQuantitySum(int64_t orderkey, int64_t sum);
  int64_t OrderQuantitySum(int64_t orderkey) const;

  // ── Metadata accessors ──────────────────────────────────────────────────
  void SetOrderMeta(int64_t orderkey, int64_t custkey, int32_t orderdate_days,
                    int64_t totalprice_raw);
  bool HasOrderMeta(int64_t orderkey) const;
  int64_t OrderCustkey(int64_t orderkey) const;
  int32_t OrderOrderdate(int64_t orderkey) const;
  int64_t OrderTotalprice(int64_t orderkey) const;
};

enum class TpchQ18ImageKind : uint8_t { OrderQuantitySums };

struct TpchQ18StateImage {
  TpchQ18ImageKind kind = TpchQ18ImageKind::OrderQuantitySums;
  std::shared_ptr<const TpchQ18State> state;
};

// ── Output row ──────────────────────────────────────────────────────────────
struct TpchQ18OutputRow {
  std::string name;
  int64_t custkey = 0;
  int64_t orderkey = 0;
  int32_t orderdate_days = 0;
  int64_t totalprice_raw = 0;
  int64_t quantity_sum_raw = 0;
};

// ── Delta ───────────────────────────────────────────────────────────────────
struct TpchQ18LineitemDelta {
  int64_t orderkey = 0;
  int64_t quantity_raw = 0;
  int8_t sign = 0;
};

// ── Encode / Decode ─────────────────────────────────────────────────────────
std::vector<uint8_t> EncodeTpchQ18OrderQuantitySums(const TpchQ18State &state);
bool DecodeTpchQ18OrderQuantitySums(const std::vector<uint8_t> &bytes,
                                    TpchQ18State &state);

// ── TxnBuffer ───────────────────────────────────────────────────────────────
class TpchQ18TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  const std::vector<TpchQ18LineitemDelta> &Deltas() const;
  void MarkRebuild();
  void AddDelta(TpchQ18LineitemDelta delta);

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<TpchQ18LineitemDelta> deltas;
};

// ── Module ──────────────────────────────────────────────────────────────────
class TpchQ18Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = TpchQ18Layout::kAccelerator;

  explicit TpchQ18Module(TpchQ18Config config,
                         TpchQ18InitialScan *initial_scan = nullptr)
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

private:
  TpchQ18State BuildState(core::AuxEngineServices &services) const;
  bool LoadState(core::AuxEngineServices &services,
                  std::shared_ptr<const TpchQ18State> &state) const;
  void EmitResult(const TpchQ18State &state, int64_t threshold_raw,
                  core::AuxEngineServices &services,
                  core::AuxRowSink &sink) const;

  TpchQ18Config config;
  TpchQ18InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query