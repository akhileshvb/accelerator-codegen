#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_query_module.hpp"
#include "prims/flat_hash_map.hpp"

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

// A lineitem as a partitioned build scan delivers it: strings are views valid
// during the callback.
struct TpchQ19LineitemRow {
  int64_t partkey = 0;
  int64_t quantity_raw = 0;
  int64_t extendedprice_raw = 0;
  int64_t discount_raw = 0;
  std::string_view shipmode;
  std::string_view shipinstruct;
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

  // Optional SQL pushdown for Build: the filtered GROUP BY
  // (brand, container family, quantity) of qualifying lineitems joined to
  // parts, as (brand, family 1..3, qty_raw, SUM(revenue_raw), COUNT(*)).
  virtual bool ScanQualifyingAggregates(
      const std::function<void(const std::string &, int, int64_t, int64_t,
                               int64_t)> &consumer) {
    (void)consumer;
    return false;
  }

  // Partitioned scans for the full build: rows of one partition arrive in
  // table order on one thread; partitions run concurrently on the pool.
  virtual void ScanPartsPartitioned(
      const core::AuxEngineServices &, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ19PartInput &)> &) = 0;
  virtual void ScanLineitemsPartitioned(
      const core::AuxEngineServices &, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ19LineitemRow &)> &) = 0;

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
  // ── Per-part index of qualifying lineitems ────────────────────────────────
  // For every partkey, the lineitems passing the part-independent filters
  // (shipmode AIR/AIR REG, shipinstruct DELIVER IN PERSON) aggregated by
  // quantity: (qty, SUM(revenue), COUNT). Immutable compact base (sorted by
  // partkey) plus a copy-on-write overlay holding the full current entry
  // list of every part touched since the base.
  struct IndexEnt {
    int64_t qty = 0;
    int64_t rev = 0;
    int64_t count = 0;
  };
  struct IndexEnt32 {
    int64_t rev = 0;
    int32_t count = 0;
    int32_t qty = 0;
  };
  struct IndexBase {
    std::vector<int64_t> keys; // ascending partkeys
    std::vector<uint64_t> off; // keys.size() + 1
    std::vector<IndexEnt32> ents;
  };
  std::shared_ptr<const IndexBase> index_base = std::make_shared<IndexBase>();
  core::ShardedCowHashMap<std::vector<IndexEnt>, 7> index_overlay;
  // Current entries of one part (merged view; empty when none).
  std::vector<IndexEnt> IndexEntries(int64_t partkey) const;

  // ── Part dimension codes ──────────────────────────────────────────────────
  // 0 = no such part, 1 = part exists but can never qualify, otherwise
  // ((brand index + 1) << 2) | family (1..3). Paged and shared between
  // versions; a write clones only the touched page.
  static constexpr std::size_t kDimPage = 4096;
  static constexpr int64_t kDimDenseLimit = int64_t{1} << 30; // SF3000 parts reach 6e8
  struct DimPage {
    std::vector<uint32_t> codes = std::vector<uint32_t>(kDimPage, 0u);
  };
  std::vector<std::shared_ptr<DimPage>> dim_pages; // null page = all absent
  std::shared_ptr<std::unordered_map<int64_t, uint32_t>> dim_sparse =
      std::make_shared<std::unordered_map<int64_t, uint32_t>>();
  std::vector<std::string> brands; // append-only dictionary
  uint32_t DimGet(int64_t partkey) const;
  uint32_t EncodeDim(const std::string &brand, int family);
  void RemoveAggregate(const std::string &brand, TpchQ19Family family,
                       int64_t quantity_coord, int64_t rev, int64_t count);

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
  // brand → [SM=0, MED=1, LG=2] sorted qty→rev vectors, copy-on-write per
  // brand: copying the state shares every brand; a staged write clones only
  // the brands it touches (once per staged state).
  using BrandAgg = std::array<std::vector<QtyRev>, kNumFamilies>;
  class Aggregates {
  public:
    Aggregates() = default;
    Aggregates(const Aggregates &o) : slots_(o.slots_) {
      for (auto &s : slots_)
        s.owned = false;  // a copy owns nothing
    }
    Aggregates &operator=(const Aggregates &o) {
      if (this != &o) {
        slots_ = o.slots_;
        for (auto &s : slots_)
          s.owned = false;
      }
      return *this;
    }
    Aggregates(Aggregates &&) = default;
    Aggregates &operator=(Aggregates &&) = default;
    const BrandAgg *Find(const std::string &brand) const {
      for (const auto &s : slots_)
        if (s.name == brand)
          return s.agg.get();
      return nullptr;
    }
    BrandAgg &Mut(const std::string &brand) {
      for (auto &s : slots_) {
        if (s.name != brand)
          continue;
        if (!s.owned) {
          s.agg = std::make_shared<BrandAgg>(*s.agg);
          s.owned = true;
        }
        return *s.agg;
      }
      slots_.push_back({brand, std::make_shared<BrandAgg>(), true});
      return *slots_.back().agg;
    }
    void Clear() { slots_.clear(); }
    std::size_t size() const { return NonEmptyCount(); }
    // Brands with at least one non-empty family.
    template <class F> void ForEachNonEmpty(F &&f) const {
      for (const auto &s : slots_) {
        bool any = false;
        for (const auto &v : *s.agg)
          any |= !v.empty();
        if (any)
          f(s.name, *s.agg);
      }
    }
    std::size_t NonEmptyCount() const {
      std::size_t n = 0;
      ForEachNonEmpty([&](const std::string &, const BrandAgg &) { ++n; });
      return n;
    }

  private:
    struct Slot {
      std::string name;
      std::shared_ptr<BrandAgg> agg;
      bool owned = false;
    };
    std::vector<Slot> slots_;
  };
  Aggregates aggregates;


  static int FamilyIndex(TpchQ19Family f) {
    return static_cast<int>(f) - 1;  // SM=0, MED=1, LG=2
  }

  void AddContribution(const std::string &brand, TpchQ19Family family,
                       int32_t size, int64_t quantity_coord, int64_t delta);
  void AddAggregate(const std::string &brand, TpchQ19Family family,
                    int64_t quantity_coord, int64_t rev, int64_t count);
  // Sum of revenue and count of qualifying rows with qty in [lo, hi].
  int64_t RangeSumCount(const std::string &brand, TpchQ19Family family,
                        int64_t qty_low, int64_t qty_high,
                        int64_t &count) const;
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
// One qualifying lineitem contribution to add (sign +1) or remove (sign -1),
// or one part-table change (part_op != 0).
struct TpchQ19Delta {
  int64_t partkey = 0;
  int64_t quantity_raw = 0;
  int64_t revenue_raw = 0;
  int8_t sign = 0;
  // 0 = lineitem delta, 1 = part insert, 2 = part delete, 3 = part update
  // (new_* carry the new row for insert / update).
  int8_t part_op = 0;
  std::string new_brand;
  std::string new_container;
  int32_t new_size = 0;
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
  // full = also build the per-part index and dimension codes (published
  // state); otherwise only the aggregates (read-only fallback).
  TpchQ19State BuildState(core::AuxEngineServices &services, bool full) const;
  bool LoadState(core::AuxEngineServices &services,
                  std::shared_ptr<const TpchQ19State> &state) const;
  void EmitResult(const TpchQ19State &state, const std::string &brand1,
                  int64_t qty_low1, const std::string &brand2,
                  int64_t qty_low2, const std::string &brand3,
                  int64_t qty_low3, core::AuxRowSink &sink) const;

  TpchQ19Config config;
  TpchQ19InitialScan *initial_scan = nullptr;

  // Read-path cache: skips the ReadPartitions slot scan while the visible
  // partition version is unchanged. Swapped atomically (concurrent readers).
  struct CachedRead {
    std::shared_ptr<const TpchQ19State> state;
    core::Version version = 0;
  };
  mutable std::shared_ptr<const CachedRead> cached_read_;
};

} // namespace accelerator_codegen::aux_state::query
