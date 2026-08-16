#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_query_module.hpp"
#include "accelerator_codegen/stateful/core/query_layouts.hpp"

namespace accelerator_codegen::aux_state::query {

// ── Q16 layout ──────────────────────────────────────────────────────────────
// Partition 0: eligibility + intern + domain-bounded intermediate sorted group
// counts (Execute filters brand/type — not final answers). Group cardinality is
// TPC-H domain-bounded, so the read scan stays SF20-safe.
// Partitions 1..50: size membership + eligible_count (SF-scaled supplier blocks).
// Partition 51: supplier→group reverse index for point maintenance that only
// COW-copies touched sizes (avoids deep-copying all size images at SF20).
struct TpchQ16Layout {
  static constexpr const char *kAccelerator = "tpch_q16_aux";
  static constexpr core::PartitionId kSupplierEligibility = 0;
  static constexpr core::PartitionId kSizeBase = 1;
  static constexpr int32_t kMinSize = 1;
  static constexpr int32_t kMaxSize = 50;
  // Separate from eligibility so the hot read image stays compact at SF20.
  static constexpr core::PartitionId kSupplierGroupRefs =
      kSizeBase + static_cast<core::PartitionId>(kMaxSize) + 1;

  static constexpr core::PartitionId SizePartition(int32_t p_size) {
    return kSizeBase + static_cast<core::PartitionId>(p_size);
  }
  static constexpr int32_t SizeFromPartition(core::PartitionId partition) {
    return static_cast<int32_t>(partition - kSizeBase);
  }
  static constexpr bool IsSizePartition(core::PartitionId partition) {
    return partition >= kSizeBase &&
           partition <= kSizeBase + static_cast<core::PartitionId>(kMaxSize);
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
  virtual void ScanSuppliers(
      const std::function<void(const TpchQ16SupplierInput &)> &) = 0;
  virtual void ScanParts(
      const std::function<void(const TpchQ16PartInput &)> &) = 0;
  virtual void ScanPartsupp(
      const std::function<void(const TpchQ16PartsuppInput &)> &) = 0;
};

// ── State payloads ──────────────────────────────────────────────────────────
// Supplier eligibility is stored as a byte vector indexed by suppkey.
// Size partitions hold sparse supplier blocks; each block is keyed by
// floor(suppkey / block_width) to make both lookup and updates bounded.
struct TpchQ16SupplierBlock {
  std::vector<int64_t> suppkeys;
  std::vector<int32_t> ref_counts;
};

struct TpchQ16GroupMembership {
  std::string brand;
  std::string type;
  uint16_t brand_id = 0;
  uint16_t type_id = 0;
  // Cached distinct eligible suppliers (intermediate). Enables O(groups)
  // supplier point maintenance without rescanning supplier blocks (SF-safe).
  int64_t eligible_count = 0;
  std::unordered_map<int64_t, TpchQ16SupplierBlock> blocks;
};

struct TpchQ16SizePartition {
  std::vector<TpchQ16GroupMembership> groups;
};

// Intermediate per-(brand,type,size) eligible counts — NOT the final query
// answer (brand/type filters apply at Execute). Domain-bounded cardinality
// (TPC-H brand×type×size), so the read scan stays SF20-safe.
struct Q16PrecomputedRow {
  uint16_t brand_id = 0;
  uint16_t type_id = 0;
  int32_t size = 0;
  int64_t count = 0;
};

struct TpchQ16State {
  // Hot read fields first (Execute touches these only).
  std::vector<uint8_t> supplier_eligible;
  std::vector<std::string> brand_intern;
  std::vector<std::string> type_intern;
  std::vector<Q16PrecomputedRow> precomputed_rows;
  // Point-maintenance reverse index: packed (size << 32) | group_index.
  // Cold on the read path; lets supplier flips touch O(groups_for_supplier)
  // partitions instead of deep-copying all size images (SF-critical).
  std::unordered_map<int64_t, std::vector<uint64_t>> supplier_group_refs;

  // Build-only catalogs — stripped before publishing the eligibility image.
  std::unordered_map<int64_t, std::string> part_brand;
  std::unordered_map<int64_t, std::string> part_type;
  std::unordered_map<int64_t, int32_t> part_size;
  std::unordered_map<int64_t, std::vector<int64_t>> reverse_postings;
  std::unordered_map<int32_t, TpchQ16SizePartition> membership;

  static constexpr int64_t kBlockWidth = 4096;

  static int64_t BlockIndex(int64_t suppkey) {
    if (suppkey < 0)
      suppkey = -suppkey;
    return suppkey / kBlockWidth;
  }

