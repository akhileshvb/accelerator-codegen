#pragma once

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_query_module.hpp"
#include "prims/chunked_cow_array.hpp"
#include "prims/dense_id_array.hpp"

namespace accelerator_codegen::aux_state::query {

struct TpchQ9Layout {
  static constexpr const char *kAccelerator = "tpch_q9_aux";
  // Metadata partition (name index + nation labels; profit cells live in slot partitions).
  static constexpr core::PartitionId kMetadata = 0;
  // Maintenance-only side maps (supplier/order/partsupp); Execute never reads this.
  static constexpr core::PartitionId kMaintenanceSide = 1;
  // Per-name-token aggregate cube [token][nation][year]; read path for
  // whole-token needles touches only this partition + the metadata.
  static constexpr core::PartitionId kCube = 2;
  static constexpr core::PartitionId kProfitSlotBase = 10000;
  // Internal-only profit shards (not published); sized from part cardinality at build.
  static constexpr core::PartitionId kMinProfitSlots = 128;
  static constexpr core::PartitionId kMaxProfitSlots = 4096;

  static core::PartitionId ProfitSlotPartition(core::PartitionId slot) {
    return kProfitSlotBase + slot;
  }

  static std::array<core::PartitionId, 1> Partitions() {
    return {kMetadata};
  }
};

struct TpchQ9Config {
  core::TableId part_table = 0;
  core::TableId supplier_table = 0;
  core::TableId nation_table = 0;
  core::TableId orders_table = 0;
  core::TableId partsupp_table = 0;
  core::TableId lineitem_table = 0;
};

struct TpchQ9PartInput {
  int64_t partkey = 0;
  std::string name;
};

struct TpchQ9SupplierInput {
  int64_t suppkey = 0;
  int32_t nationkey = 0;
};

struct TpchQ9NationInput {
  int32_t nationkey = 0;
  std::string name;
};

struct TpchQ9OrdersInput {
  int64_t orderkey = 0;
  int32_t orderdate_days = 0;
};

struct TpchQ9PartsuppInput {
  int64_t partkey = 0;
  int64_t suppkey = 0;
  int64_t supplycost_raw = 0;
};

struct TpchQ9LineitemInput {
  int64_t partkey = 0;
  int64_t suppkey = 0;
  int64_t orderkey = 0;
  int64_t quantity_raw = 0;
  int64_t extendedprice_raw = 0;
  int64_t discount_raw = 0;
};

class TpchQ9InitialScan {
public:
  virtual ~TpchQ9InitialScan() = default;
  // Partitioned scans for builds: rows of one partition arrive in table order
  // on one thread; partitions run concurrently on the pool.
  virtual void ScanParts(
      const core::AuxEngineServices &, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ9PartInput &)> &) = 0;
  virtual void ScanSuppliers(
      const core::AuxEngineServices &, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ9SupplierInput &)> &) = 0;
  virtual void ScanNationsSerial(
      const std::function<void(const TpchQ9NationInput &)> &) = 0;
  virtual void ScanOrders(
      const core::AuxEngineServices &, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ9OrdersInput &)> &) = 0;
  virtual void ScanPartsupps(
      const core::AuxEngineServices &, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ9PartsuppInput &)> &) = 0;
  virtual void ScanLineitems(
      const core::AuxEngineServices &, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ9LineitemInput &)> &) = 0;

  // Point lookup of an order's year; false when unsupported or absent.
  virtual bool LookupOrderYear(int64_t, int32_t &) { return false; }
};

struct Q9CellKey {
  int32_t nationkey = 0;
  int32_t year = 0;

  bool operator==(const Q9CellKey &other) const {
    return nationkey == other.nationkey && year == other.year;
  }
  bool operator<(const Q9CellKey &other) const {
    if (nationkey != other.nationkey)
      return nationkey < other.nationkey;
    return year < other.year;
  }
};

struct Q9CellKeyHash {
  std::size_t operator()(const Q9CellKey &key) const {
    return static_cast<std::size_t>(key.nationkey) * 31 +
           static_cast<std::size_t>(key.year);
  }
};

#pragma pack(push, 1)
struct Q9ProfitCell {
  uint8_t nationkey = 0;
  int16_t year = 0; // any year (orders may move outside 1992-1998)
  int64_t profit = 0;

  int32_t Nation() const { return static_cast<int32_t>(nationkey); }
  int32_t Year() const { return static_cast<int32_t>(year); }

