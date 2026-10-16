#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_query_module.hpp"

namespace accelerator_codegen::aux_state::query {

// ── Q16 layout ──────────────────────────────────────────────────────────────
// The state is a set of small copy-on-write pages, so a point write to part,
// partsupp or supplier republishes only the few pages it touches:
//   0                      meta: brand/type dictionaries + sort ranks
//   1..50                  per-p_size result list, sorted (count DESC, brand,
//                          type); read = k-way merge of the requested sizes
//   kGroupBase + ...       per (p_size, brand) page: supplier ref counts of
//                          each (brand, type, size) group + eligible count
//   kPartBase + ...        partkey -> (brand, type, size) pages
//   kPsBase + ...          partkey -> suppkeys pages (all partsupp rows)
//   kSuppBase + ...        suppkey -> eligibility + groups-containing pages
struct TpchQ16Layout {
  static constexpr const char *kAccelerator = "tpch_q16_aux";
  static constexpr core::PartitionId kMeta = 0;
  static constexpr core::PartitionId kSizeBase = 1;
  static constexpr int32_t kMinSize = 1;
  static constexpr int32_t kMaxSize = 50;
  static constexpr core::PartitionId kGroupBase = 100;
  static constexpr uint32_t kBrandCap = 256;
  // Page ranges, 2^28 partitions apart: the store keys partitions as int32
  // (state/partition_key.hpp), so every id must stay below 2^31.
  static constexpr core::PartitionId kPartBase = core::PartitionId{1} << 28;
  static constexpr unsigned kPartShift = 13;
  static constexpr core::PartitionId kPsBase = core::PartitionId{2} << 28;
  static constexpr unsigned kPsShift = 12;
  static constexpr core::PartitionId kSuppBase = core::PartitionId{3} << 28;
  static constexpr unsigned kSuppShift = 10;
  // Supported key domain for partkey / suppkey: >= 0 and below 2^36, so each
  // page range (key >> shift < 2^26) stays inside its block (the build indexes
  // pages densely, so a key near the limit costs a large page table).
  static constexpr int64_t kKeyLimit = int64_t{1} << 36;

  static constexpr bool KeyInRange(int64_t key) {
    return key >= 0 && key < kKeyLimit;
  }
  static constexpr core::PartitionId SizePartition(int32_t p_size) {
    return kSizeBase + static_cast<core::PartitionId>(p_size);
  }
  static constexpr core::PartitionId GroupPartition(int32_t p_size,
                                                    uint32_t brand_id) {
    return kGroupBase +
           static_cast<core::PartitionId>(p_size - 1) * kBrandCap + brand_id;
  }
  static constexpr core::PartitionId PartPartition(int64_t partkey) {
    return kPartBase + static_cast<core::PartitionId>(partkey >> kPartShift);
  }
  static constexpr core::PartitionId PsPartition(int64_t partkey) {
    return kPsBase + static_cast<core::PartitionId>(partkey >> kPsShift);
  }
  static constexpr core::PartitionId SuppPartition(int64_t suppkey) {
    return kSuppBase + static_cast<core::PartitionId>(suppkey >> kSuppShift);
  }
};

// ── Config ──────────────────────────────────────────────────────────────────
struct TpchQ16Config {
  core::TableId part_table = 0;
  core::TableId partsupp_table = 0;
  core::TableId supplier_table = 0;
};

// ── Typed bulk-scan contract ────────────────────────────────────────────────
struct TpchQ16SupplierInput {
  int64_t suppkey = 0;
  std::string comment;
};

struct TpchQ16PartInput {
  int64_t partkey = 0;
  std::string brand;
  std::string type;
  int32_t size = 0;
};

struct TpchQ16PartsuppInput {
  int64_t partkey = 0;
  int64_t suppkey = 0;
};

class TpchQ16InitialScan {
public:
  virtual ~TpchQ16InitialScan() = default;
  // Partitioned scans for builds: rows of one partition arrive in table order
  // on one thread; partitions run concurrently on the pool.
  virtual void ScanSuppliers(
      const core::AuxEngineServices &, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ16SupplierInput &)> &) = 0;
  virtual void ScanParts(
      const core::AuxEngineServices &, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ16PartInput &)> &) = 0;
  virtual void ScanPartsupp(
      const core::AuxEngineServices &, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ16PartsuppInput &)> &) = 0;
};

// ── State payloads (each published image is immutable) ─────────────────────
struct TpchQ16Meta {
  std::vector<std::string> brands;
  std::vector<std::string> types;
  // Rank of an id in lexicographic order (ids are assigned in arrival order).
  std::vector<uint16_t> brand_rank;
  std::vector<uint16_t> type_rank;
  // Inverse of the ranks: id of the name with a given lexicographic rank.
  std::vector<uint16_t> brand_by_rank;
  std::vector<uint16_t> type_by_rank;
  std::unordered_map<std::string, uint16_t> brand_index;
  std::unordered_map<std::string, uint16_t> type_index;
};

