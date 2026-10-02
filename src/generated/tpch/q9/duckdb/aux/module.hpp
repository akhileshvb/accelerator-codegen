#pragma once

#include <array>
#include <climits>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_query_module.hpp"
#include "accelerator_codegen/stateful/core/dense_id_array.hpp"
#include "accelerator_codegen/stateful/core/query_layouts.hpp"

namespace accelerator_codegen::aux_state::query {

// ── Q9 layout ───────────────────────────────────────────────────────────────
struct TpchQ9Layout {
  static constexpr const char *kAccelerator = "tpch_q9_aux";
  // Metadata partition (name index + nation labels; profit cells live in slot partitions).
  static constexpr core::PartitionId kCompleteState = 0;
  // Maintenance-only side maps (supplier/order/partsupp); Execute never reads this.
  static constexpr core::PartitionId kMaintenanceSide = 1;
  static constexpr core::PartitionId kProfitSlotBase = 10000;
  // Internal-only profit shards (not published); sized from part cardinality at build.
  static constexpr core::PartitionId kMinProfitSlots = 128;
  static constexpr core::PartitionId kMaxProfitSlots = 4096;

  static core::PartitionId ProfitSlotPartition(core::PartitionId slot) {
    return kProfitSlotBase + slot;
  }

  static std::array<core::PartitionId, 1> Partitions() {
    return {kCompleteState};
  }
};

// ── Config ──────────────────────────────────────────────────────────────────
struct TpchQ9Config {
  core::TableId part_table = 0;
  core::TableId supplier_table = 0;
  core::TableId nation_table = 0;
  core::TableId orders_table = 0;
  core::TableId partsupp_table = 0;
  core::TableId lineitem_table = 0;
};

// ── Typed bulk-scan contract ────────────────────────────────────────────────
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

struct TpchQ9ProfitContribution {
  int64_t partkey = 0;
  int32_t nationkey = 0;
  int32_t year = 0;
  int64_t profit_raw = 0;
};

class TpchQ9InitialScan {
public:
  virtual ~TpchQ9InitialScan() = default;
  virtual void ScanParts(
      const std::function<void(const TpchQ9PartInput &)> &) = 0;
  virtual void ScanSuppliers(
      const std::function<void(const TpchQ9SupplierInput &)> &) = 0;
  virtual void ScanNations(
      const std::function<void(const TpchQ9NationInput &)> &) = 0;
  virtual void ScanOrders(
      const std::function<void(const TpchQ9OrdersInput &)> &) = 0;
  virtual void ScanPartsupps(
      const std::function<void(const TpchQ9PartsuppInput &)> &) = 0;
  virtual void ScanLineitems(
      const std::function<void(const TpchQ9LineitemInput &)> &) = 0;

  // Optional: SQL pushdown of joined profit GROUP BY part/nation/year.
  virtual bool ScanProfitContributions(
      const std::function<void(const TpchQ9ProfitContribution &)> &consumer) {
    (void)consumer;
    return false;
  }

  virtual bool LookupSupplierNation(int64_t suppkey, int32_t &nationkey) {
    (void)suppkey;
    (void)nationkey;
    return false;
  }
  virtual bool LookupOrderYear(int64_t orderkey, int32_t &year) {
    (void)orderkey;
    (void)year;
    return false;
  }
  virtual bool LookupPartsuppCost(int64_t partkey, int64_t suppkey,
                                  int64_t &supplycost_raw) {
    (void)partkey;
    (void)suppkey;
    (void)supplycost_raw;
    return false;
  }
};

// ── Profit cell key ─────────────────────────────────────────────────────────
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

// ── Profit cell (packed: nation + year offset + profit, no padding) ─────────
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

struct Q9PartRecord {
  std::string normalized_name;
  std::vector<Q9ProfitCell> cells;
};

// ── State structures ────────────────────────────────────────────────────────
struct TpchQ9State {
  struct PartProfitPartition {
    std::unordered_map<int64_t, Q9PartRecord> parts;
  };

  // Internal partition slots (not published individually)
  core::PartitionId profit_slot_count = TpchQ9Layout::kMinProfitSlots;
  std::vector<PartProfitPartition> profit_partitions;

  // Name index: word token -> sorted unique part keys (uint32; TPC-H partkeys
  // fit). Execute intersects these lists; keep them dense and decode-free.
  std::unordered_map<std::string, std::vector<int32_t>> name_postings;

  // Nations sorted by name for ORDER BY nation ASC without per-query sort.
  std::vector<std::pair<std::string, int32_t>> nation_emit_order;

  // ── Side state (maintenance only; Execute never reads these) ────────────
  std::unordered_map<int32_t, std::string> nation_names;

  static constexpr uint8_t kAbsentNationCode = 255;
  static constexpr uint8_t kAbsentYearCode = 0;
  static constexpr int32_t kPackedYearBias = 1991; // stored = year - 1991

  core::DenseIdArray<uint8_t> supplier_nations;
  core::DenseIdArray<uint8_t> order_years;
  std::unordered_map<int64_t, int32_t> order_year_overflow;