  static Q9ProfitCell Make(int32_t nation, int32_t year, int64_t profit_raw) {
    Q9ProfitCell cell;
    cell.nationkey = nation < 0 ? 0 : static_cast<uint8_t>(nation);
    cell.year = static_cast<int16_t>(year);
    cell.profit = profit_raw;
    return cell;
  }
};
#pragma pack(pop)

// Lineitem aggregate of one (part, supplier) pair, per order year
// 1992..1998 (rev = Σ ep_raw*(100-disc_raw), qty = Σ quantity_raw). A pair's
// contribution to the answer is rev - cost*qty under the supplier's nation,
// present iff part, supplier and partsupp rows all exist.
struct Q9Pair {
  static constexpr int32_t kYearBase = 1992;
  static constexpr int32_t kYears = 7;
  int32_t suppkey = 0;
  int64_t rev[kYears] = {};
  int64_t qty[kYears] = {};
};

// Aggregate for an order year outside Q9Pair's dense range.
struct Q9ExtraAgg {
  int32_t suppkey = 0;
  int32_t year = 0;
  int64_t rev = 0;
  int64_t qty = 0;
};

struct Q9PartRecord {
  std::string normalized_name;
  bool exists = false; // a part row exists (aggregates may outlive it)
  std::vector<Q9ProfitCell> cells; // derived from pairs; empty if !exists
  std::vector<Q9Pair> pairs;
  std::vector<Q9ExtraAgg> extras;
};

// Part records of one profit slot, sorted by partkey. Records are shared_ptr'd
// so copying a slot (every maintenance write copies each slot it touches out of
// the published image) copies only pointers; Mutable() clones just the records
// a write changes. Bulk mode (build) appends through a hash index; FinishBulk()
// sorts and drops it.
class Q9PartMap {
public:
  using Entry = std::pair<int64_t, std::shared_ptr<Q9PartRecord>>;

  std::vector<Entry>::const_iterator begin() const { return entries.begin(); }
  std::vector<Entry>::const_iterator end() const { return entries.end(); }
  std::size_t size() const { return entries.size(); }
  bool empty() const { return entries.empty(); }
  void clear() {
    entries.clear();
    bulk_index.clear();
  }

  void BeginBulk() { bulk = true; }
  void FinishBulk() {
    if (!bulk)
      return;
    bulk = false;
    bulk_index.clear();
    bulk_index.rehash(0);
    std::sort(entries.begin(), entries.end(),
              [](const Entry &a, const Entry &b) { return a.first < b.first; });
  }

  const Q9PartRecord *Find(int64_t partkey) const {
    const auto it = Lower(partkey);
    return it != entries.end() && it->first == partkey ? it->second.get()
                                                       : nullptr;
  }
  // The record for writing (cloned if a published image shares it); null if absent.
  Q9PartRecord *Mutable(int64_t partkey) {
    if (bulk) {
      const auto it = bulk_index.find(partkey);
      return it == bulk_index.end() ? nullptr : Own(entries[it->second].second);
    }
    const auto it = Lower(partkey);
    return it != entries.end() && it->first == partkey ? Own(it->second)
                                                       : nullptr;
  }
  // The record for writing, created empty if absent.
  Q9PartRecord &Get(int64_t partkey) {
    if (bulk) {
      const auto [it, inserted] = bulk_index.emplace(partkey, entries.size());
      if (inserted)
        entries.emplace_back(partkey, std::make_shared<Q9PartRecord>());
      return *Own(entries[it->second].second);
    }
    auto it = Lower(partkey);
    if (it == entries.end() || it->first != partkey)
      it = entries.insert(it, {partkey, std::make_shared<Q9PartRecord>()});
    return *Own(it->second);
  }
  void Emplace(int64_t partkey, Q9PartRecord record) {
    Get(partkey) = std::move(record);
  }
  void Erase(int64_t partkey) {
    const auto it = Lower(partkey);
    if (it != entries.end() && it->first == partkey)
      entries.erase(it);
  }

private:
  std::vector<Entry>::const_iterator Lower(int64_t partkey) const {
    return std::lower_bound(
        entries.begin(), entries.end(), partkey,
        [](const Entry &e, int64_t k) { return e.first < k; });
  }
  std::vector<Entry>::iterator Lower(int64_t partkey) {
    return std::lower_bound(
        entries.begin(), entries.end(), partkey,
        [](const Entry &e, int64_t k) { return e.first < k; });
  }
  static Q9PartRecord *Own(std::shared_ptr<Q9PartRecord> &rec) {
    if (rec.use_count() > 1)
      rec = std::make_shared<Q9PartRecord>(*rec);
    return rec.get();
  }

