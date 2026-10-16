#pragma once

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <string_view>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_query_module.hpp"
#include "prims/flat_hash_map.hpp"

namespace accelerator_codegen::aux_state::query {

// ── Q20 layout ──────────────────────────────────────────────────────────────
struct TpchQ20Layout {
  static constexpr const char *kAccelerator = "tpch_q20_aux";
  static constexpr core::PartitionId kFullState = 0;

  static constexpr std::array<core::PartitionId, 1> Partitions() {
    return {kFullState};
  }
};

struct TpchQ20Config {
  core::TableId lineitem_table = 0;
  core::TableId part_table = 0;
  core::TableId partsupp_table = 0;
  core::TableId supplier_table = 0;
  core::TableId nation_table = 0;
};

// ── Typed bulk-scan contract ───────────────────────────────────────────────
struct TpchQ20LineitemInput {
  int32_t shipdate_days = 0;
  int64_t partkey = 0;
  int64_t suppkey = 0;
  int64_t quantity_raw = 0;
};

struct TpchQ20PartsuppInput {
  int64_t partkey = 0;
  int64_t suppkey = 0;
  int64_t availqty = 0;
};

struct TpchQ20SupplierInput {
  int64_t key = 0;
  std::string name;
  std::string address;
  int32_t nationkey = 0;
};

struct TpchQ20NationInput {
  int32_t key = 0;
  std::string name;
};

class TpchQ20InitialScan {
public:
  virtual ~TpchQ20InitialScan() = default;
  virtual void ScanNations(
      const std::function<void(const TpchQ20NationInput &)> &) = 0;
  // Partitioned scans for builds: rows of one partition arrive in table order
  // on one thread; partitions run concurrently on the pool.
  virtual void ScanLineitems(
      const core::AuxEngineServices &, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ20LineitemInput &)> &) = 0;
  virtual void ScanPartNames(
      const core::AuxEngineServices &, std::size_t partitions,
      const std::function<void(std::size_t, int64_t partkey, std::string_view name)> &) = 0;
  virtual void ScanPartsupps(
      const core::AuxEngineServices &, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ20PartsuppInput &)> &) = 0;
  virtual void ScanSuppliers(
      const core::AuxEngineServices &, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ20SupplierInput &)> &) = 0;
};

// ── State structures ────────────────────────────────────────────────────────
struct TpchQ20DateQuantity {
  int32_t shipdate_days = 0;
  int64_t quantity_raw = 0;
};

struct TpchQ20SupplierRecord {
  std::string name;
  std::string address;
  int32_t nationkey = 0;
};

// Immutable bulk image built once per (re)build; shared by every version.
struct TpchQ20Base {
  struct Entry {
    int32_t date = 0;
    int32_t qty = 0;
  };
  // One partsupp row in the nation-major pair index.
  struct PairEntry {
    uint32_t rank = 0; // the part's index in the sorted name arrays
    int32_t suppkey = 0;
    uint32_t series_idx = kNoSeries;
    int32_t availqty = 0; // kWideAvail: the value is in avail_wide
  };
  // A partsupp row of a part with no base name (no part row at build time).
  struct OrphanPair {
    int64_t partkey = 0;
    int64_t availqty = 0;
    int32_t suppkey = 0;
    uint32_t series_idx = kNoSeries;
  };
  static constexpr uint32_t kNoSeries = 0xFFFFFFFFu;
  static constexpr uint32_t kNoRank = 0xFFFFFFFFu;
  static constexpr uint32_t kNoSlot = 0xFFFFFFFFu;
  static constexpr int32_t kWideAvail = INT32_MIN;

  // (partkey << 20 | suppkey) sorted; CSR over series_entries.
  std::vector<uint64_t> series_key;
  std::vector<uint64_t> series_off; // series_key.size() + 1
  std::vector<Entry> series_entries;
  // Pairs whose quantities do not fit Entry::qty (idx -> entries).
  std::unordered_map<uint32_t, std::vector<TpchQ20DateQuantity>> series_wide;

  // Partsupp pairs, nation-major: sorted by (slot of the supplier's nation,
  // part-name rank, suppkey), CSR per slot.  A read walks one name-rank range
  // per target nation, i.e. only pairs passing both the part-name and the
  // nation filter, sequentially.  The last slot holds pairs whose supplier has
  // no base row.  A part with several base names has its pairs once per name.
  std::vector<PairEntry> pairs;
  std::vector<uint32_t> slot_off; // slots + 1
  std::unordered_map<int32_t, uint32_t> nation_slot; // base nationkey -> slot
  std::unordered_map<uint64_t, int64_t> avail_wide;  // pair key -> availqty
  std::vector<OrphanPair> orphan_pairs; // sorted by (partkey, suppkey)

