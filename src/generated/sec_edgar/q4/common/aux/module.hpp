#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_query_module.hpp"
#include "sec_edgar/q4/layout.hpp"
#include "sec_edgar/q4/common/aux/state.hpp"

namespace accelerator_codegen::aux_state::query {

struct SecEdgarQ4Config {
  core::TableId num_table = 0;
  core::TableId sub_table = 0;
  core::TableId tag_table = 0;
  core::TableId pre_table = 0;
};

// Mutation row layout (physical column order of the base tables; trailing
// columns may be omitted):
//   num: 0 adsh, 1 tag, 2 version, 3 ddate, 4 qtrs, 5 uom, 6 coreg, 7 value
//   sub: 0 adsh, 1 cik, 2 name, 3 sic
//   tag: 0 tag, 1 version, 2 custom, 3 abstract, ..., 7 tlabel
//   pre: 0 adsh, 1 report, 2 line, 3 stmt, 4 inpth, 5 rfile, 6 tag, 7 version
// Only identity columns are read for sub/tag/pre rows: those tables are
// maintained by re-reading the affected keys from the base tables.
inline constexpr core::ColumnId kQ4NumAdsh = 0, kQ4NumTag = 1, kQ4NumVersion = 2,
                                 kQ4NumUom = 5, kQ4NumValue = 7;
inline constexpr core::ColumnId kQ4SubAdsh = 0, kQ4SubCik = 1, kQ4SubSic = 3;
inline constexpr core::ColumnId kQ4TagTag = 0, kQ4TagVersion = 1, kQ4TagAbstract = 3,
                                 kQ4TagTlabel = 7;
inline constexpr core::ColumnId kQ4PreAdsh = 0, kQ4PreStmt = 3, kQ4PreTag = 6,
                                 kQ4PreVersion = 7;

struct SecEdgarQ4SubInput {
  std::string adsh;
  int32_t cik = 0;
  int32_t sic = 0;
  bool cik_null = false;
};

struct SecEdgarQ4TagInput {
  std::string tag;
  std::string version;
  int32_t abstract_flag = 0;
  std::string tlabel;
  bool tlabel_null = false;
};

struct SecEdgarQ4PreInput {
  std::string adsh;
  std::string tag;
  std::string version;
  std::string stmt;
};

struct SecEdgarQ4NumInput {
  std::string adsh;
  std::string tag;
  std::string version;
  std::string uom;
  double value = 0.0;
  bool value_is_null = true;
};

// One bridge key as seen in the base tables (post-write state).
struct SecEdgarQ4BridgeRow {
  std::string adsh;
  std::string tag;
  std::string version;
  uint32_t mult = 0;  // pre rows with stmt='EQ' for the key
  bool cik_null = false;
  int32_t cik = 0;
  int32_t sic = 0;
  bool tlabel_null = false;
  std::string tlabel;
  double ns = 0.0;    // SUM(value) over uom='USD' AND value IS NOT NULL
  double ns_lo = 0.0; // optional low-order part of ns (double-double)
  int64_t nc = 0;     // COUNT(value) over the same rows
};

struct SecEdgarQ4BridgeFilter {
  struct Key {
    std::string adsh, tag, version;
  };
  bool all = true;          // everything (build)
  bool include_num = true;  // fill ns/nc (false: engine fills them in later)
  std::vector<std::string> adsh;                              // every key of these filings
  std::vector<std::pair<std::string, std::string>> tag_versions; // every key of these tags
  std::vector<Key> keys;                                      // exact keys
};

class SecEdgarQ4InitialScan {
public:
  virtual ~SecEdgarQ4InitialScan() = default;
  virtual void ScanSub(
      const std::function<void(const SecEdgarQ4SubInput &)> &) const = 0;
  virtual void ScanTag(
      const std::function<void(const SecEdgarQ4TagInput &)> &) const = 0;
  virtual void ScanPre(
      const std::function<void(const SecEdgarQ4PreInput &)> &) const = 0;
  virtual void ScanNum(
      const std::function<void(const SecEdgarQ4NumInput &)> &) const = 0;
  // Qualifying bridge keys.  The default implementation derives them from the
  // four table scans above (portable, slow); engines override it with a join
  // that runs inside the database.
  virtual void
  ScanBridge(const SecEdgarQ4BridgeFilter &filter,
             const std::function<void(const SecEdgarQ4BridgeRow &)> &consumer) const;
};

enum class SecEdgarQ4ImageKind : uint8_t {
  SicCohort,
  PreEqBridge,
  TagTlabel,
  GroupAggregates,
};

struct SecEdgarQ4StateImage {
  SecEdgarQ4ImageKind kind = SecEdgarQ4ImageKind::SicCohort;
  std::shared_ptr<const SecEdgarQ4State> state;
};

// Compact serialized forms (used for footprint accounting).
std::vector<uint8_t> EncodeSecEdgarQ4SicCohort(const SecEdgarQ4State &);
std::vector<uint8_t> EncodeSecEdgarQ4PreEqBridge(const SecEdgarQ4State &);
std::vector<uint8_t> EncodeSecEdgarQ4TagTlabel(const SecEdgarQ4State &);
std::vector<uint8_t> EncodeSecEdgarQ4GroupAggregates(const SecEdgarQ4State &);

// Resident bytes of the in-memory image (what the process actually holds).
uint64_t SecEdgarQ4ResidentBytes(const SecEdgarQ4State &, SecEdgarQ4ImageKind);

struct SecEdgarQ4OutputRow {
  int32_t sic = 0;
  bool tlabel_null = false;
  std::string tlabel;
  std::string stmt = "EQ";
  int64_t num_companies = 0;
  double total_value = 0.0;
  double avg_value = 0.0;
};

// Top-500 groups (same ordering the module emits), for tests and tools.
std::vector<SecEdgarQ4OutputRow> BuildSecEdgarQ4Output(const SecEdgarQ4State &);

class SecEdgarQ4TxnBuffer final : public core::AuxTxnBuffer {
public:
  struct NumDelta {
    uint32_t off = 0; // into arena: adsh | tag | version
    uint32_t adsh_len = 0;
    uint32_t tag_len = 0;
    uint32_t version_len = 0;
    double value = 0.0;
    int8_t sign = 0;
  };

  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  void MarkRebuild();
  void AddNumDelta(std::string_view adsh, std::string_view tag,
                   std::string_view version, double value, int8_t sign);
  void AddDirtyAdsh(std::string adsh);
  void AddDirtyTagVersion(std::string tag, std::string version);
  void AddDirtyKey(std::string adsh, std::string tag, std::string version);
  void MarkPending() { pending = true; }