  std::vector<Entry> entries;
  bool bulk = false;
  std::unordered_map<int64_t, std::size_t> bulk_index;
};

// Ids beyond these are never stored in the dense COW arrays (a directory entry
// per chunk must stay affordable). Part and supplier ids stay far below
// 2^31 (SF3000: 6e8 and 3e7); order keys reach 1.8e10 and have 64K-slot chunks.
constexpr int64_t kQ9MaxId = int64_t{1} << 31;
constexpr int64_t kQ9MaxOrderKey = int64_t{1} << 36;
inline bool Q9InRange(int64_t key, int64_t limit = kQ9MaxId) {
  return key >= 0 && key < limit;
}
template <typename Array, typename V>
bool Q9BoundedSet(Array &array, int64_t key, const V &value,
                  int64_t limit = kQ9MaxId) {
  return Q9InRange(key, limit) && array.Set(key, value);
}

// Per-token aggregate of profit cells over all parts whose name has the token.
struct Q9TokenCube;

struct TpchQ9State {
  struct PartProfitPartition {
    Q9PartMap parts;
  };

  // Internal partition slots (not published individually)
  core::PartitionId profit_slot_count = TpchQ9Layout::kMinProfitSlots;
  std::vector<PartProfitPartition> profit_partitions;

  // Name index: word token -> sorted unique part keys (uint32; TPC-H partkeys
  // fit). Execute intersects these lists; keep them dense and decode-free.
  // Posting vectors are shared_ptr'd so a name write clones only the touched
  // tokens' vectors; published images never mutate them.
  using Posting = std::vector<int32_t>;
  std::unordered_map<std::string, std::shared_ptr<Posting>> name_postings;
  bool postings_bulk = false; // build mode: append now, sort in Finalize

  // Nations sorted by name for ORDER BY nation ASC without per-query sort.
  std::vector<std::pair<std::string, int32_t>> nation_emit_order;

  std::unordered_map<int32_t, std::string> nation_names;

  static constexpr uint8_t kAbsentNationCode = 255;
  static constexpr uint8_t kAbsentYearCode = 0;
  static constexpr int32_t kPackedYearBias = 1991; // stored = year - 1991

  core::ChunkedCowArray<uint8_t, 16> supplier_nations{kAbsentNationCode};
  core::ChunkedCowArray<uint8_t, 16> order_years{kAbsentYearCode};
  // suppkey -> partkeys that have a lineitem-aggregate pair with that supplier.
  core::ChunkedCowArray<std::shared_ptr<const std::vector<int64_t>>, 10> supp_pairs;
  std::unordered_map<int64_t, int32_t> order_year_overflow;

  struct PackedPartsuppRow {
    static constexpr int kSlots = 4;
    int32_t suppkeys[kSlots]{};
    int32_t costs[kSlots]{};
  };
  core::ChunkedCowArray<PackedPartsuppRow, 10> partsupp_rows;

  struct PartsuppKey {
    int64_t partkey = 0;
    int64_t suppkey = 0;
    bool operator==(const PartsuppKey &o) const {
      return partkey == o.partkey && suppkey == o.suppkey;
    }
  };
  struct PartsuppKeyHash {
    std::size_t operator()(const PartsuppKey &k) const {
      return std::hash<int64_t>()(k.partkey) ^
             (std::hash<int64_t>()(k.suppkey) << 1);
    }
  };
  std::unordered_map<PartsuppKey, int64_t, PartsuppKeyHash> partsupp_overflow;

  // Token cube (published in its own partition; null/inactive elsewhere).
  std::unordered_map<std::string, std::shared_ptr<Q9TokenCube>> cube;
  bool cube_active = false;

  void CubeAdd(const std::vector<std::string> &tokens, int32_t nation,
               int32_t year, int64_t delta);
  // Add (sign=+1) / subtract (sign=-1) all cells of a record into the cube.
  void CubeApplyRecord(const Q9PartRecord &record, int sign);
  void BuildCube(const core::AuxEngineServices &services);

  // Copy only the join-side maps (page directories + overflow maps).
  void CopySideFrom(const TpchQ9State &o) {
    supplier_nations = o.supplier_nations;
    order_years = o.order_years;
    order_year_overflow = o.order_year_overflow;
    partsupp_rows = o.partsupp_rows;
    partsupp_overflow = o.partsupp_overflow;
    supp_pairs = o.supp_pairs;
  }

