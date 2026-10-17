#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_query_module.hpp"
#include "prims/any_key_cow_array.hpp"
#include "prims/string_blob.hpp"

namespace accelerator_codegen::aux_state::query {

// ── Q19 layout ──────────────────────────────────────────────────────────────
struct TpchQ19Layout {
  static constexpr const char *kAccelerator = "tpch_q19_aux";
  static constexpr core::PartitionId kAggregatePartition = 0;
};

// State size and costs (one image, copy-on-write):
//  * per-part code: 4 B per part, `(brand id + 1) << 2 | family` (0: the part
//    can never qualify: absent, unknown container family, size outside its
//    family's range). ChunkedCowArray<uint32_t>, 64 Ki-id chunks:
//    80 MB at SF100 (2e7 parts), 800 MB at SF1000 (2e8). This replaces a
//    node-based unordered_map of std::string dims (~100 B/part, +a transient
//    second map in the build: ~4.5 GB at SF100).
//  * aggregates: per (brand, family) a sorted (quantity -> revenue, count)
//    vector, shared copy-on-write: ~25 x 3 x 50 entries x 32 B = ~120 KB at
//    any SF; fractional / off-spec quantities add entries, never drop rows.
// A write costs O(touched chunks + touched vectors). A part insert / delete /
// brand-container-size change re-aggregates that part's qualifying lineitems
// with one indexed point scan (WHERE l_partkey IN (...)) and moves their
// contributions. Execute: 3 binary searches, independent of SF.

// ── Config ──────────────────────────────────────────────────────────────────
struct TpchQ19Config {
  core::TableId lineitem_table = 0;
  core::TableId part_table = 0;
};

// ── Typed scan contract ─────────────────────────────────────────────────────
struct TpchQ19PartInput {
  int64_t key = 0;
  std::string brand;
  std::string container;
  int32_t size = 0;
};

// Qualifying lineitems (AIR / AIR REG, DELIVER IN PERSON) of eligible parts,
// aggregated by (brand, family, quantity) in the engine.
struct TpchQ19GroupAggregate {
  std::string brand;
  int family = 0;           // 1 SM, 2 MED, 3 LG
  int64_t quantity_raw = 0; // scale 2
  __int128 revenue_raw = 0; // sum(price * (1 - discount)), scale 4
  int64_t count = 0;
};

// One part's qualifying lineitems by quantity (point re-aggregation).
struct TpchQ19PartQuantityRevenue {
  int64_t partkey = 0;
  int64_t quantity_raw = 0;
  __int128 revenue_raw = 0;
  int64_t count = 0;
};

class TpchQ19InitialScan {
public:
  virtual ~TpchQ19InitialScan() = default;
  // Split into `partitions` fixed row ranges: sink(partition, row) runs
  // concurrently for different partitions, one thread per partition.
  virtual void ScanParts(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ19PartInput &)> &sink) = 0;
  // lineitem JOIN part, shipping predicate and family classification
  // (TpchQ19FamilySql) applied and grouped in the engine.
  virtual void ScanGroupAggregates(
      const std::function<void(const TpchQ19GroupAggregate &)> &) = 0;
  // lineitem WHERE l_partkey IN (...) AND shipping predicate, grouped by
  // (l_partkey, l_quantity).
  virtual void ScanPartQuantityRevenue(
      const std::vector<int64_t> &partkeys,
      const std::function<void(const TpchQ19PartQuantityRevenue &)> &) = 0;
};

// ── Template constants (the three OR branches of the query) ─────────────────
enum class TpchQ19Family : uint8_t {
  None = 0,
  SM = 1,  // SM CASE, SM BOX, SM PACK, SM PKG ; p_size 1..5
  MED = 2, // MED BAG, MED BOX, MED PKG, MED PACK ; p_size 1..10
  LG = 3,  // LG CASE, LG BOX, LG PACK, LG PKG ; p_size 1..15
};

// SQL `CASE` expression yielding the family number (1..3) of a part, NULL when
// it can never qualify (container family unknown or size out of range): the
// single source of the branch constants for engine-side aggregation.
std::string TpchQ19FamilySql();
// SQL predicate on lineitem columns: l_shipmode IN ('AIR', 'AIR REG') AND
// l_shipinstruct = 'DELIVER IN PERSON'.
std::string TpchQ19ShippingSql();

// ── State ───────────────────────────────────────────────────────────────────
struct TpchQ19State {
  struct QtyRev {
    int64_t qty = 0; // l_quantity raw (scale 2)
    __int128 rev = 0;
    int64_t count = 0;
  };
  using Vec = std::vector<QtyRev>; // sorted by qty

  core::AnyKeyCowArray<uint32_t, 14> part_code{0};
  std::shared_ptr<const core::StringDict> brands =
      std::make_shared<core::StringDict>();
  // (brand id + 1) << 2 | family -> sorted vector; vectors shared COW.
  std::unordered_map<uint32_t, std::shared_ptr<Vec>> aggregates;

  static uint32_t Code(uint32_t brand_id, TpchQ19Family family) {
    return ((brand_id + 1) << 2) | static_cast<uint32_t>(family);
  }
  // Id of a brand string, interned on demand (copies the dictionary once per
  // state copy when a new brand appears).
  uint32_t InternBrand(const std::string &brand);
  // Code of a part with these columns (0: never qualifies).
  uint32_t PartCode(const std::string &brand, const std::string &container,
                    int32_t size);
  void Add(uint32_t code, int64_t qty, __int128 revenue, int64_t count);
  // Sum over quantity in [qty_lo, qty_hi] of the (brand, family) vector.
  void RangeSum(const std::string &brand, TpchQ19Family family,
                __int128 qty_lo, __int128 qty_hi, __int128 &revenue,
                int64_t &count) const;
};

// The whole state is one image (partition 0).
struct TpchQ19StateImage {
  std::shared_ptr<const TpchQ19State> state;
};

// ── Delta ───────────────────────────────────────────────────────────────────
struct TpchQ19LineitemDelta {
  int64_t partkey = 0;
  int64_t quantity_raw = 0;
  __int128 revenue_raw = 0;
  int8_t sign = 0;
};

// A part row appeared or changed (present) or went away.
struct TpchQ19PartEvent {
  bool present = false;
  TpchQ19PartInput part;
  int64_t partkey = 0;
};

// ── TxnBuffer ───────────────────────────────────────────────────────────────
class TpchQ19TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  const std::vector<TpchQ19LineitemDelta> &Deltas() const;
  const std::vector<TpchQ19PartEvent> &PartEvents() const;
  void MarkRebuild();
  void AddDelta(TpchQ19LineitemDelta delta);
  void AddPartEvent(TpchQ19PartEvent event);

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<TpchQ19LineitemDelta> deltas;
  std::vector<TpchQ19PartEvent> part_events;
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
  std::vector<core::CalibrationWrite> CalibrationWrites() const override;

private:
  std::shared_ptr<TpchQ19State> BuildState(core::AuxEngineServices &services) const;
  void Answer(const TpchQ19State &, const core::QueryArguments &,
              core::AuxRowSink &) const;
  void EmitResult(__int128 revenue_raw, int64_t matches,
                  core::AuxRowSink &) const;

  TpchQ19Config config;
  TpchQ19InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