  struct PackedPartsuppRow {
    static constexpr int kSlots = 4;
    int32_t suppkeys[kSlots]{};
    int32_t costs[kSlots]{};
  };
  core::DenseIdArray<PackedPartsuppRow> partsupp_rows;

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
    supplier_nations.Set(suppkey, code, kAbsentNationCode);
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
    if (year > kPackedYearBias &&
        year <= kPackedYearBias + 254) {
      order_year_overflow.erase(orderkey);
      order_years.Set(orderkey, static_cast<uint8_t>(year - kPackedYearBias));
      return;
    }
    order_years.Set(orderkey, kAbsentYearCode);
    order_year_overflow[orderkey] = year;
  }
  void EraseOrderYear(int64_t orderkey) {
    order_years.Set(orderkey, kAbsentYearCode);
    order_year_overflow.erase(orderkey);
  }

  bool LookupPartsuppCost(int64_t partkey, int64_t suppkey,
                          int64_t &cost) const {
    const auto ov = partsupp_overflow.find(PartsuppKey{partkey, suppkey});
    if (ov != partsupp_overflow.end()) {
      cost = ov->second;
      return true;
    }
    if (suppkey < 1 || suppkey > static_cast<int64_t>(INT32_MAX))
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
  void SetPartsuppCost(int64_t partkey, int64_t suppkey, int64_t cost) {
    if (suppkey < 1 || suppkey > static_cast<int64_t>(INT32_MAX) ||
        cost < static_cast<int64_t>(INT32_MIN) ||
        cost > static_cast<int64_t>(INT32_MAX)) {
      partsupp_overflow[PartsuppKey{partkey, suppkey}] = cost;
      return;
    }
    auto row = partsupp_rows.Get(partkey);
    const auto sk = static_cast<int32_t>(suppkey);
    int empty = -1;
    for (int i = 0; i < PackedPartsuppRow::kSlots; ++i) {
      if (row.suppkeys[i] == sk) {
        row.costs[i] = static_cast<int32_t>(cost);
        partsupp_rows.Set(partkey, row);
        return;
      }
      if (empty < 0 && row.suppkeys[i] == 0)
        empty = i;
    }
    if (empty >= 0) {
      row.suppkeys[empty] = sk;
      row.costs[empty] = static_cast<int32_t>(cost);
      partsupp_rows.Set(partkey, row);
      return;
    }
    partsupp_overflow[PartsuppKey{partkey, suppkey}] = cost;
  }

  // ── Initialization ─────────────────────────────────────────────────────
  static core::PartitionId ChooseProfitSlots(std::size_t part_count);

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
    return GetPartition(partkey).parts[partkey];
  }

  const Q9PartRecord *FindPart(int64_t partkey) const {
    const auto &partition = GetPartition(partkey);
    auto it = partition.parts.find(partkey);
    return it == partition.parts.end() ? nullptr : &it->second;
  }

  static Q9ProfitCell *FindCell(std::vector<Q9ProfitCell> &cells,
                                int32_t nationkey, int32_t year) {
    for (auto &cell : cells) {
      if (cell.Nation() == nationkey && cell.Year() == year)
        return &cell;
    }
    return nullptr;
  }

  void FinalizePostings();
  void RebuildNationEmitOrder();

  // ── Profit contribution helpers ────────────────────────────────────────
  void AddContribution(int64_t partkey, int32_t nationkey, int32_t year,
                       int64_t profit_raw) {
    if (profit_raw == 0) return;
    auto &cells = PartRecord(partkey).cells;
    if (auto *cell = FindCell(cells, nationkey, year))
      cell->profit += profit_raw;
    else
      cells.push_back(Q9ProfitCell::Make(nationkey, year, profit_raw));
  }

  void RemoveContribution(int64_t partkey, int32_t nationkey, int32_t year,
                          int64_t profit_raw) {
    if (profit_raw == 0) return;
    auto it = GetPartition(partkey).parts.find(partkey);
    if (it == GetPartition(partkey).parts.end()) return;
    auto &cells = it->second.cells;
    for (auto cell_it = cells.begin(); cell_it != cells.end(); ++cell_it) {
      if (cell_it->Nation() != nationkey || cell_it->Year() != year) continue;
      cell_it->profit -= profit_raw;
      if (cell_it->profit == 0)
        cells.erase(cell_it);
      return;
    }
  }

  // ── Name posting helpers ───────────────────────────────────────────────
  void AddNamePostings(int64_t partkey, const std::string &raw_name);
  void RemoveNamePostings(int64_t partkey, const std::string &raw_name);
};

enum class TpchQ9ImageKind : uint8_t {
  CompleteState,
  Metadata,
  MaintenanceSide,
  ProfitSlot,
};

struct TpchQ9StateImage {
  TpchQ9ImageKind kind = TpchQ9ImageKind::CompleteState;
  core::PartitionId profit_slot = 0;
  std::shared_ptr<const TpchQ9State> state;
};