  // part names sorted bytewise; parallel partkey array.
  std::string name_arena;
  std::vector<uint64_t> name_off; // n + 1
  std::vector<int64_t> name_partkey;
  std::vector<uint32_t> part_rank; // dense by partkey: first rank, or kNoRank
  std::unordered_map<int64_t, uint32_t> sparse_rank; // keys past part_rank
  // Further ranks of partkeys with several base names (normally empty).
  std::unordered_map<int64_t, std::vector<uint32_t>> extra_ranks;

  // suppliers: records sorted by (name, suppkey), so a supplier's index is
  // its name rank and output in name order reads them in memory order.
  std::vector<TpchQ20SupplierRecord> supplier_records;
  std::vector<int64_t> supplier_keys; // parallel to supplier_records
  std::vector<uint32_t> supp_rank;    // dense by suppkey: rank, or kNoRank
  std::unordered_map<int64_t, uint32_t> sparse_supp_rank; // keys past supp_rank
  std::vector<int32_t> supp_nation; // dense by suppkey, -1 = absent

  // nations
  std::unordered_map<int32_t, std::string> nations;
  std::unordered_map<std::string, std::vector<int32_t>> nation_name_index;

  uint32_t FindSeries(uint64_t pair_key) const; // idx or kNoSeries
  // Sum of quantities with shipdate in [lo, hi); returns whether any entry.
  bool WindowSum(uint32_t series_idx, int32_t lo, int32_t hi,
                 int64_t &sum) const;
  std::string_view PartName(std::size_t i) const {
    return std::string_view(name_arena.data() + name_off[i],
                            name_off[i + 1] - name_off[i]);
  }
  uint32_t RankOf(int64_t partkey) const {
    if (partkey >= 0 && static_cast<std::size_t>(partkey) < part_rank.size())
      return part_rank[static_cast<std::size_t>(partkey)];
    if (sparse_rank.empty())
      return kNoRank;
    const auto it = sparse_rank.find(partkey);
    return it == sparse_rank.end() ? kNoRank : it->second;
  }
  std::size_t SlotCount() const {
    return slot_off.empty() ? 0 : slot_off.size() - 1;
  }
  uint32_t NoNationSlot() const {
    return static_cast<uint32_t>(SlotCount()) - 1;
  }
  uint32_t SlotOfNation(int32_t nationkey) const {
    const auto it = nation_slot.find(nationkey);
    return it == nation_slot.end() ? kNoSlot : it->second;
  }
  // The supplier's nation in the base image (-1 when it has no base row).
  int32_t BaseNation(int64_t suppkey) const;
  uint32_t SupplierRank(int64_t suppkey) const {
    if (suppkey >= 0 && static_cast<std::size_t>(suppkey) < supp_rank.size())
      return supp_rank[static_cast<std::size_t>(suppkey)];
    if (sparse_supp_rank.empty())
      return kNoRank;
    const auto it = sparse_supp_rank.find(suppkey);
    return it == sparse_supp_rank.end() ? kNoRank : it->second;
  }
  const TpchQ20SupplierRecord *BaseSupplier(int64_t suppkey) const {
    const uint32_t rank = SupplierRank(suppkey);
    return rank == kNoRank ? nullptr : &supplier_records[rank];
  }
  // [begin, end) of the pairs of `slot` with rank in [rank_lo, rank_hi).
  std::pair<std::size_t, std::size_t> RankRange(uint32_t slot, uint32_t rank_lo,
                                                uint32_t rank_hi) const;
  int64_t Avail(const PairEntry &e) const;
  // Every base partsupp row of `partkey`, once: f(suppkey, availqty, series_idx).
  template <typename F> void ForEachBasePair(int64_t partkey, F &&f) const {
    const uint32_t rank = RankOf(partkey);
    if (rank == kNoRank) {
      auto it = std::lower_bound(
          orphan_pairs.begin(), orphan_pairs.end(), partkey,
          [](const OrphanPair &p, int64_t k) { return p.partkey < k; });
      for (; it != orphan_pairs.end() && it->partkey == partkey; ++it)
        f(static_cast<int64_t>(it->suppkey), it->availqty, it->series_idx);
      return;
    }
    for (uint32_t slot = 0; slot < SlotCount(); ++slot) {
      const auto [b, e] = RankRange(slot, rank, rank + 1);
      for (std::size_t i = b; i < e; ++i)
        f(static_cast<int64_t>(pairs[i].suppkey), Avail(pairs[i]),
          pairs[i].series_idx);
    }
  }
};