  void EraseSupplierNation(int64_t suppkey) {
    Q9BoundedSet(supplier_nations, suppkey, kAbsentNationCode);
  }
  std::shared_ptr<const std::vector<int64_t>> SuppPairs(int64_t suppkey) const {
    return supp_pairs.Get(suppkey, nullptr);
  }
  // Register partkey under suppkey (clones that supplier's list).
  void AddSuppPair(int64_t suppkey, int64_t partkey) {
    auto cur = supp_pairs.Get(suppkey, nullptr);
    auto next = cur ? std::make_shared<std::vector<int64_t>>(*cur)
                    : std::make_shared<std::vector<int64_t>>();
    next->push_back(partkey);
    Q9BoundedSet(supp_pairs, suppkey, std::shared_ptr<const std::vector<int64_t>>(std::move(next)));
  }

  bool LookupSupplierNation(int64_t suppkey, int32_t &nationkey) const {
    const auto code = supplier_nations.Get(suppkey, kAbsentNationCode);
    if (code == kAbsentNationCode)
      return false;
    nationkey = static_cast<int32_t>(code);
    return true;
  }
  void SetSupplierNation(int64_t suppkey, int32_t nationkey) {
    const auto code =
        (nationkey < 0 || nationkey >= static_cast<int32_t>(kAbsentNationCode))
            ? kAbsentNationCode
            : static_cast<uint8_t>(nationkey);
    Q9BoundedSet(supplier_nations, suppkey, code);
  }

  bool LookupOrderYear(int64_t orderkey, int32_t &year) const {
    const auto ov = order_year_overflow.find(orderkey);
    if (ov != order_year_overflow.end()) {
      year = ov->second;
      return true;
    }
    const auto code = order_years.Get(orderkey, kAbsentYearCode);
    if (code == kAbsentYearCode)
      return false;
    year = kPackedYearBias + static_cast<int32_t>(code);
    return true;
  }
  void SetOrderYear(int64_t orderkey, int32_t year) {
    if (year > kPackedYearBias && year <= kPackedYearBias + 254 &&
        Q9BoundedSet(order_years, orderkey,
                     static_cast<uint8_t>(year - kPackedYearBias),
                     kQ9MaxOrderKey)) {
      order_year_overflow.erase(orderkey);
      return;
    }
    Q9BoundedSet(order_years, orderkey, kAbsentYearCode, kQ9MaxOrderKey);
    order_year_overflow[orderkey] = year;
  }
  void EraseOrderYear(int64_t orderkey) {
    Q9BoundedSet(order_years, orderkey, kAbsentYearCode, kQ9MaxOrderKey);
    order_year_overflow.erase(orderkey);
  }

  bool LookupPartsuppCost(int64_t partkey, int64_t suppkey,
                          int64_t &cost) const {
    const auto ov = partsupp_overflow.find(PartsuppKey{partkey, suppkey});
    if (ov != partsupp_overflow.end()) {
      cost = ov->second;
      return true;
    }
    if (!Q9InRange(partkey) || suppkey < 1 ||
        suppkey > static_cast<int64_t>(INT32_MAX))
      return false;
    const auto row = partsupp_rows.Get(partkey);
    const auto sk = static_cast<int32_t>(suppkey);
    for (int i = 0; i < PackedPartsuppRow::kSlots; ++i) {
      if (row.suppkeys[i] == sk) {
        cost = row.costs[i];
        return true;
      }
    }
    return false;
  }
  // Clear the packed-row entry (if any) for the pair.
  void ErasePackedPartsupp(int64_t partkey, int64_t suppkey) {
    if (!Q9InRange(partkey) || suppkey < 1 || suppkey > static_cast<int64_t>(INT32_MAX))
      return;
    auto row = partsupp_rows.Get(partkey);
    const auto sk = static_cast<int32_t>(suppkey);
    for (int i = 0; i < PackedPartsuppRow::kSlots; ++i) {
      if (row.suppkeys[i] == sk) {
        row.suppkeys[i] = 0;
        row.costs[i] = 0;
        Q9BoundedSet(partsupp_rows, partkey, row);
        return;
      }
    }
  }
  void ErasePartsupp(int64_t partkey, int64_t suppkey) {
    partsupp_overflow.erase(PartsuppKey{partkey, suppkey});
    ErasePackedPartsupp(partkey, suppkey);
  }
  void SetPartsuppCost(int64_t partkey, int64_t suppkey, int64_t cost) {
    if (!partsupp_overflow.empty())
      partsupp_overflow.erase(PartsuppKey{partkey, suppkey});
    if (!Q9InRange(partkey) || suppkey < 1 ||
        suppkey > static_cast<int64_t>(INT32_MAX) ||
        cost < static_cast<int64_t>(INT32_MIN) ||
        cost > static_cast<int64_t>(INT32_MAX)) {
      ErasePackedPartsupp(partkey, suppkey);
      partsupp_overflow[PartsuppKey{partkey, suppkey}] = cost;
      return;
    }
    auto row = partsupp_rows.Get(partkey);
    const auto sk = static_cast<int32_t>(suppkey);
    int empty = -1;
    for (int i = 0; i < PackedPartsuppRow::kSlots; ++i) {
      if (row.suppkeys[i] == sk) {
        row.costs[i] = static_cast<int32_t>(cost);
        Q9BoundedSet(partsupp_rows, partkey, row);
        return;
      }
      if (empty < 0 && row.suppkeys[i] == 0)
        empty = i;
    }
    if (empty >= 0) {
      row.suppkeys[empty] = sk;
      row.costs[empty] = static_cast<int32_t>(cost);
      Q9BoundedSet(partsupp_rows, partkey, row);
      return;
    }
    partsupp_overflow[PartsuppKey{partkey, suppkey}] = cost;
  }

