#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_query_module.hpp"
#include "accelerator_codegen/stateful/prims/chunked_cow_array.hpp"
#include "accelerator_codegen/stateful/prims/flat_hash_map.hpp"
#include "accelerator_codegen/stateful/prims/any_key_cow_array.hpp"
#include "accelerator_codegen/stateful/prims/keyed_string_column.hpp"

namespace accelerator_codegen::aux_state::query {

// ── Q9 layout ───────────────────────────────────────────────────────────────
struct TpchQ9Layout {
  static constexpr const char *kAccelerator = "tpch_q9_aux";
  // The whole state is one copy-on-write image.
  static constexpr core::PartitionId kCompleteState = 0;

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
// Scans return only rows whose join / filter columns are NOT NULL.
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

// One pre-aggregated (part, supplier nation, order year) group of the join
// lineitem x partsupp x supplier x orders (no part / nation join: those are
// resolved at Execute time).
struct TpchQ9ProfitContribution {
  int64_t partkey = 0;
  int32_t nationkey = 0;
  int32_t year = 0;
  int64_t rows = 0;       // number of joined lineitems
  int64_t profit_raw = 0; // SUM(amount) in 1e-4 units
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

  // Optional: the DBMS evaluates the join + GROUP BY (pre-aggregation) and
  // streams the groups; returns false when not supported (the module then
  // joins ScanLineitems itself).
  virtual bool ScanProfitContributions(
      const std::function<void(const TpchQ9ProfitContribution &)> &consumer) {
    (void)consumer;
    return false;
  }

  // Optional: the lineitems whose `column` (l_partkey, l_suppkey or
  // l_orderkey, all indexed) is one of `keys`, as the transaction leaves the
  // database (index lookups at pre-commit), with their l_linenumber; returns
  // false when not supported (a join change then rebuilds).
  virtual bool LookupLineitems(
      const char *column, const std::vector<int64_t> &keys,
      const std::function<void(const TpchQ9LineitemInput &, int32_t linenumber)>
          &consumer) {
    (void)column;
    (void)keys;
    (void)consumer;
    return false;
  }
};

// ── State ───────────────────────────────────────────────────────────────────
//
// Profit is kept per (part, group), where a group is a (supplier nationkey,
// order year) pair interned in a small dictionary: a cell is
// {group id, joined-lineitem count, SUM(amount)} = 16 B. Execute only adds up
// the cells of the parts whose p_name matches, so any LIKE pattern is exact and
// the nation / year domains are unrestricted (any int32 nationkey and year).
// The count keeps a group alive even when its profit sums to exactly 0.
//
// Parts are grouped into immutable blocks of 16 consecutive partkeys holding
// both their profit cells and their partsupp rows (CSR layout) in a
// copy-on-write chunked array: a write batch clones only the blocks of the
// touched parts. Part names live in a KeyedStringColumn (SIMD LIKE scan over
// segments of 4096 names, multithreaded). Join keys for maintenance are
// compact COW arrays: suppkey -> nationkey (8 B), orderkey -> year code (1 B,
// 255 distinct years inline plus a hash overflow, so any year is exact).
//
// Memory (TPC-H shapes): ~20 cells/part x 16 B + 4 partsupp rows x 16 B + ~25 B
// block overhead + ~45 B name = ~450 B/part (~15 B per lineitem) + ~5 B/order
// of year code: ~9 GB at SF100, ~90 GB at SF1000 (before: ~490 B/part in
// node-based maps, ~10 GB at SF100, copied whole per touched shard).
// Execute: O(parts) multithreaded name scan + O(matching cells); no state is
// copied. Write batch: O(touched blocks + touched chunks).
//
// Limits: cell profit sums are int64 in 1e-4 units (a checked add throws on
// overflow); keys are >= 0; per-cell row counts fit uint32.
constexpr std::size_t kTpchQ9BlockParts = 16; // power of two (block = key >> 4)

struct TpchQ9Cell {
  uint32_t group = 0;
  uint32_t rows = 0;
  int64_t profit = 0;
};

struct TpchQ9PartsuppEntry {
  int64_t suppkey = 0;
  int64_t cost_raw = 0;
};

struct TpchQ9PartBlock {
  uint32_t cell_begin[kTpchQ9BlockParts + 1] = {};
  uint32_t ps_begin[kTpchQ9BlockParts + 1] = {};
  std::vector<TpchQ9Cell> cells;           // per part sorted by group
  std::vector<TpchQ9PartsuppEntry> ps;     // per part sorted by suppkey
};

// Color index (read path): for each of the 92 TPC-H P_NAME color words w, the
// answer of `p_name LIKE '%w%'` pre-aggregated per group (supplier nationkey,
// order year). A part contributes its cells to every color whose word occurs
// in its name (byte substring == LIKE '%w%': the words hold no LIKE
// metacharacters). Maintained from the per-block cell diff of each write batch,
// so a read for one of these colors is O(groups) (~175 at TPC-H scale); any
// other argument scans the name column as before. Stored group-major, one
// immutable record of 92 (rows, profit) pairs (1.5 KB) per group: a batch
// clones only the records of the groups its changed cells fall in.
// Sums are int64 in 1e-4 units (per color and group: ~1e15 at SF1000); an
// overflow drops the index (color_index = false) and reads scan again.
constexpr std::size_t kTpchQ9Colors = 92;

struct TpchQ9ColorAcc {
  int64_t rows = 0;
  int64_t profit = 0;
};

struct TpchQ9GroupColors {
  std::array<TpchQ9ColorAcc, kTpchQ9Colors> by_color{};
};

struct TpchQ9State {
  core::AnyKeyCowArray<std::shared_ptr<TpchQ9PartBlock>, 12> blocks;
  bool color_index = false; // color_groups maintained (false in fallback builds)
  // Per group id (may be shorter than groups; null / missing = all zero).
  std::vector<std::shared_ptr<const TpchQ9GroupColors>> color_groups;
  core::KeyedStringColumn part_names;