// ── Output row ──────────────────────────────────────────────────────────────
struct TpchQ9OutputRow {
  std::string nation;
  int32_t year = 0;
  int64_t sum_profit_raw = 0; // fixed-point raw value
};

// ── Join-key reverse-dependency store ───────────────────────────────────────
struct TpchQ9ReverseDeps {
  // partkey -> set of orderkeys
  std::unordered_map<int64_t, std::unordered_set<int64_t>> part_to_orderkeys;
  // orderkey -> set of partkeys
  std::unordered_map<int64_t, std::unordered_set<int64_t>> order_to_partkeys;
  // suppkey -> set of partkeys (for supplier nation changes)
  std::unordered_map<int64_t, std::unordered_set<int64_t>> suppkey_to_partkeys;
};

// ── Mutation deltas ─────────────────────────────────────────────────────────
struct TpchQ9LineitemDelta {
  int64_t partkey = 0;
  int64_t suppkey = 0;
  int64_t orderkey = 0;
  int64_t quantity_raw = 0;
  int64_t extendedprice_raw = 0;
  int64_t discount_raw = 0;
  int8_t sign = 0;
};

struct TpchQ9OrderDelta {
  int64_t orderkey = 0;
  int32_t order_year = 0;
  int8_t sign = 0; // +1 insert, -1 delete
};

// ── Encode / Decode ─────────────────────────────────────────────────────────
std::vector<uint8_t> EncodeTpchQ9CompleteState(const TpchQ9State &state);
bool DecodeTpchQ9CompleteState(const std::vector<uint8_t> &bytes,
                                TpchQ9State &state);
std::vector<uint8_t> EncodeTpchQ9Metadata(const TpchQ9State &state);
bool DecodeTpchQ9Metadata(const std::vector<uint8_t> &bytes,
                          TpchQ9State &state);
std::vector<uint8_t> EncodeTpchQ9MaintenanceSide(const TpchQ9State &state);
bool DecodeTpchQ9MaintenanceSide(const std::vector<uint8_t> &bytes,
                                 TpchQ9State &state);
std::vector<uint8_t> EncodeTpchQ9ProfitSlot(
    core::PartitionId slot, const TpchQ9State::PartProfitPartition &partition);
bool DecodeTpchQ9ProfitSlot(const std::vector<uint8_t> &bytes,
                            TpchQ9State::PartProfitPartition &partition);

// ── Date helpers ────────────────────────────────────────────────────────────
int32_t TpchQ9ExtractYear(int32_t days_since_epoch);

// ── Profit computation ──────────────────────────────────────────────────────
int64_t TpchQ9ComputeProfitRaw(int64_t extendedprice_raw, int64_t discount_raw,
                                int64_t supplycost_raw, int64_t quantity_raw);

// ── TxnBuffer ───────────────────────────────────────────────────────────────
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
  void AddRebuildPartkey(int64_t partkey);

  bool PartNameChanged() const { return !part_name_deltas.empty(); }
  const std::vector<std::tuple<int64_t, std::string, std::string>> &
  PartNameDeltas() const { return part_name_deltas; }

  bool HasRebuildSet() const { return !rebuild_partkeys.empty(); }
  const std::unordered_set<int64_t> &RebuildPartkeys() const {
    return rebuild_partkeys;
  }

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<TpchQ9LineitemDelta> deltas;
  std::vector<TpchQ9OrderDelta> order_deltas;
  std::vector<std::tuple<int64_t, std::string, std::string>> part_name_deltas;
  std::unordered_set<int64_t> rebuild_partkeys;
};

// ── Module ──────────────────────────────────────────────────────────────────
class TpchQ9Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = TpchQ9Layout::kAccelerator;

  explicit TpchQ9Module(TpchQ9Config config,
                        TpchQ9InitialScan *initial_scan = nullptr,
                        TpchQ9ReverseDeps *reverse_deps = nullptr)
      : config(config), initial_scan(initial_scan),
        reverse_deps(reverse_deps) {}

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
  // Read-path helpers
  bool LoadState(core::AuxEngineServices &services,
                 std::shared_ptr<const TpchQ9State> &state,
                 std::vector<core::PartitionId> &required_partitions,
                 const std::string *color = nullptr) const;

  void EmitResult(const TpchQ9State &state, const std::string &color,
                  core::AuxEngineServices &services,
                  core::AuxRowSink &sink) const;

  // Build helper
  void BuildStateInto(TpchQ9State &state) const;

  // Rebuild profit cells for specific partkeys from base data
  void RebuildPartkeys(TpchQ9State &state,
                       const std::unordered_set<int64_t> &partkeys) const;

  TpchQ9Config config;
  TpchQ9InitialScan *initial_scan = nullptr;
  TpchQ9ReverseDeps *reverse_deps = nullptr;
};

// Free helper functions used internally
std::string NormalizeForIndex(const std::string &s);
std::vector<std::string> NameTokens(const std::string &normalized);
std::vector<std::string> Trigrams(const std::string &normalized);
bool ContainsSubstring(std::string_view haystack, std::string_view needle);

} // namespace accelerator_codegen::aux_state::query