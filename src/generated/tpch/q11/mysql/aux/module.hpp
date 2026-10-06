#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_query_module.hpp"
#include "accelerator_codegen/stateful/core/chunked_cow_array.hpp"
#include "accelerator_codegen/stateful/core/any_key_cow_array.hpp"
#include "accelerator_codegen/stateful/core/cow_sorted_blocks.hpp"
#include "accelerator_codegen/stateful/core/flat_hash_map.hpp"

namespace accelerator_codegen::aux_state::query {

// ── Q11 layout ──────────────────────────────────────────────────────────────
struct TpchQ11Layout {
  static constexpr const char *kAccelerator = "tpch_q11_aux";
  static constexpr core::PartitionId kState = 0;

  static constexpr std::array<core::PartitionId, 1> FixedPartitions() {
    return {kState};
  }
};

// ── Config ──────────────────────────────────────────────────────────────────
struct TpchQ11Config {
  core::TableId partsupp_table = 0;
  core::TableId supplier_table = 0;
  core::TableId nation_table = 0;
};

// ── Typed bulk-scan contract ────────────────────────────────────────────────
struct TpchQ11PartsuppInput {
  int64_t partkey = 0;
  int64_t suppkey = 0;
  int64_t availqty = 0;
  int64_t supplycost_raw = 0;
  int32_t supplycost_scale = 2;
};

struct TpchQ11SupplierInput {
  int64_t suppkey = 0;
  int64_t nationkey = 0;
};

struct TpchQ11NationInput {
  int64_t nationkey = 0;
  std::string name;
};

class TpchQ11InitialScan {
public:
  virtual ~TpchQ11InitialScan() = default;
  virtual void ScanPartsupp(
      const std::function<void(const TpchQ11PartsuppInput &)> &) = 0;
  virtual void ScanSuppliers(
      const std::function<void(const TpchQ11SupplierInput &)> &) = 0;
  virtual void ScanNations(
      const std::function<void(const TpchQ11NationInput &)> &) = 0;
};

using Q11AccT = __int128_t;

// ── Output row ──────────────────────────────────────────────────────────────
struct TpchQ11OutputRow {
  int64_t partkey = 0;
  Q11AccT value = 0;
};

// ---------------------------------------------------------------------------
// State layout
//
// Q11 groups the partsupp rows of one nation by part and keeps the groups whose
// value exceeds `fraction` of the nation's total.  The state keeps
//   * the partsupp facts per part (partkey-indexed, chunked copy-on-write; up
//     to 4 offers inline = 88 B/part ~ 22 B/row, the rest in an overflow map),
//   * suppkey -> nationkey (4 B/supplier),
//   * per nation key one group: the total and the parts ordered by
//     (value DESC, partkey ASC) in a block directory (16 B/entry, ~1.35x with
//     block slack; core::CowSortedBlocks), so a delta is O(log n + block) and
//     Execute walks the ordered entries only while they exceed the threshold.
// Nation rows only supply names (resolved at query time), so nation writes
// touch nothing else; a supplier moving nation re-groups just its parts (found
// by one in-memory scan of the facts, no table scan, no rebuild).
// Values with |value| > 2^62 (impossible for TPC-H data) live in a side set so
// no data overflows; any int64 key works (AnyKeyCowArray).
//
// Memory: ~22 B/partsupp row facts + ~19 B/row ordered entries = ~41 B/row:
// SF100 (80M rows) ~3.3 GB, SF1000 (800M rows) ~33 GB (was ~15-20 GB at SF100).
// Execute: O(output rows + log n); deltas O(log n + 64 KB block copy).
// ---------------------------------------------------------------------------
constexpr int64_t kTpchQ11NoNation = std::numeric_limits<int64_t>::min();
constexpr std::size_t kTpchQ11InlineOffers = 4;
constexpr int64_t kTpchQ11NarrowLimit = int64_t{1} << 62;

struct TpchQ11Offer {
  int64_t suppkey = 0;
  int64_t cost_raw = 0;
  int32_t availqty = 0;
};

struct TpchQ11OfferSlots {
  int64_t suppkey[kTpchQ11InlineOffers] = {0, 0, 0, 0};
  int64_t cost_raw[kTpchQ11InlineOffers] = {0, 0, 0, 0};
  int32_t availqty[kTpchQ11InlineOffers] = {0, 0, 0, 0};
  uint32_t count = 0;
};

struct TpchQ11Key {
  int64_t value = 0;
  int64_t partkey = 0;
};
struct TpchQ11KeyLess {
  bool operator()(const TpchQ11Key &a, const TpchQ11Key &b) const {
    return a.value != b.value ? a.value > b.value : a.partkey < b.partkey;
  }
};
struct TpchQ11WideKey {
  Q11AccT value = 0;
  int64_t partkey = 0;
  bool operator<(const TpchQ11WideKey &o) const {
    return value != o.value ? value > o.value : partkey < o.partkey;
  }
};

// All parts with at least one offer from a supplier of one nation.
struct TpchQ11Group {
  Q11AccT total = 0;
  uint64_t parts = 0;
  core::CowSortedBlocks<TpchQ11Key, TpchQ11KeyLess> narrow;
  std::set<TpchQ11WideKey> wide_positive; // value >  2^62 (ahead of `narrow`)
  std::set<TpchQ11WideKey> wide_negative; // value < -2^62 (behind `narrow`)