  const std::vector<NumDelta> &Deltas() const { return deltas; }
  std::string_view Adsh(const NumDelta &d) const {
    return std::string_view(arena.data() + d.off, d.adsh_len);
  }
  std::string_view Tag(const NumDelta &d) const {
    return std::string_view(arena.data() + d.off + d.adsh_len, d.tag_len);
  }
  std::string_view Version(const NumDelta &d) const {
    return std::string_view(arena.data() + d.off + d.adsh_len + d.tag_len,
                            d.version_len);
  }
  bool HasStructural() const {
    return !dirty_adsh.empty() || !dirty_tv.empty() || !dirty_keys.empty();
  }

  std::vector<std::string> dirty_adsh;
  std::vector<std::pair<std::string, std::string>> dirty_tv;
  std::vector<SecEdgarQ4BridgeFilter::Key> dirty_keys;

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::string arena;
  std::vector<NumDelta> deltas;
};

class SecEdgarQ4Module : public core::AuxQueryModule {
public:
  static constexpr const char *kId = core::SecEdgarQ4Layout::kAccelerator;

  explicit SecEdgarQ4Module(SecEdgarQ4Config config,
                            SecEdgarQ4InitialScan *initial_scan = nullptr)
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

protected:
  // Builds the complete state from the base tables.  The parallel module
  // overrides this to aggregate num in parallel.
  virtual SecEdgarQ4State BuildState(core::AuxEngineServices &) const;
  std::vector<core::StateImage> MakeImages(const SecEdgarQ4State &,
                                           const core::Version versions[4]) const;

  SecEdgarQ4Config config;
  SecEdgarQ4InitialScan *initial_scan = nullptr;

private:
  core::StagedMaintenanceOutcome RebuildOutcome(core::AuxEngineServices &,
                                                const char *why) const;
  bool ReadState(core::AuxEngineServices &, SecEdgarQ4State &,
                 core::Version versions[4]) const;
  void EmitGroups(const SecEdgarQ4Groups &, core::AuxRowSink &) const;
};

// Exposed for the parallel module / tests.
SecEdgarQ4State SecEdgarQ4StateFromBridgeRows(
    const std::vector<SecEdgarQ4BridgeRow> &rows);
// Finds the entry for (adsh, tag, version); returns kQ4None when absent.
uint32_t SecEdgarQ4FindEntry(const SecEdgarQ4State &, std::string_view adsh,
                             std::string_view tag, std::string_view version);

} // namespace accelerator_codegen::aux_state::query