  static core::PartitionId ChooseProfitSlots(std::size_t part_count);

  // Fix the slot count exactly (InitPartitions takes a PART count).
  void InitSlots(core::PartitionId slots) {
    profit_slot_count = slots < TpchQ9Layout::kMinProfitSlots
                            ? TpchQ9Layout::kMinProfitSlots
                            : slots;
    profit_partitions.assign(profit_slot_count, PartProfitPartition{});
  }
  void InitPartitions(std::size_t part_count = 0) {
    if (part_count > 0)
      profit_slot_count = ChooseProfitSlots(part_count);
    if (profit_slot_count < TpchQ9Layout::kMinProfitSlots)
      profit_slot_count = TpchQ9Layout::kMinProfitSlots;
    profit_partitions.resize(profit_slot_count);
  }

  static core::PartitionId HashPartition(int64_t partkey,
                                         core::PartitionId slot_count) {
    if (partkey < 0 || slot_count == 0) return 0;
    return static_cast<core::PartitionId>(
        static_cast<uint64_t>(partkey) % slot_count);
  }

  PartProfitPartition &GetPartition(int64_t partkey) {
    return profit_partitions[HashPartition(partkey, profit_slot_count)];
  }
  const PartProfitPartition &GetPartition(int64_t partkey) const {
    return profit_partitions[HashPartition(partkey, profit_slot_count)];
  }

  Q9PartRecord &PartRecord(int64_t partkey) {
    return GetPartition(partkey).parts.Get(partkey);
  }

  const Q9PartRecord *FindPart(int64_t partkey) const {
    return GetPartition(partkey).parts.Find(partkey);
  }

  void BeginBulkParts() {
    for (auto &partition : profit_partitions)
      partition.parts.BeginBulk();
  }
  void FinishBulkParts() {
    for (auto &partition : profit_partitions)
      partition.parts.FinishBulk();
  }

  void FinalizePostings();
  void RebuildNationEmitOrder();

  static Q9Pair *FindPair(Q9PartRecord &rec, int32_t suppkey) {
    for (auto &p : rec.pairs)
      if (p.suppkey == suppkey)
        return &p;
    return nullptr;
  }
  static const Q9Pair *FindPair(const Q9PartRecord &rec, int32_t suppkey) {
    for (const auto &p : rec.pairs)
      if (p.suppkey == suppkey)
        return &p;
    return nullptr;
  }
  // Add signed aggregate; returns true if a new dense pair was created.
  static bool AddPairAgg(Q9PartRecord &rec, int32_t suppkey, int32_t year,
                         int64_t rev, int64_t qty);
  // Signed cell delta of one part, gathered during maintenance and netted
  // before it is applied to the part's cells and the token cube.
  struct CellDelta {
    int64_t partkey;
    int32_t nation;
    int32_t year;
    int64_t delta;
  };
  // One pair's per-year profit (rev - cost*qty) under its supplier's nation.
  struct PairSnap {
    int32_t nation = 0;
    int64_t v[Q9Pair::kYears] = {};
  };
  // Fills `snap` if the pair currently contributes (part, supplier and partsupp
  // exist); out-of-range-year extras are appended to extras_out scaled by sign.
  static bool PairSnapshot(int64_t partkey, const Q9PartRecord &rec,
                           int32_t suppkey, int sign, const TpchQ9State &side,
                           PairSnap &snap, std::vector<CellDelta> &extras_out);
  // Nets the gathered deltas per (part, nation, year) and applies them to the
  // parts' cells (per slot) and to the cube.
  void ApplyNetCellDeltas(const core::AuxEngineServices &services,
                          std::vector<std::vector<CellDelta>> &chunks);
  // Add (sign=+1) / remove (sign=-1) one pair's contribution to the record's
  // cells and the token cube, resolving supplier nation / cost from `side`.
  void ApplyPairContribution(int64_t partkey, Q9PartRecord &rec,
                             const std::vector<std::string> &tokens,
                             int32_t suppkey, int sign,
                             const TpchQ9State &side);
  void AdjustCell(Q9PartRecord &rec, const std::vector<std::string> &tokens,
                  int32_t nation, int32_t year, int64_t delta);
  // Build-time: derive cells of every existing part from its pairs.
  void BuildCellsFromPairs(const core::AuxEngineServices &services);
  void BuildSuppPairsIndex();