struct TpchQ20PsPair {
  int64_t suppkey = 0;
  int64_t availqty = 0;
};

struct TpchQ20PartOverlay {
  bool present = false;
  std::string name;
};

struct TpchQ20SupplierOverlay {
  bool present = false;
  TpchQ20SupplierRecord record;
};

// A pair's series as seen through the overlay layers.
struct TpchQ20SeriesView {
  const TpchQ20DateQuantity *data = nullptr;
  std::size_t n = 0;
  bool found = false; // false: the overlay has no entry (look in the base)
  const TpchQ20DateQuantity *begin() const { return data; }
  const TpchQ20DateQuantity *end() const { return data + n; }
};

// One immutable overlay layer: pair_key -> complete series (possibly empty,
// which shadows the base), an open-addressing table over flat storage.
class TpchQ20SeriesLayer {
public:
  class Builder {
  public:
    void Add(uint64_t key, const TpchQ20DateQuantity *d, std::size_t n) {
      keys_.push_back(key);
      off_.push_back(static_cast<uint32_t>(data_.size()));
      data_.insert(data_.end(), d, d + n);
    }
    std::shared_ptr<const TpchQ20SeriesLayer> Finish() {
      auto layer = std::make_shared<TpchQ20SeriesLayer>();
      std::size_t cap = 16;
      while (cap < keys_.size() * 2)
        cap <<= 1;
      layer->mask_ = cap - 1;
      layer->table_.assign(cap, Slot{kEmpty, 0, 0});
      off_.push_back(static_cast<uint32_t>(data_.size()));
      for (std::size_t i = 0; i < keys_.size(); ++i) {
        std::size_t h = Hash(keys_[i]) & layer->mask_;
        while (layer->table_[h].key != kEmpty)
          h = (h + 1) & layer->mask_;
        layer->table_[h] = {keys_[i], off_[i], off_[i + 1] - off_[i]};
      }
      layer->count_ = keys_.size();
      layer->key_ = std::move(keys_);
      layer->off_ = std::move(off_);
      layer->data_ = std::move(data_);
      return layer;
    }
    void Reserve(std::size_t keys, std::size_t entries) {
      keys_.reserve(keys);
      off_.reserve(keys + 1);
      data_.reserve(entries);
    }

  private:
    std::vector<uint64_t> keys_;
    std::vector<uint32_t> off_;
    std::vector<TpchQ20DateQuantity> data_;
  };

  TpchQ20SeriesView Find(uint64_t key) const {
    std::size_t h = Hash(key) & mask_;
    for (;;) {
      const Slot &sl = table_[h];
      if (sl.key == key)
        return {data_.data() + sl.off, static_cast<std::size_t>(sl.n), true};
      if (sl.key == kEmpty)
        return {};
      h = (h + 1) & mask_;
    }
  }
  // Two-stage software prefetch for a coming Find: the table slot, then (once
  // the slot is cached) the series data it points to.
  void PrefetchSlot(uint64_t key) const {
    __builtin_prefetch(&table_[Hash(key) & mask_]);
  }
  void PrefetchData(uint64_t key) const {
    std::size_t h = Hash(key) & mask_;
    for (;;) {
      const Slot &sl = table_[h];
      if (sl.key == key) {
        __builtin_prefetch(data_.data() + sl.off);
        return;
      }
      if (sl.key == kEmpty)
        return;
      h = (h + 1) & mask_;
    }
  }
  std::size_t Size() const { return count_; }
  std::size_t EntryCount() const { return data_.size(); }
  template <typename F> void ForEach(F &&f) const {
    for (std::size_t i = 0; i < key_.size(); ++i)
      f(key_[i], TpchQ20SeriesView{data_.data() + off_[i],
                                   static_cast<std::size_t>(off_[i + 1] - off_[i]),
                                   true});
  }

private:
  static constexpr uint64_t kEmpty = ~0ULL;
  static std::size_t Hash(uint64_t k) {
    return static_cast<std::size_t>((k * 0x9E3779B97F4A7C15ULL) >> 20);
  }
  std::size_t mask_ = 0;
  std::size_t count_ = 0;
  struct Slot {
    uint64_t key;
    uint32_t off;
    uint32_t n;
  };
  std::vector<Slot> table_;
  std::vector<uint64_t> key_;
  std::vector<uint32_t> off_;
  std::vector<TpchQ20DateQuantity> data_;
};

