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

// ── Q19 layout (inlined here because query_layouts.hpp is not writable) ─────
struct TpchQ19Layout {
  static constexpr const char *kAccelerator = "tpch_q19_aux";
  static constexpr core::PartitionId kAggregatePartition = 0;
};

// ── Config ──────────────────────────────────────────────────────────────────
struct TpchQ19Config {
  core::TableId lineitem_table = 0;
  core::TableId part_table = 0;
};

// ── Typed bulk-scan contract ────────────────────────────────────────────────
struct TpchQ19PartInput {
  int64_t key = 0;
  std::string brand;
  std::string container;
  int32_t size = 0;
};

struct TpchQ19LineitemInput {
  int64_t partkey = 0;
  int64_t quantity_raw = 0;
  int64_t extendedprice_raw = 0;
  int64_t discount_raw = 0;
  std::string shipmode;
  std::string shipinstruct;
};

class TpchQ19InitialScan {
public:
  virtual ~TpchQ19InitialScan() = default;
  virtual void ScanParts(
      const std::function<void(const TpchQ19PartInput &)> &) = 0;
  virtual void ScanLineitems(
      const std::function<void(const TpchQ19LineitemInput &)> &) = 0;

  // Point lookup for a part image when a lineitem write needs dimension data
  // that is not cached.
  virtual bool LookupPart(int64_t partkey, TpchQ19PartInput &out) {
    (void)partkey;
    (void)out;
    return false;
  }
};

// ── State structures ────────────────────────────────────────────────────────
enum class TpchQ19Family : uint8_t {
  None = 0,
  SM = 1,   // SM CASE, SM BOX, SM PACK, SM PKG
  MED = 2,  // MED BAG, MED BOX, MED PKG, MED PACK
  LG = 3,   // LG CASE, LG BOX, LG PACK, LG PKG
};


struct TpchQ19PartDim {
  std::string brand;
  std::string container;
  int32_t size = 0;
  TpchQ19Family family = TpchQ19Family::None;
};


struct TpchQ19State {
  // Flat per-family quantity→revenue vectors: brand → family_index → sorted
  // vector of (qty, rev). Since RangeSum always queries the full size range
  // for each family (SM 1-5, MED 1-10, LG 1-15), the size dimension is
  // pre-aggregated.  Vectors are kept sorted by qty for binary-search range
  // queries; small enough that insert/erase shift is cheap (<10 entries).
  struct QtyRev {
    int64_t qty = 0;
    int64_t rev = 0;
    int64_t count = 0; // qualifying rows (SUM over none is NULL)

    bool operator<(const QtyRev &o) const { return qty < o.qty; }
    bool operator<(int64_t q) const { return qty < q; }
  };
  static constexpr int kNumFamilies = 3;
  // brand → [SM=0, MED=1, LG=2] sorted qty→rev vectors
  std::unordered_map<std::string, std::array<std::vector<QtyRev>, kNumFamilies>> aggregates;


  static int FamilyIndex(TpchQ19Family f) {
    return static_cast<int>(f) - 1;  // SM=0, MED=1, LG=2
  }

  void AddContribution(const std::string &brand, TpchQ19Family family,
                       int32_t size, int64_t quantity_coord, int64_t delta);
  int64_t RangeSum(const std::string &brand, TpchQ19Family family,
                   int32_t size_low, int32_t size_high, int64_t qty_low,
                   int64_t qty_high) const;
  int64_t RangeCount(const std::string &brand, TpchQ19Family family,
                     int64_t qty_low, int64_t qty_high) const;
  void RemoveContribution(const std::string &brand, TpchQ19Family family,
                          int32_t size, int64_t quantity_coord,
                          int64_t delta);
};

enum class TpchQ19ImageKind : uint8_t { Aggregate };

struct TpchQ19StateImage {
  TpchQ19ImageKind kind = TpchQ19ImageKind::Aggregate;
  std::shared_ptr<const TpchQ19State> state;
};

// ── Output row ──────────────────────────────────────────────────────────────
struct TpchQ19OutputRow {
  int64_t revenue_raw = 0;
};

int64_t TpchQ19RevenueRaw(int64_t extendedprice_raw, int64_t discount_raw);

// ── Delta ───────────────────────────────────────────────────────────────────
struct TpchQ19Delta {
  bool lineitem = false;
  bool part = false;
  int64_t partkey = 0;
  int64_t lineitem_id = 0;
  TpchQ19PartInput old_part;
  TpchQ19PartInput new_part;
  TpchQ19LineitemInput old_line;
  TpchQ19LineitemInput new_line;
  bool old_line_valid = false;
  bool new_line_valid = false;
  bool part_update = false;
  bool lineitem_update = false;
};

// ── Encode / Decode ─────────────────────────────────────────────────────────
std::vector<uint8_t> EncodeTpchQ19Aggregate(const TpchQ19State &state);
bool DecodeTpchQ19Aggregate(const std::vector<uint8_t> &bytes,
                            TpchQ19State &state);

// ── TxnBuffer ───────────────────────────────────────────────────────────────
class TpchQ19TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  const std::vector<TpchQ19Delta> &Deltas() const;
  void MarkRebuild();
  void AddDelta(TpchQ19Delta delta);

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<TpchQ19Delta> deltas;
};

// ── Module ──────────────────────────────────────────────────────────────────
class TpchQ19Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = TpchQ19Layout::kAccelerator;

  explicit TpchQ19Module(TpchQ19Config config,
                         TpchQ19InitialScan *initial_scan = nullptr)
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
  TpchQ19State BuildState(core::AuxEngineServices &services) const;
  bool LoadState(core::AuxEngineServices &services,
                  std::shared_ptr<const TpchQ19State> &state) const;
  void EmitResult(const TpchQ19State &state, const std::string &brand1,
                  int64_t qty_low1, const std::string &brand2,
                  int64_t qty_low2, const std::string &brand3,
                  int64_t qty_low3, core::AuxRowSink &sink) const;

  TpchQ19Config config;
  TpchQ19InitialScan *initial_scan = nullptr;
  mutable std::unordered_map<int64_t, TpchQ19PartDim> part_dim_cache;
  // Mutable active state for zero-copy incremental maintenance.
  // Published as const shared_ptr after each mutation.
  mutable std::shared_ptr<TpchQ19State> active_state_;
  // Read-path state cache: skips LoadState when the partition version
  // has not changed. CheckValidity is still called per query (correctness),
  // but its visible_version bumps on every publish, so the cache is invalid.
  mutable std::shared_ptr<const TpchQ19State> cached_state_;
  mutable core::Version cached_partition_version_ = 0;
};

} // namespace accelerator_codegen::aux_state::query