  void AddNamePostings(int64_t partkey, const std::string &raw_name);
  void RemoveNamePostings(int64_t partkey);
};

struct Q9TokenCube {
  static constexpr int32_t kNations = 25;
  static constexpr int32_t kYears = 7;
  static constexpr int32_t kYearBase = 1992;
  int64_t dense[kNations][kYears] = {};
  // Cells outside the dense range (profit != 0 only).
  std::vector<Q9ProfitCell> sparse;
};

enum class TpchQ9ImageKind : uint8_t {
  Metadata,
  MaintenanceSide,
  ProfitSlot,
  Cube,
};

struct TpchQ9StateImage {
  TpchQ9ImageKind kind = TpchQ9ImageKind::Metadata;
  core::PartitionId profit_slot = 0;
  std::shared_ptr<const TpchQ9State> state;
};

struct TpchQ9LineitemDelta {
  int64_t partkey = 0;
  int64_t suppkey = 0;
  int64_t orderkey = 0;
  int64_t quantity_raw = 0;
  int64_t extendedprice_raw = 0;
  int64_t discount_raw = 0;
  int8_t sign = 0;
};

struct TpchQ9SupplierDelta {
  int64_t suppkey = 0;
  int64_t nationkey = 0;
  int8_t sign = 0;
};

struct TpchQ9PartsuppDelta {
  int64_t partkey = 0;
  int64_t suppkey = 0;
  int64_t cost_raw = 0;
  int8_t sign = 0;
};

struct TpchQ9OrderDelta {
  int64_t orderkey = 0;
  int32_t order_year = 0;
  int8_t sign = 0; // +1 insert, -1 delete
};

int32_t TpchQ9ExtractYear(int32_t days_since_epoch);

class TpchQ9TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  const std::vector<TpchQ9LineitemDelta> &Deltas() const;
  const std::vector<TpchQ9OrderDelta> &OrderDeltas() const;
  void MarkRebuild();
  void AddDelta(TpchQ9LineitemDelta delta);
  void AddOrderDelta(TpchQ9OrderDelta delta);
  void AddPartNamePosting(int64_t partkey, const std::string &old_name,
                          const std::string &new_name);

  const std::vector<TpchQ9SupplierDelta> &SupplierDeltas() const {
    return supplier_deltas;
  }
  const std::vector<TpchQ9PartsuppDelta> &PartsuppDeltas() const {
    return partsupp_deltas;
  }
  void AddSupplierDelta(TpchQ9SupplierDelta d);
  void AddPartsuppDelta(TpchQ9PartsuppDelta d);

  const std::vector<std::tuple<int64_t, std::string, std::string>> &
  PartNameDeltas() const { return part_name_deltas; }

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<TpchQ9LineitemDelta> deltas;
  std::vector<TpchQ9OrderDelta> order_deltas;
  std::vector<TpchQ9SupplierDelta> supplier_deltas;
  std::vector<TpchQ9PartsuppDelta> partsupp_deltas;
  std::vector<std::tuple<int64_t, std::string, std::string>> part_name_deltas;
};

class TpchQ9Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = TpchQ9Layout::kAccelerator;

  explicit TpchQ9Module(TpchQ9Config config,
                        TpchQ9InitialScan *initial_scan = nullptr)
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
  void BuildStateInto(const core::AuxEngineServices &services, TpchQ9State &state) const;

  TpchQ9Config config;
  TpchQ9InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