// Result row of one group with a non-zero eligible supplier count.
struct Q16PrecomputedRow {
  uint16_t brand_id = 0;
  uint16_t type_id = 0;
  int32_t size = 0;
  int64_t count = 0;
};

struct TpchQ16SizeList {
  std::vector<Q16PrecomputedRow> rows; // (count DESC, brand rank, type rank)
};

// One (brand, type, size) group: sorted distinct suppliers with the number of
// partsupp rows (over the group's parts) that reference them.
struct TpchQ16GroupMembers {
  uint16_t type_id = 0;
  int32_t eligible_count = 0;
  std::vector<int64_t> suppkeys;
  std::vector<int32_t> ref_counts;
};

struct TpchQ16GroupPage {
  std::vector<TpchQ16GroupMembers> groups; // sorted by type_id
};

struct TpchQ16PartInfo {
  uint16_t brand_id = 0xFFFF; // 0xFFFF: no such part
  uint16_t type_id = 0;
  int32_t size = 0;
};

struct TpchQ16PartPage {
  std::vector<TpchQ16PartInfo> info;
  void Init() {
    info.assign(std::size_t{1} << TpchQ16Layout::kPartShift, TpchQ16PartInfo{});
  }
};

// Compressed rows: row r is data[off[r] .. off[r+1]).
template <class T> struct TpchQ16Csr {
  std::vector<uint32_t> off;
  std::vector<T> data;
  void Init(std::size_t rows) {
    off.assign(rows + 1, 0);
    data.clear();
  }
  const T *Begin(std::size_t r) const { return data.data() + off[r]; }
  const T *End(std::size_t r) const { return data.data() + off[r + 1]; }
  void Insert(std::size_t r, const T &value) {
    data.insert(data.begin() + static_cast<std::ptrdiff_t>(off[r + 1]), value);
    for (std::size_t i = r + 1; i < off.size(); ++i)
      ++off[i];
  }
  bool Erase(std::size_t r, const T &value) {
    auto first = data.begin() + static_cast<std::ptrdiff_t>(off[r]);
    auto last = data.begin() + static_cast<std::ptrdiff_t>(off[r + 1]);
    auto it = std::find(first, last, value);
    if (it == last)
      return false;
    data.erase(it);
    for (std::size_t i = r + 1; i < off.size(); ++i)
      --off[i];
    return true;
  }
};

struct TpchQ16PartsuppPage {
  TpchQ16Csr<int64_t> rows; // partkey -> suppkeys (every partsupp row)
  void Init() { rows.Init(std::size_t{1} << TpchQ16Layout::kPsShift); }
};

struct TpchQ16SupplierPage {
  std::vector<uint8_t> eligible; // default 1: no row / not a complaint supplier
  // suppkey -> codes of the groups it currently belongs to (ref count > 0)
  TpchQ16Csr<uint32_t> groups;
  void Init() {
    const std::size_t n = std::size_t{1} << TpchQ16Layout::kSuppShift;
    eligible.assign(n, 1);
    groups.Init(n);
  }
};

// Result of a full build: every image of the accelerator.
struct TpchQ16State {
  std::vector<core::StateImage> images;
};

struct Q16ResultRow {
  std::string brand;
  std::string type;
  uint16_t brand_id = 0;
  uint16_t type_id = 0;
  int32_t size = 0;
  int64_t count = 0;
};

// ── Deltas ──────────────────────────────────────────────────────────────────
// Applied in arrival order against the published pages (order independent in
// effect: partsupp rows are kept for parts that do not exist).
struct TpchQ16Op {
  enum class Kind : uint8_t {
    PartDelete,
    PartInsert,
    PartsuppDelete,
    PartsuppInsert,
    SupplierSet
  };
  Kind kind = Kind::SupplierSet;
  int64_t key = 0;  // partkey, or suppkey for SupplierSet
  int64_t key2 = 0; // suppkey of a partsupp op
  int32_t size = 0;
  bool eligible = true;
  std::string brand;
  std::string type;
};

// ── Predicates ──────────────────────────────────────────────────────────────
bool TpchQ16IsComplaintSupplier(const std::string &comment);
bool TpchQ16TypeNotLike(const std::string &type, const std::string &pattern);

// ── TxnBuffer ───────────────────────────────────────────────────────────────
class TpchQ16TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  const std::vector<TpchQ16Op> &Ops() const;
  void MarkRebuild();
  void AddOp(TpchQ16Op op);

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<TpchQ16Op> ops;
};

// ── Module ──────────────────────────────────────────────────────────────────
class TpchQ16Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = TpchQ16Layout::kAccelerator;

  explicit TpchQ16Module(TpchQ16Config config,
                         TpchQ16InitialScan *initial_scan = nullptr)
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
  TpchQ16State BuildState(core::AuxEngineServices &) const;

  TpchQ16Config config;
  TpchQ16InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
