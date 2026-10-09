#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_query_module.hpp"
#include "sec_edgar/q6/layout.hpp"
#include "sec_edgar/q6/common/state.hpp"

namespace accelerator_codegen::aux_state::query {

// Table ids are assigned by the engine adapter (num=1, sub=2, pre=3).
struct SecEdgarQ6Config {
  core::TableId num_table = 0;
  core::TableId sub_table = 0;
  core::TableId pre_table = 0;
};

// Column positions inside the captured mutation rows. The module projects
// the columns it reads (TableDependency::mutation_attnums = the physical
// attribute numbers below), so a captured AuxRow holds exactly these, in this
// order.
struct SecEdgarQ6Columns {
  // num(adsh=0, tag=1, version=2, uom=5, value=7)
  static constexpr core::ColumnId kNumAdsh = 0, kNumTag = 1, kNumVersion = 2,
                                  kNumUom = 3, kNumValue = 4;
  // sub(adsh=0, name=2, fy=10)
  static constexpr core::ColumnId kSubAdsh = 0, kSubName = 1, kSubFy = 2;
  // pre(adsh=0, stmt=3, tag=6, version=7, plabel=8)
  static constexpr core::ColumnId kPreAdsh = 0, kPreStmt = 1, kPreTag = 2,
                                  kPreVersion = 3, kPrePlabel = 4;
  static constexpr int32_t kNumAttnums[] = {0, 1, 2, 5, 7};
  static constexpr int32_t kSubAttnums[] = {0, 2, 10};
  static constexpr int32_t kPreAttnums[] = {0, 3, 6, 7, 8};
};

struct SecEdgarQ6NumInput {
  std::string adsh;
  std::string tag;
  std::string version;
  std::string uom;
  double value = 0.0;
  bool value_is_null = true;
};

struct SecEdgarQ6SubInput {
  std::string adsh;
  std::string name;
  int32_t fy = 0;
  bool name_is_null = false;
  bool fy_is_null = false;
};

struct SecEdgarQ6PreInput {
  std::string adsh;
  std::string tag;
  std::string version;
  std::string stmt;
  std::string plabel;
  bool plabel_is_null = false;
};

struct SecEdgarQ6GroupAggregateInput {
  int32_t fy = 0;
  std::string stmt;
  std::string uom;
  std::string name;
  std::string tag;
  std::string plabel;
  bool name_is_null = false;
  bool plabel_is_null = false;
  double sum = 0.0;
  int64_t count = 0;
};

// Post-transaction state of one adsh, read from the base tables:
//   in_cohort/fy/name: sub row with a non-NULL fy, pre: its rows (stmt, tag,
//   version non-NULL) grouped by (tag, version, stmt, plabel) with
//   multiplicity, nums: non-NULL value sums and counts per (tag, version, uom).
struct SecEdgarQ6AdshSnapshot {
  bool in_cohort = false;
  int32_t fy = 0;
  std::string name;
  bool name_is_null = false;
  std::vector<SecEdgarQ6PreEntry> pre;
  std::vector<SecEdgarQ6NumSum> nums;
};

// Initial-scan contract.
//   * ScanSub / ScanPre / ScanNum may be pre-filtered by the implementation to
//     the rows the query can use (sub.fy IS NOT NULL, pre rows of those adsh
//     with stmt / tag / version NOT NULL, num rows with uom and value NOT
//     NULL). Consumers re-check every predicate.
//   * ScanGroupAggregates is an optional fast path: the engine computes
//     (fy, name, stmt, tag, plabel, uom) -> (SUM(value), COUNT(*)) of the query
//     body without the parameters and the final ORDER/LIMIT. Return false when
//     unsupported.
class SecEdgarQ6InitialScan {
public:
  virtual ~SecEdgarQ6InitialScan() = default;
  virtual void ScanNum(
      const std::function<void(const SecEdgarQ6NumInput &)> &) = 0;
  virtual void ScanSub(
      const std::function<void(const SecEdgarQ6SubInput &)> &) = 0;
  virtual void ScanPre(
      const std::function<void(const SecEdgarQ6PreInput &)> &) = 0;
  virtual bool ScanGroupAggregates(
      const std::function<void(const SecEdgarQ6GroupAggregateInput &)> &) {
    return false;
  }
  // Optional: targeted per-adsh lookups used by incremental sub/pre
  // maintenance. Return false when unsupported (the module then rebuilds).
  virtual bool LoadAdshSnapshots(const std::vector<std::string> &,
                                 std::vector<SecEdgarQ6AdshSnapshot> &) {
    return false;
  }
};

// Published image: the whole (immutable) state lives in the aggregate
// partition so a single atomic publish moves structure, aggregates and top
// cache together.
struct SecEdgarQ6StateImage {
  std::shared_ptr<const SecEdgarQ6State> state;
};

// Compact, arena-backed transaction buffer.
class SecEdgarQ6TxnBuffer final : public core::AuxTxnBuffer {
public:
  struct NumDelta {
    uint32_t offset = 0; // into the arena: adsh | tag | version
    uint32_t adsh_len = 0;
    uint32_t tag_len = 0;
    uint32_t version_len = 0;
    uint32_t uom_len = 0;
    double value = 0.0;
    int8_t sign = 0;
  };
  struct SubChange {
    std::string adsh;
    std::string name;
    bool name_is_null = false;
    int8_t sign = 0;
  };
  struct PreChange {
    std::string adsh;
    std::string tag;
    std::string version;
    std::string stmt;
    std::string plabel;
    bool plabel_is_null = false;
    int8_t sign = 0;
  };