  void Insert(int64_t partkey, Q11AccT value);
  void Erase(int64_t partkey, Q11AccT value);
};

struct TpchQ11NationRec {
  int64_t key = 0;
  std::string name;
};

// A part's value in one nation: present iff some offer's supplier is there.
struct TpchQ11PartValue {
  bool present = false;
  Q11AccT value = 0;
  bool operator==(const TpchQ11PartValue &o) const {
    return present == o.present && (!present || value == o.value);
  }
};

struct TpchQ11State {
  std::shared_ptr<const std::vector<TpchQ11NationRec>> nations =
      std::make_shared<const std::vector<TpchQ11NationRec>>();
  core::AnyKeyCowArray<int64_t> supplier_nation{kTpchQ11NoNation};
  core::AnyKeyCowArray<TpchQ11OfferSlots, 10> offers;
  core::ShardedCowHashMap<std::vector<TpchQ11Offer>> offer_overflow;
  std::unordered_map<int64_t, std::shared_ptr<TpchQ11Group>> groups;

  TpchQ11Offer OfferAt(const TpchQ11OfferSlots &slots, int64_t partkey,
                       uint32_t i) const;
  // Value of `partkey` in `nationkey` given a suppkey -> nationkey map.
  TpchQ11PartValue
  PartValueIn(int64_t partkey, int64_t nationkey,
              const core::AnyKeyCowArray<int64_t> &supplier_map) const;
  const TpchQ11Group *FindGroup(int64_t nationkey) const {
    const auto it = groups.find(nationkey);
    return it == groups.end() ? nullptr : it->second.get();
  }

  // ── point edits (Build and incremental maintenance) ──
  // Facts only; the caller keeps the groups in step (see ApplyPartsupp).
  void AddOffer(int64_t partkey, const TpchQ11Offer &offer);
  bool RemoveOffer(int64_t partkey, const TpchQ11Offer &offer);
  // sign > 0 adds the row, < 0 removes it; false if a removal finds no row.
  bool ApplyPartsupp(int8_t sign, int64_t partkey, const TpchQ11Offer &offer);
  void SetSupplierNation(int64_t suppkey, int64_t nationkey); // kNoNation: absent
  // Re-group the parts of suppliers whose nation changed (old_map -> current).
  void RegroupSuppliers(
      const core::FlatHashMap<uint8_t> &changed_suppliers,
      const core::AnyKeyCowArray<int64_t> &old_map);
  void UpsertNation(const TpchQ11NationInput &nation);
  void EraseNation(const TpchQ11NationInput &nation);
  // (Re)build every group from the facts and the supplier map.
  void BuildGroups();

  std::size_t MemoryBytes() const;

private:
  void UpdateEntry(int64_t nationkey, int64_t partkey,
                   const TpchQ11PartValue &before,
                   const TpchQ11PartValue &after);
  TpchQ11Group &MutableGroup(int64_t nationkey);
};

struct TpchQ11StateImage {
  std::shared_ptr<const TpchQ11State> state;
};

// ── Query arguments ─────────────────────────────────────────────────────────
// fraction = coeff / 10^scale (exact decimal); `infinity` = +1 / -1 for
// +/-Infinity (coeff unused), `nan` for NaN.
struct TpchQ11Fraction {
  Q11AccT coeff = 0;
  int scale = 0;
  int infinity = 0;
  bool nan = false;
};
// Parses a decimal text (sign, digits, '.', exponent, Infinity, NaN).
bool TpchQ11ParseFraction(std::string_view text, TpchQ11Fraction &out);

struct TpchQ11Args {
  std::optional<std::string> nation; // nullopt = SQL NULL
  std::optional<TpchQ11Fraction> fraction;
};

void TpchQ11FinalizeAndEmit(const TpchQ11State &state, const TpchQ11Args &args,
                            core::AuxRowSink &sink);

// ── Captured row changes ────────────────────────────────────────────────────
struct TpchQ11PartsuppEvent {
  int8_t sign = 0;
  int64_t partkey = 0;
  TpchQ11Offer offer;
};
struct TpchQ11SupplierEvent {
  int8_t sign = 0;
  int64_t suppkey = 0;
  int64_t nationkey = kTpchQ11NoNation;
};
struct TpchQ11NationEvent {
  int8_t sign = 0;
  TpchQ11NationInput row;
};

// ── TxnBuffer ───────────────────────────────────────────────────────────────
class TpchQ11TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;
  bool RequiresRebuild() const { return requires_rebuild; }
  void MarkRebuild() { requires_rebuild = true; }
  uint64_t EventCount() const {
    return partsupps.size() + suppliers.size() + nations.size();
  }

  std::vector<TpchQ11PartsuppEvent> partsupps;
  std::vector<TpchQ11SupplierEvent> suppliers;
  std::vector<TpchQ11NationEvent> nations;

private:
  bool requires_rebuild = false;
};

// ── Module ──────────────────────────────────────────────────────────────────
class TpchQ11Module final : public core::AuxQueryModule {
public:
  explicit TpchQ11Module(TpchQ11Config config,
                         TpchQ11InitialScan *initial_scan = nullptr)
      : config(std::move(config)), initial_scan(initial_scan) {}
  ~TpchQ11Module() override = default;

  core::AcceleratorId Id() const override;
  std::vector<core::TableDependency> Dependencies() const override;
  std::unique_ptr<core::AuxTxnBuffer> CreateTxnBuffer() const override;
  void Build(core::AuxEngineServices &, core::StateBuilder &) override;
  core::InvalidationPlan
  InterpretMutation(const core::MutationBatch &) const override;
  void AccumulateMutation(const core::MutationBatch &, core::AuxEngineServices &,
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
  TpchQ11State BuildState(core::AuxEngineServices &services) const;
  std::shared_ptr<const TpchQ11State>
  LoadState(core::AuxEngineServices &services) const;
  void EmitResult(const TpchQ11State &, const core::QueryArguments &,
                  core::AuxRowSink &) const;

  TpchQ11Config config;
  TpchQ11InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
