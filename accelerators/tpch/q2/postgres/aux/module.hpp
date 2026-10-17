#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_query_module.hpp"
#include "prims/chunked_cow_array.hpp"
#include "prims/any_key_cow_array.hpp"
#include "prims/chunked_blob_map.hpp"
#include "prims/flat_hash_map.hpp"
#include "prims/string_blob.hpp"

namespace accelerator_codegen::aux_state::query {

struct TpchQ2Config {
  core::TableId part_table = 0;
  core::TableId supplier_table = 0;
  core::TableId partsupp_table = 0;
  core::TableId nation_table = 0;
  core::TableId region_table = 0;
};

struct TpchQ2PartInput {
  int64_t key = 0;
  std::string mfgr;
  std::string type;
  int32_t size = 0;
};

struct TpchQ2SupplierInput {
  int64_t key = 0;
  std::string name;
  std::string address;
  int64_t nationkey = 0;
  std::string phone;
  int64_t acctbal_raw = 0;
  std::string comment;
};

struct TpchQ2PartsuppInput {
  int64_t partkey = 0;
  int64_t suppkey = 0;
  int64_t supplycost_raw = 0;
};

struct TpchQ2NationInput {
  int64_t key = 0;
  std::string name;
  int64_t regionkey = 0;
  bool regionkey_null = false; // captured rows only: n_regionkey IS NULL
};

struct TpchQ2RegionInput {
  int64_t key = 0;
  std::string name;
};

class TpchQ2InitialScan {
public:
  virtual ~TpchQ2InitialScan() = default;
  // Split into `partitions` fixed row ranges: sink(partition, row) runs
  // concurrently for different partitions, one thread per partition.
  virtual void ScanParts(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ2PartInput &)> &sink) = 0;
  virtual void ScanSuppliers(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ2SupplierInput &)> &sink) = 0;
  virtual void ScanPartsupps(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ2PartsuppInput &)> &sink) = 0;
  virtual void ScanNationsSerial(
      const std::function<void(const TpchQ2NationInput &)> &) = 0;
  virtual void ScanRegionsSerial(
      const std::function<void(const TpchQ2RegionInput &)> &) = 0;
};

// ---------------------------------------------------------------------------
// State layout (columnar, copy-on-write, no cap on key values)
//
// Q2 answers "cheapest regional offer per qualifying part", so the state keeps
// the BASE FACTS compactly instead of a precomputed per-(region, part) minimum:
//   * part columns keyed by partkey: p_size (4 B), p_type id (4 B, dictionary
//     encoded), p_mfgr id (4 B, dictionary encoded);
//   * the partsupp offers of each part inline in its partkey slot (16 B per
//     offer, up to 4 inline, the rest in a sharded overflow map);
//   * supplier columns keyed by suppkey: nationkey + acctbal (16 B) and one
//     packed text blob (name/address/phone/comment, 8 B + bytes);
//   * the (tiny) nation and region tables.
// Because nothing is derived across tables, every write (any of the five
// tables) is a point edit of one chunk: nation/region/supplier changes simply
// change what Execute sees, and there is never a rebuild for routine writes.
// All arrays are ChunkedCowArray / ChunkedBlobMap, so a published state
// is copied as chunk pointers and a write batch clones only the touched chunks.
//
// Memory: ~12 B/part + 72 B/part for up to 4 offers (= 18 B per partsupp row at
// 4 offers/part) + ~150 B/supplier.  SF100 (20M parts, 80M partsupp, 1M
// suppliers): ~1.9 GB; SF1000 (200M parts, 800M partsupp, 10M suppliers):
// ~19 GB.
// Execute: a size-column scan over all parts (4 B/part, sliced for large
// tables) plus, for the ~1/250 parts that also match the type bitmap,
// at most a few supplier lookups: ~25 ms at SF100, ~0.1-0.2 s at SF1000.
// Any int64 key works: keys in [-2^35, 2^35) are zigzag-mapped into the dense
// chunks, the rest live in sharded hash maps (AnyKeyCowArray).
// ---------------------------------------------------------------------------
constexpr int32_t kTpchQ2NoSize = std::numeric_limits<int32_t>::min();
constexpr uint32_t kTpchQ2NoId = 0xFFFFFFFFu;
constexpr std::size_t kTpchQ2InlineOffers = 4;

struct TpchQ2Offer {
  int64_t suppkey = 0;
  int64_t cost_raw = 0;
};

// All partsupp offers of one part: the first kTpchQ2InlineOffers inline, the
// remainder (count > 4) in TpchQ2State::offer_overflow[partkey].
struct TpchQ2OfferSlots {
  int64_t suppkey[kTpchQ2InlineOffers] = {0, 0, 0, 0};
  int64_t cost_raw[kTpchQ2InlineOffers] = {0, 0, 0, 0};
  uint32_t count = 0;
};

struct TpchQ2SupplierRec {
  int64_t acctbal_raw = 0;
  int64_t nationkey = 0;
  uint8_t present = 0;
};

struct TpchQ2NationRec {
  int64_t key = 0;
  std::string name;
  int64_t regionkey = 0;
};

struct TpchQ2RegionRec {
  int64_t key = 0;
  std::string name;
};

// name/address/phone/comment of one supplier packed in a single blob value.
struct TpchQ2SupplierText {
  std::string_view name, address, phone, comment;
};
std::string TpchQ2PackSupplierText(const std::string &name,
                                   const std::string &address,
                                   const std::string &phone,
                                   const std::string &comment);