  static uint64_t PackGroupRef(int32_t size, uint32_t group_index) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(size)) << 32) |
           static_cast<uint64_t>(group_index);
  }
  static int32_t UnpackSize(uint64_t ref) {
    return static_cast<int32_t>(ref >> 32);
  }
  static uint32_t UnpackGroupIndex(uint64_t ref) {
    return static_cast<uint32_t>(ref & 0xffffffffu);
  }

  static std::string MakeGroupKey(const std::string &brand,
                                  const std::string &type) {
    return brand + '\x1f' + type;
  }

  bool IsSupplierEligible(int64_t suppkey) const {
    if (suppkey < 0 ||
        static_cast<std::size_t>(suppkey) >= supplier_eligible.size())
      return false;
    return supplier_eligible[static_cast<std::size_t>(suppkey)] != 0;
  }

  void EnsureSupplierCapacity(int64_t max_suppkey) {
    if (max_suppkey < 0)
      return;
    if (static_cast<std::size_t>(max_suppkey) >= supplier_eligible.size())
      supplier_eligible.resize(static_cast<std::size_t>(max_suppkey) + 1, 0);
  }

  void SetSupplierEligible(int64_t suppkey, bool eligible) {
    if (suppkey < 0)
      return;
    EnsureSupplierCapacity(suppkey);
    supplier_eligible[static_cast<std::size_t>(suppkey)] = eligible ? 1 : 0;
  }
};

enum class TpchQ16ImageKind : uint8_t {
  Eligibility,
  SizeMembership,
  SupplierGroupRefs
};

struct TpchQ16StateImage {
  TpchQ16ImageKind kind = TpchQ16ImageKind::Eligibility;
  int32_t size = 0;
  std::shared_ptr<const TpchQ16State> state;
  std::shared_ptr<const TpchQ16SizePartition> size_membership;
  std::shared_ptr<const std::unordered_map<int64_t, std::vector<uint64_t>>>
      supplier_group_refs;
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
struct TpchQ16PartsuppDelta {
  int64_t partkey = 0;
  int64_t suppkey = 0;
  int8_t sign = 0; // +1 insert, -1 delete
  int32_t old_size = 0;
  int32_t new_size = 0;
  std::string old_brand;
  std::string old_type;
  std::string new_brand;
  std::string new_type;
};

struct TpchQ16SupplierDelta {
  int64_t suppkey = 0;
  bool old_eligible = false;
  bool new_eligible = false;
};

// ── Encode / Decode ─────────────────────────────────────────────────────────
std::vector<uint8_t> EncodeTpchQ16Eligibility(const TpchQ16State &state);
std::vector<uint8_t> EncodeTpchQ16SizeMembership(
    int32_t size, const TpchQ16SizePartition &membership);
bool DecodeTpchQ16Eligibility(const std::vector<uint8_t> &bytes,
                              TpchQ16State &state);
bool DecodeTpchQ16SizeMembership(const std::vector<uint8_t> &bytes,
                                 int32_t size, TpchQ16SizePartition &membership);

// ── Predicates ──────────────────────────────────────────────────────────────
bool TpchQ16IsComplaintSupplier(const std::string &comment);
bool TpchQ16TypeNotLike(const std::string &type, const std::string &pattern);

// ── TxnBuffer ───────────────────────────────────────────────────────────────
class TpchQ16TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  const std::vector<TpchQ16PartsuppDelta> &PartsuppDeltas() const;
  const std::vector<TpchQ16SupplierDelta> &SupplierDeltas() const;
  void MarkRebuild();
  void AddPartsuppDelta(TpchQ16PartsuppDelta delta);
  void AddSupplierDelta(TpchQ16SupplierDelta delta);

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<TpchQ16PartsuppDelta> partsupp_deltas;
  std::vector<TpchQ16SupplierDelta> supplier_deltas;
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
  void ExecuteFallback(core::AuxEngineServices &, const core::QueryArguments &,
                       core::AuxRowSink &) override;

private:
  TpchQ16State BuildState(core::AuxEngineServices &) const;
  bool LoadSizePartition(core::AuxEngineServices &, int32_t size,
                         TpchQ16SizePartition &) const;
  bool LoadEligibility(core::AuxEngineServices &,
                       std::vector<uint8_t> &eligible) const;
  void EmitResult(
      const std::vector<Q16ResultRow> &rows,
      core::AuxRowSink &sink) const;

  TpchQ16Config config;
  TpchQ16InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