  // (nationkey, year) groups, interned in first-use order; never removed.
  std::vector<std::pair<int32_t, int32_t>> groups;
  core::FlatHashMap<uint32_t> group_index;

  std::map<int32_t, std::string> nation_names;

  // Join keys needed to resolve lineitem deltas.
  static constexpr int64_t kNoNation = INT64_MIN;
  core::AnyKeyCowArray<int64_t> supplier_nation{kNoNation};
  static constexpr std::size_t kMaxYearCodes = 255;
  core::AnyKeyCowArray<uint8_t, 10> order_year_code{0}; // 0 = absent
  std::vector<int32_t> year_table;                      // code - 1 -> year
  core::ShardedCowHashMap<int32_t, 12> order_year_overflow;

  const TpchQ9PartBlock *FindBlock(int64_t partkey) const {
    return blocks.Get(partkey >> 4).get(); // floor: exact for negative keys
  }
  uint32_t GroupFor(int32_t nationkey, int32_t year);
  bool LookupSupplierNation(int64_t suppkey, int32_t &nationkey) const;
  bool LookupOrderYear(int64_t orderkey, int32_t &year) const;
  void SetSupplierNation(int64_t suppkey, int32_t nationkey);
  void EraseSupplier(int64_t suppkey);
  void SetOrderYear(int64_t orderkey, int32_t year);
  void EraseOrder(int64_t orderkey);
};

enum class TpchQ9ImageKind : uint8_t { CompleteState };

struct TpchQ9StateImage {
  TpchQ9ImageKind kind = TpchQ9ImageKind::CompleteState;
  std::shared_ptr<const TpchQ9State> state;
};

// ── Output row ──────────────────────────────────────────────────────────────
struct TpchQ9OutputRow {
  std::string nation;
  int32_t year = 0;
  int64_t sum_profit_raw = 0; // fixed-point raw value
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

struct TpchQ9SupplierDelta {
  int64_t suppkey = 0;
  int32_t nationkey = 0;
  int8_t sign = 0;
};

struct TpchQ9PartsuppDelta {
  int64_t partkey = 0;
  int64_t suppkey = 0;
  int64_t cost_raw = 0;
  int8_t sign = 0;
};

struct TpchQ9PartDelta {
  int64_t partkey = 0;
  std::string name;
  int8_t sign = 0;
};

struct TpchQ9NationDelta {
  int32_t nationkey = 0;
  std::string name;
  int8_t sign = 0;
};

// ── Date / profit helpers ───────────────────────────────────────────────────
int32_t TpchQ9ExtractYear(int32_t days_since_epoch);
int64_t TpchQ9ComputeProfitRaw(int64_t extendedprice_raw, int64_t discount_raw,
                               int64_t supplycost_raw, int64_t quantity_raw);

// ── TxnBuffer ───────────────────────────────────────────────────────────────
class TpchQ9TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  const std::vector<TpchQ9LineitemDelta> &LineitemDeltas() const { return lineitems; }
  const std::vector<TpchQ9OrderDelta> &OrderDeltas() const { return orders; }
  const std::vector<TpchQ9SupplierDelta> &SupplierDeltas() const { return suppliers; }
  const std::vector<TpchQ9PartsuppDelta> &PartsuppDeltas() const { return partsupps; }
  const std::vector<TpchQ9PartDelta> &PartDeltas() const { return parts; }
  const std::vector<TpchQ9NationDelta> &NationDeltas() const { return nations; }

  void Add(TpchQ9LineitemDelta delta);
  void Add(TpchQ9OrderDelta delta);
  void Add(TpchQ9SupplierDelta delta);
  void Add(TpchQ9PartsuppDelta delta);
  void Add(TpchQ9PartDelta delta);
  void Add(TpchQ9NationDelta delta);

private:
  bool pending = false;
  std::vector<TpchQ9LineitemDelta> lineitems;
  std::vector<TpchQ9OrderDelta> orders;
  std::vector<TpchQ9SupplierDelta> suppliers;
  std::vector<TpchQ9PartsuppDelta> partsupps;
  std::vector<TpchQ9PartDelta> parts;
  std::vector<TpchQ9NationDelta> nations;
};

// ── Module ──────────────────────────────────────────────────────────────────
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
  void ExecuteFallback(core::AuxEngineServices &, const core::QueryArguments &,
                       core::AuxRowSink &) override;
  std::vector<core::CalibrationWrite> CalibrationWrites() const override;

private:
  std::shared_ptr<TpchQ9State> BuildState(bool color_index) const;
  bool LoadState(core::AuxEngineServices &services,
                 std::shared_ptr<const TpchQ9State> &state) const;
  void EmitResult(const TpchQ9State &state, const core::QueryArguments &arguments,
                  core::AuxRowSink &sink) const;

  TpchQ9Config config;
  TpchQ9InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