  bool HasPendingChanges() const override { return pending_; }
  void Clear() override;

  bool RequiresRebuild() const { return requires_rebuild_; }
  void MarkRebuild() {
    pending_ = true;
    requires_rebuild_ = true;
  }
  void AddNumDelta(std::string_view adsh, std::string_view tag,
                   std::string_view version, std::string_view uom, double value,
                   int8_t sign);
  void AddSubChange(SubChange change) {
    pending_ = true;
    sub_changes_.push_back(std::move(change));
  }
  void AddPreChange(PreChange change) {
    pending_ = true;
    pre_changes_.push_back(std::move(change));
  }

  const std::vector<NumDelta> &NumDeltas() const { return num_deltas_; }
  std::string_view Adsh(const NumDelta &d) const {
    return {arena_.data() + d.offset, d.adsh_len};
  }
  std::string_view Tag(const NumDelta &d) const {
    return {arena_.data() + d.offset + d.adsh_len, d.tag_len};
  }
  std::string_view Version(const NumDelta &d) const {
    return {arena_.data() + d.offset + d.adsh_len + d.tag_len, d.version_len};
  }
  std::string_view Uom(const NumDelta &d) const {
    return {arena_.data() + d.offset + d.adsh_len + d.tag_len + d.version_len,
            d.uom_len};
  }
  const std::vector<SubChange> &SubChanges() const { return sub_changes_; }
  const std::vector<PreChange> &PreChanges() const { return pre_changes_; }

private:
  bool pending_ = false;
  bool requires_rebuild_ = false;
  std::vector<char> arena_;
  std::vector<NumDelta> num_deltas_;
  std::vector<SubChange> sub_changes_;
  std::vector<PreChange> pre_changes_;
};

class SecEdgarQ6Module : public core::AuxQueryModule {
public:
  static constexpr const char *kId = core::SecEdgarQ6Layout::kAccelerator;

  explicit SecEdgarQ6Module(SecEdgarQ6Config config,
                            SecEdgarQ6InitialScan *initial_scan = nullptr)
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

  // Fresh state from the initial-scan source (also used by the fallback path).
  std::shared_ptr<const SecEdgarQ6State> BuildState() const;

private:
  void EmitRows(const std::vector<SecEdgarQ6OutputRow> &, core::AuxRowSink &) const;
  std::shared_ptr<const SecEdgarQ6State>
  LoadPublishedState(core::AuxEngineServices &, core::Version *version) const;

  SecEdgarQ6Config config;
  SecEdgarQ6InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