TpchQ2SupplierText TpchQ2UnpackSupplierText(std::string_view packed);

struct TpchQ2State {
  std::shared_ptr<const std::vector<TpchQ2RegionRec>> regions;
  std::shared_ptr<const std::vector<TpchQ2NationRec>> nations;
  // Dictionaries are append-only and cloned only when a write introduces a
  // value the shared copy does not have yet.
  std::shared_ptr<core::StringDict> types = std::make_shared<core::StringDict>();
  std::shared_ptr<core::StringDict> mfgrs = std::make_shared<core::StringDict>();

  core::AnyKeyCowArray<int32_t> part_size{kTpchQ2NoSize};
  core::AnyKeyCowArray<uint32_t> part_type{kTpchQ2NoId};
  core::AnyKeyCowArray<uint32_t> part_mfgr{kTpchQ2NoId};
  core::AnyKeyCowArray<TpchQ2OfferSlots, 10> offers;
  core::ShardedCowHashMap<std::vector<TpchQ2Offer>> offer_overflow;

  core::AnyKeyCowArray<TpchQ2SupplierRec> suppliers;
  core::ChunkedBlobMap supplier_text;

  TpchQ2State() {
    regions = std::make_shared<const std::vector<TpchQ2RegionRec>>();
    nations = std::make_shared<const std::vector<TpchQ2NationRec>>();
  }

  // --- point edits (used by Build and by incremental maintenance) ----------
  // A part with NULL p_size / p_type can never qualify, so it is simply absent.
  void SetPart(int64_t partkey, const TpchQ2PartInput &part);
  // SetPart with the dictionary ids already known.
  void SetPartIds(int64_t partkey, int32_t size, uint32_t type_id,
                  uint32_t mfgr_id);
  void ErasePart(int64_t partkey);
  void SetSupplier(int64_t suppkey, const TpchQ2SupplierInput &supplier);
  void EraseSupplier(int64_t suppkey);
  void AddOffer(int64_t partkey, int64_t suppkey, int64_t cost_raw);
  // False when no such offer exists (state out of sync with the table).
  bool RemoveOffer(int64_t partkey, int64_t suppkey, int64_t cost_raw);
  void UpsertNation(const TpchQ2NationInput &nation);
  void EraseNation(const TpchQ2NationInput &nation);
  void UpsertRegion(const TpchQ2RegionInput &region);
  void EraseRegion(const TpchQ2RegionInput &region);

  // Offer `i` (0 <= i < slots.count) of the part whose slots are `slots`.
  TpchQ2Offer OfferAt(const TpchQ2OfferSlots &slots, int64_t partkey,
                      uint32_t i) const;

  std::size_t MemoryBytes() const;

private:
  void InternInto(std::shared_ptr<core::StringDict> &dict,
                  const std::string &value, uint32_t &id);
};

struct TpchQ2StateImage {
  std::shared_ptr<const TpchQ2State> state;
};

// One captured row change.  sign +1 = row inserted / new image, -1 = row
// deleted / old image; per-table order is the order of application.
struct TpchQ2PartEvent {
  int8_t sign = 0;
  TpchQ2PartInput row;
};
struct TpchQ2SupplierEvent {
  int8_t sign = 0;
  TpchQ2SupplierInput row;
};
struct TpchQ2PartsuppEvent {
  int8_t sign = 0;
  TpchQ2PartsuppInput row;
};
struct TpchQ2NationEvent {
  int8_t sign = 0;
  TpchQ2NationInput row;
};
struct TpchQ2RegionEvent {
  int8_t sign = 0;
  TpchQ2RegionInput row;
};

// Query arguments; std::nullopt = SQL NULL (the predicate is then never true).
struct TpchQ2Args {
  std::optional<int64_t> size;
  std::optional<std::string> type_pattern;
  std::optional<std::string> region_name;
};

void TpchQ2FinalizeAndEmit(const core::AuxEngineServices &services,
                           const TpchQ2State &state, const TpchQ2Args &args,
                           core::AuxRowSink &sink);

constexpr std::size_t kTpchQ2ResultLimit = 100;

class TpchQ2TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const { return requires_rebuild; }
  void MarkRebuild();

  std::vector<TpchQ2PartEvent> parts;
  std::vector<TpchQ2SupplierEvent> suppliers;
  std::vector<TpchQ2PartsuppEvent> partsupps;
  std::vector<TpchQ2NationEvent> nations;
  std::vector<TpchQ2RegionEvent> regions;
  uint64_t EventCount() const {
    return parts.size() + suppliers.size() + partsupps.size() +
           nations.size() + regions.size();
  }

private:
  bool requires_rebuild = false;
};

class TpchQ2Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = "tpch_q2";

  explicit TpchQ2Module(TpchQ2Config config,
                        TpchQ2InitialScan *initial_scan = nullptr)
      : config(config), initial_scan(initial_scan) {}

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
  std::vector<core::CalibrationWrite> CalibrationWrites() const override;

private:
  TpchQ2State BuildState(core::AuxEngineServices &) const;
  std::shared_ptr<const TpchQ2State>
  LoadState(core::AuxEngineServices &) const;
  void EmitResult(const core::AuxEngineServices &, const TpchQ2State &,
                  const core::QueryArguments &, core::AuxRowSink &) const;

  TpchQ2Config config;
  TpchQ2InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