// Persistent layered overlay of changed series (an LSM: newest layer first).
// A commit adds one layer holding only the pairs it touched; adjacent layers
// of comparable size are merged, so the work per commit is proportional to the
// commit's changes (amortized, O(log) rewrites per entry) and the layer count
// stays logarithmic in the overlay size.  Published states share layers.
class TpchQ20SeriesOverlay {
public:
  TpchQ20SeriesView Find(uint64_t key) const {
    if (filter_ && !FilterTest(key))
      return {};
    for (const auto &layer : layers_) {
      const auto v = layer->Find(key);
      if (v.found)
        return v;
    }
    return {};
  }
  bool Empty() const { return layers_.empty(); }
  void PrefetchSlot(uint64_t key) const {
    if (!FilterTest(key))
      return;
    for (const auto &layer : layers_)
      layer->PrefetchSlot(key);
  }
  void PrefetchData(uint64_t key) const {
    if (!FilterTest(key))
      return;
    for (const auto &layer : layers_)
      layer->PrefetchData(key);
  }
  std::size_t Size() const {
    std::size_t n = 0;
    for (const auto &layer : layers_)
      n += layer->Size();
    return n;
  }
  // Every overlaid key (may repeat across layers).
  template <typename F> void ForEachKey(F &&f) const {
    for (const auto &layer : layers_)
      layer->ForEach([&](uint64_t k, const TpchQ20SeriesView &) { f(k); });
  }
  void Push(std::shared_ptr<const TpchQ20SeriesLayer> layer) {
    AddToFilter(*layer);
    layers_.insert(layers_.begin(), std::move(layer));
    while (layers_.size() >= 2 &&
           layers_[1]->Size() <= 2 * layers_[0]->Size()) {
      auto merged = Merge(*layers_[0], *layers_[1]);
      layers_.erase(layers_.begin());
      layers_[0] = std::move(merged);
    }
  }

private:
  // One-hash bit filter over every overlaid key (~8+ bits per key, L2-sized
  // for typical overlays): most reads probe keys the overlay does not hold.
  static std::size_t FilterHash(uint64_t k) {
    return static_cast<std::size_t>((k * 0xC2B2AE3D27D4EB4FULL) >> 17);
  }
  bool FilterTest(uint64_t key) const {
    const std::size_t h = FilterHash(key) & filter_mask_;
    return ((*filter_)[h >> 6] >> (h & 63)) & 1;
  }
  void AddToFilter(const TpchQ20SeriesLayer &layer) {
    const std::size_t total = Size() + layer.Size();
    std::size_t bits = filter_ ? filter_mask_ + 1 : 0;
    std::shared_ptr<std::vector<uint64_t>> next;
    if (bits == 0 || total * 8 > bits) {
      bits = 1024;
      while (bits < total * 16)
        bits <<= 1;
      next = std::make_shared<std::vector<uint64_t>>(bits / 64, 0);
      filter_mask_ = bits - 1;
      for (const auto &l : layers_)
        l->ForEach([&](uint64_t k, const TpchQ20SeriesView &) {
          const std::size_t h = FilterHash(k) & filter_mask_;
          (*next)[h >> 6] |= 1ULL << (h & 63);
        });
    } else {
      next = std::make_shared<std::vector<uint64_t>>(*filter_);
    }
    layer.ForEach([&](uint64_t k, const TpchQ20SeriesView &) {
      const std::size_t h = FilterHash(k) & filter_mask_;
      (*next)[h >> 6] |= 1ULL << (h & 63);
    });
    filter_ = std::move(next);
  }
  static std::shared_ptr<const TpchQ20SeriesLayer>
  Merge(const TpchQ20SeriesLayer &newer, const TpchQ20SeriesLayer &older) {
    TpchQ20SeriesLayer::Builder b;
    b.Reserve(newer.Size() + older.Size(),
              newer.EntryCount() + older.EntryCount());
    newer.ForEach([&](uint64_t k, const TpchQ20SeriesView &v) {
      b.Add(k, v.data, v.n);
    });
    older.ForEach([&](uint64_t k, const TpchQ20SeriesView &v) {
      if (!newer.Find(k).found)
        b.Add(k, v.data, v.n);
    });
    return b.Finish();
  }
  std::vector<std::shared_ptr<const TpchQ20SeriesLayer>> layers_;
  std::shared_ptr<const std::vector<uint64_t>> filter_;
  std::size_t filter_mask_ = 0;
};

