#pragma once

#include <array>
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

#include "accelerator_codegen/stateful/core/aux_query_module.hpp"

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

struct TpchQ20PartInput {
  int64_t key = 0;
  std::string name;
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
  virtual void ScanLineitems(
      const std::function<void(const TpchQ20LineitemInput &)> &) = 0;
  virtual void ScanParts(
      const std::function<void(const TpchQ20PartInput &)> &) = 0;
  virtual void ScanPartsupps(
      const std::function<void(const TpchQ20PartsuppInput &)> &) = 0;
  virtual void ScanSuppliers(
      const std::function<void(const TpchQ20SupplierInput &)> &) = 0;
  virtual void ScanNations(
      const std::function<void(const TpchQ20NationInput &)> &) = 0;

  // Optional typed/ordered bulk scans used by Build (SQL ORDER BY, no boxed
  // per-cell values). Return false when unsupported (generic path is used).
  //   lineitems ordered by (partkey, suppkey)
  virtual bool ScanLineitemsByPair(
      const std::function<void(int64_t partkey, int64_t suppkey,
                               int32_t shipdate_days, int64_t quantity_raw)>
          &consumer) {
    (void)consumer;
    return false;
  }
  //   partsupps ordered by (partkey, suppkey)
  virtual bool ScanPartsuppsOrdered(
      const std::function<void(int64_t partkey, int64_t suppkey,
                               int64_t availqty)> &consumer) {
    (void)consumer;
    return false;
  }
  //   parts, any order
  virtual bool ScanPartNames(
      const std::function<void(int64_t partkey, const char *name,
                               std::size_t len)> &consumer) {
    (void)consumer;
    return false;
  }
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

// Copy-on-write hash map: 128 shards, each an immutable-once-published
// shared_ptr<Map>. Copying a TpchQ20CowMap shares every shard; a mutation
// clones only the touched shard (and only once per staged commit).
template <typename V> class TpchQ20CowMap {
public:
  using Map = std::unordered_map<uint64_t, V>;
  static constexpr std::size_t kShards = 128;

  TpchQ20CowMap() = default;
  TpchQ20CowMap(const TpchQ20CowMap &other)
      : shards(other.shards), count(other.count) {} // owned stays empty
  TpchQ20CowMap &operator=(const TpchQ20CowMap &other) {
    shards = other.shards;
    count = other.count;
    owned.fill(false);
    return *this;
  }

  const V *Find(uint64_t key) const {
    const auto &shard = shards[ShardOf(key)];
    if (!shard)
      return nullptr;
    const auto it = shard->find(key);
    return it == shard->end() ? nullptr : &it->second;
  }
  // Returns a mutable slot (default-constructed when absent). The caller must
  // be the staging owner of this instance (never a published state).
  V &Mutable(uint64_t key, bool *inserted = nullptr) {
    auto &map = OwnedShard(ShardOf(key));
    const auto res = map.try_emplace(key);
    if (res.second) {
      ++count;
    }
    if (inserted)
      *inserted = res.second;
    return res.first->second;
  }
  void Erase(uint64_t key) {
    const std::size_t i = ShardOf(key);
    if (!shards[i] || shards[i]->find(key) == shards[i]->end())
      return;
    OwnedShard(i).erase(key);
    --count;
  }
  std::size_t Size() const { return count; }
  bool Empty() const { return count == 0; }
  template <typename F> void ForEach(F &&f) const {
    for (const auto &shard : shards) {
      if (!shard)
        continue;
      for (const auto &kv : *shard)
        f(kv.first, kv.second);
    }
  }

private:
  static std::size_t ShardOf(uint64_t key) {
    return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ULL) >> 57);
  }
  Map &OwnedShard(std::size_t i) {
    if (!shards[i])
      shards[i] = std::make_shared<Map>();
    else if (!owned[i])
      shards[i] = std::make_shared<Map>(*shards[i]);
    owned[i] = true;
    return *shards[i];
  }
  std::array<std::shared_ptr<Map>, kShards> shards{};
  std::array<bool, kShards> owned{};
  std::size_t count = 0;
};

// Immutable bulk image built once per (re)build; shared by every version.
struct TpchQ20Base {
  struct Entry {
    int32_t date = 0;
    int32_t qty = 0;
  };
  struct PsEntry {
    int64_t availqty = 0;
    int32_t suppkey = 0;
    uint32_t series_idx = kNoSeries;
  };
  static constexpr uint32_t kNoSeries = 0xFFFFFFFFu;

  // (partkey << 20 | suppkey) sorted; CSR over series_entries.
  std::vector<uint64_t> series_key;
  std::vector<uint32_t> series_off; // series_key.size() + 1
  std::vector<Entry> series_entries;
  // Pairs whose quantities do not fit Entry::qty (idx -> entries).
  std::unordered_map<uint32_t, std::vector<TpchQ20DateQuantity>> series_wide;

  // partsupp CSR by partkey (dense).
  std::vector<uint32_t> ps_off; // max_partkey + 2
  std::vector<PsEntry> ps;

  // part names sorted bytewise; parallel partkey array.
  std::string name_arena;
  std::vector<uint32_t> name_off; // n + 1
  std::vector<int64_t> name_partkey;

  // suppliers
  std::unordered_map<int64_t, TpchQ20SupplierRecord> suppliers;
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

// One published version: the shared immutable base plus small copy-on-write
// overlays holding every pair / part / supplier changed since the base.
struct TpchQ20State {
  std::shared_ptr<const TpchQ20Base> base;
  // pair_key -> complete current series (empty vector = no lineitems), sorted
  // by shipdate.
  TpchQ20CowMap<std::vector<TpchQ20DateQuantity>> series;
  // partkey -> complete current partsupp list (empty = none).
  TpchQ20CowMap<std::vector<TpchQ20PsPair>> partsupps;
  TpchQ20CowMap<TpchQ20PartOverlay> parts;
  TpchQ20CowMap<TpchQ20SupplierOverlay> suppliers;

  // Total overlay population; Stage requests a rebuild past a bound.
  std::size_t OverlayWeight() const {
    return series.Size() + partsupps.Size() + parts.Size() + suppliers.Size();
  }

  // Effective-state helpers (overlay first, then base).
  int32_t SupplierNation(int64_t suppkey) const; // -1 when absent
  bool FindSupplier(int64_t suppkey, TpchQ20SupplierRecord &out) const;
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
  void ExecuteFallback(core::AuxEngineServices &, const core::QueryArguments &,
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
