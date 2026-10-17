#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_query_module.hpp"
#include "prims/chunked_cow_array.hpp"
#include "prims/flat_hash_map.hpp"
#include "prims/any_key_cow_array.hpp"
#include "prims/keyed_string_column.hpp"

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
// Scans return only rows whose join / filter columns are NOT NULL (a row with a
// NULL there can never satisfy the query's predicates).
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
  // Split into `partitions` fixed row ranges: sink(partition, row) runs
  // concurrently for different partitions, one thread per partition.
  virtual void ScanLineitemsPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ20LineitemInput &)> &sink) = 0;
  virtual void ScanPartsPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ20PartInput &)> &sink) = 0;
  virtual void ScanPartsuppsPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ20PartsuppInput &)> &sink) = 0;
  virtual void ScanSuppliers(
      const std::function<void(const TpchQ20SupplierInput &)> &) = 0;
  virtual void ScanNations(
      const std::function<void(const TpchQ20NationInput &)> &) = 0;
};

// ── State ───────────────────────────────────────────────────────────────────
//
// For every (partkey, suppkey) pair that appears in partsupp and/or lineitem,
// the state keeps ps_availqty (if any) and the pair's lineitems as a sorted
// array of (shipdate, quantity) entries. Pairs are grouped by partkey into
// immutable blocks of kTpchQ20BlockParts parts (CSR layout, one allocation set
// per block), held in a copy-on-write chunked array: a write batch clones only
// the blocks of the touched parts, and nothing is ever dropped lazily (no
// overlay chain). Quantities / availqty that do not fit 31 bits live in a
// per-block `wide` table, so any int64 value is exact.
//
// Memory: 8 B per lineitem entry + 16 B per pair (~7.5 lineitems per pair in
// TPC-H => ~2.1 B/lineitem) + ~22 B/part of block overhead + part names
// (~45 B/part) => ~12 B per lineitem all-in: ~7.5 GB at SF100, ~75 GB at
// SF1000.
// Execute is a multithreaded SIMD scan of the part-name column (O(parts)),
// then O(log entries) per partsupp pair of the matching parts.
constexpr std::size_t kTpchQ20BlockParts = 8; // power of two (block = key >> 3)

struct TpchQ20Entry {
  int32_t shipdate = 0;
  int32_t qty = 0; // narrow value, or INT32_MIN + index into the block's wide table
};

struct TpchQ20Pair {
  static constexpr uint32_t kHasAvail = 0x80000000u;
  int64_t suppkey = 0;
  int32_t availqty = 0;   // narrow value or wide-table code (valid if kHasAvail)
  uint32_t entry_begin = 0; // low 31 bits: first entry; bit 31: partsupp row exists
};

struct TpchQ20PartBlock {
  uint32_t pair_begin[kTpchQ20BlockParts + 1] = {};
  std::vector<TpchQ20Pair> pairs;   // per part sorted by suppkey
  std::vector<TpchQ20Entry> entries; // per pair sorted by (shipdate, qty)
  std::vector<int64_t> wide;
};

struct TpchQ20Supplier {
  std::string name;
  std::string address;
  int32_t nationkey = 0;
};

struct TpchQ20State {
  core::AnyKeyCowArray<std::shared_ptr<TpchQ20PartBlock>, 12> blocks;
  core::KeyedStringColumn part_names;
  core::ShardedCowHashMap<TpchQ20Supplier, 12> suppliers;
  std::map<int32_t, std::string> nations;

  const TpchQ20PartBlock *FindBlock(int64_t partkey) const {
    return blocks.Get(partkey >> 3).get(); // floor: exact for negative keys
  }
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

struct TpchQ20PartDelta {
  int64_t partkey = 0;
  std::string name;
  int8_t sign = 0;
};

struct TpchQ20SupplierDelta {
  int64_t suppkey = 0;
  TpchQ20Supplier supplier;
  int8_t sign = 0;
};

struct TpchQ20NationDelta {
  int32_t nationkey = 0;
  std::string name;
  int8_t sign = 0;
};

// ── TxnBuffer ───────────────────────────────────────────────────────────────
class TpchQ20TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  const std::vector<TpchQ20LineitemDelta> &LineitemDeltas() const { return lineitem_deltas; }
  const std::vector<TpchQ20PartsuppDelta> &PartsuppDeltas() const { return partsupp_deltas; }
  const std::vector<TpchQ20PartDelta> &PartDeltas() const { return part_deltas; }
  const std::vector<TpchQ20SupplierDelta> &SupplierDeltas() const { return supplier_deltas; }
  const std::vector<TpchQ20NationDelta> &NationDeltas() const { return nation_deltas; }

  void AddLineitemDelta(TpchQ20LineitemDelta delta);
  void AddPartsuppDelta(TpchQ20PartsuppDelta delta);
  void AddPartDelta(TpchQ20PartDelta delta);
  void AddSupplierDelta(TpchQ20SupplierDelta delta);
  void AddNationDelta(TpchQ20NationDelta delta);

private:
  bool pending = false;
  std::vector<TpchQ20LineitemDelta> lineitem_deltas;
  std::vector<TpchQ20PartsuppDelta> partsupp_deltas;
  std::vector<TpchQ20PartDelta> part_deltas;
  std::vector<TpchQ20SupplierDelta> supplier_deltas;
  std::vector<TpchQ20NationDelta> nation_deltas;
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
  std::shared_ptr<TpchQ20State> BuildState(core::AuxEngineServices &services) const;
  bool LoadState(core::AuxEngineServices &services,
                 std::shared_ptr<const TpchQ20State> &state) const;
  void EmitResult(const TpchQ20State &state, const core::QueryArguments &arguments,
                  const core::AuxEngineServices &services,
                  core::AuxRowSink &sink) const;

  TpchQ20Config config;
  TpchQ20InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