// One published version: the shared immutable base plus small copy-on-write
// overlays holding every pair / part / supplier changed since the base.
struct TpchQ20State {
  std::shared_ptr<const TpchQ20Base> base;
  // pair_key -> complete current series (empty vector = no lineitems), sorted
  // by shipdate.
  TpchQ20SeriesOverlay series;
  // partkey -> complete current partsupp list (empty = none).
  core::ShardedCowHashMap<std::vector<TpchQ20PsPair>, 7> partsupps;
  core::ShardedCowHashMap<TpchQ20PartOverlay, 7> parts;
  core::ShardedCowHashMap<TpchQ20SupplierOverlay, 7> suppliers;

  // Total overlay population; Stage requests a rebuild past a bound.
  std::size_t OverlayWeight() const {
    return series.Size() + partsupps.size() + parts.size() + suppliers.size();
  }

  // Effective-state helpers (overlay first, then base).
  int32_t SupplierNation(int64_t suppkey) const; // -1 when absent
  bool FindSupplier(int64_t suppkey, TpchQ20SupplierRecord &out) const;
  // The effective record (overlay first), or nullptr; valid with the state.
  const TpchQ20SupplierRecord *SupplierRecord(int64_t suppkey) const;
  // Window sum for a pair; returns whether any lineitem in the window.
  bool PairWindowSum(uint64_t pair_key, int32_t lo, int32_t hi,
                     int64_t &sum) const;
  std::vector<int32_t> FindNationKeysByName(const std::string &name) const;
};

enum class TpchQ20ImageKind : uint8_t { Full };

struct TpchQ20StateImage {
  TpchQ20ImageKind kind = TpchQ20ImageKind::Full;
  std::shared_ptr<const TpchQ20State> state;
};

// ── Output row ──────────────────────────────────────────────────────────────
struct TpchQ20OutputRow {
  std::string name;
  std::string address;
};

// ── Delta types ─────────────────────────────────────────────────────────────
struct TpchQ20LineitemDelta {
  int32_t shipdate_days = 0;
  int64_t partkey = 0;
  int64_t suppkey = 0;
  int64_t quantity_raw = 0;
  int8_t sign = 0;
};

struct TpchQ20PartsuppDelta {
  int64_t partkey = 0;
  int64_t suppkey = 0;
  int64_t availqty = 0;
  int8_t sign = 0;
};

struct TpchQ20PartNameDelta {
  int64_t partkey = 0;
  std::string old_name;
  std::string new_name;
  int8_t sign = 0;
};

struct TpchQ20SupplierDelta {
  int64_t suppkey = 0;
  bool present = false; // false: supplier removed
  TpchQ20SupplierRecord record;
};

// ── Encode / Decode ─────────────────────────────────────────────────────────
std::vector<uint8_t> EncodeTpchQ20State(const TpchQ20State &state);
bool DecodeTpchQ20State(const std::vector<uint8_t> &bytes, TpchQ20State &state);

// ── TxnBuffer ───────────────────────────────────────────────────────────────
class TpchQ20TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  void MarkRebuild();

  const std::vector<TpchQ20LineitemDelta> &LineitemDeltas() const;
  const std::vector<TpchQ20PartsuppDelta> &PartsuppDeltas() const;
  const std::vector<TpchQ20PartNameDelta> &PartNameDeltas() const;
  const std::vector<TpchQ20SupplierDelta> &SupplierDeltas() const;

  void AddLineitemDelta(TpchQ20LineitemDelta delta);
  void AddPartsuppDelta(TpchQ20PartsuppDelta delta);
  void AddPartNameDelta(TpchQ20PartNameDelta delta);
  void AddSupplierDelta(TpchQ20SupplierDelta delta);

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<TpchQ20LineitemDelta> lineitem_deltas;
  std::vector<TpchQ20PartsuppDelta> partsupp_deltas;
  std::vector<TpchQ20PartNameDelta> part_name_deltas;
  std::vector<TpchQ20SupplierDelta> supplier_deltas;
};

// ── Module ──────────────────────────────────────────────────────────────────
class TpchQ20Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = TpchQ20Layout::kAccelerator;

  explicit TpchQ20Module(TpchQ20Config config,
                         TpchQ20InitialScan *initial_scan = nullptr)
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
  TpchQ20State BuildState(core::AuxEngineServices &services) const;
  bool LoadState(core::AuxEngineServices &services,
                 std::shared_ptr<const TpchQ20State> &state) const;
  void EmitResult(const TpchQ20State &state, int32_t start_date_days,
                  int32_t end_date_days, const std::string &part_name_pattern,
                  const std::string &nation_name,
                  core::AuxEngineServices &services,
                  core::AuxRowSink &sink) const;

  TpchQ20Config config;
  TpchQ20InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
