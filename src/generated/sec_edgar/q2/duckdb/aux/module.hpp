#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_query_module.hpp"
#include "sec_edgar/q2/duckdb/aux/state.hpp"
#include "sec_edgar/q2/layout.hpp"

namespace accelerator_codegen::aux_state::query {

// Query parameters (templated literals of benchmarks/sec_edgar/queries/q2.sql).
// The aux table function is accel_sec_edgar_q2_aux() (defaults) or
// accel_sec_edgar_q2_aux(fy BIGINT, uom VARCHAR).
constexpr int64_t kSecEdgarQ2DefaultFy = 2022;
constexpr const char *kSecEdgarQ2DefaultUom = "pure";
constexpr std::size_t kSecEdgarQ2ResultLimit = 100;

// Physical DuckDB column positions in the mutation rows.
//   num: 0 adsh, 1 tag, 2 version, 3 ddate, 4 qtrs, 5 uom, 6 coreg, 7 value
//   sub: 0 adsh, 1 cik, 2 name, ..., 10 fy
constexpr core::ColumnId kQ2NumAdsh = 0, kQ2NumTag = 1, kQ2NumUom = 5,
                         kQ2NumValue = 7;
constexpr core::ColumnId kQ2SubAdsh = 0, kQ2SubName = 2, kQ2SubFy = 10;

// A unit of measure with more non-NULL rows than this is not materialised
// (the default uom is always materialised): it is answered by the exact SQL
// path.  On SF3 400000 covers 199 of the 201 units (all but USD and shares)
// in 17.6 MB of heap; covering every unit costs 442 MB (see the q2 report).
// SecEdgarQ2Config::max_uom_rows overrides it.
constexpr uint64_t kSecEdgarQ2DefaultMaxUomRows = 400000;

struct SecEdgarQ2Config {
  core::TableId num_table = 0;
  core::TableId sub_table = 0;
  uint64_t max_uom_rows = kSecEdgarQ2DefaultMaxUomRows;
  // Overlay entries before it is compacted into a new base (0: automatic).
  uint64_t overlay_limit = 0;
};

struct SecEdgarQ2NumInput {
  std::string adsh;
  std::string tag;
  std::string uom;
  double value = 0.0;
  bool value_is_null = true;
};

struct SecEdgarQ2SubInput {
  std::string adsh;
  std::string name;
  int32_t fy = 0;
  bool name_is_null = false;
  bool fy_is_null = false;
};

// Distinct (adsh, tag, uom, value) with the number of num rows carrying it.
struct SecEdgarQ2ValueCountRow {
  std::string_view adsh;
  std::string_view tag;
  std::string_view uom;
  double value = 0.0;
  uint64_t rows = 0;
};

struct SecEdgarQ2DirectRow {
  std::string_view name;
  bool name_is_null = false;
  std::string_view tag;
  double value = 0.0;
};

// Data access for build / maintenance look-ups / uncovered-parameter path.
// ScanNum and ScanSub are the only mandatory entry points; every other method
// has a (slow, correct) default derived from them so unit-test fakes keep
// working.  The DuckDB implementation overrides all of them with columnar SQL
// (see duckdb_scan.hpp).
class SecEdgarQ2InitialScan {
public:
  virtual ~SecEdgarQ2InitialScan() = default;
  virtual void ScanNum(
      const std::function<void(const SecEdgarQ2NumInput &)> &) = 0;
  virtual void ScanSub(
      const std::function<void(const SecEdgarQ2SubInput &)> &) = 0;

  // Rows with non-NULL adsh, tag, uom and value, counted per uom.
  virtual void ScanUomRowCounts(
      const std::function<void(const std::string &uom, uint64_t rows)> &);
  // Distinct (adsh, tag, uom, value) of the given uoms (same row filter).
  virtual void ScanNumValueCounts(
      const std::vector<std::string> &uoms,
      const std::function<void(const SecEdgarQ2ValueCountRow &)> &);
  // Same rows, restricted to the given (adsh, tag) groups of one uom.
  virtual void LookupNumValueCounts(
      const std::string &uom,
      const std::vector<std::pair<std::string, std::string>> &groups,
      const std::function<void(const SecEdgarQ2ValueCountRow &)> &);
  // sub rows for the given adsh values.
  virtual void LookupSub(
      const std::vector<std::string> &adshs,
      const std::function<void(const SecEdgarQ2SubInput &)> &);
  // Full answer of the query (ordered, LIMIT applied) straight from the base
  // tables.  Used for uoms the aux state does not materialise and whenever
  // the aux state is unavailable.
  virtual void ExecuteDirect(
      int64_t fy, const std::string &uom,
      const std::function<void(const SecEdgarQ2DirectRow &)> &);
};

// Process-wide counters (relaxed atomics) used by tests and benchmarks to
// confirm which maintenance / read paths were exercised.
struct SecEdgarQ2Stats {
  std::atomic<uint64_t> apply_delta{0};
  std::atomic<uint64_t> rebuild{0};
  std::atomic<uint64_t> lost_group_lookups{0};
  std::atomic<uint64_t> new_adsh_lookups{0};
  std::atomic<uint64_t> inconsistent_deltas{0};
  std::atomic<uint64_t> state_reads{0};
  std::atomic<uint64_t> direct_reads{0};
  std::atomic<uint64_t> compactions{0};
};
SecEdgarQ2Stats &SecEdgarQ2GlobalStats();

struct SecEdgarQ2OutputRow {
  std::string name;
  bool name_is_null = false;
  std::string tag;
  double value = 0.0;
};

// The aux state is published as one partition holding one image.
struct SecEdgarQ2StateImage {
  std::shared_ptr<const SecEdgarQ2State> state;
};

// Compact wire encoding of the whole state (footprint accounting, tests).
std::vector<uint8_t> EncodeSecEdgarQ2State(const SecEdgarQ2State &);
bool DecodeSecEdgarQ2State(const std::vector<uint8_t> &, SecEdgarQ2State &);

// Zero-copy variant: views point into the state, valid while it is alive.
struct SecEdgarQ2OutputView {
  std::string_view name;
  bool name_is_null = false;
  std::string_view tag;
  double value = 0.0;
};
bool FinalizeSecEdgarQ2Views(const SecEdgarQ2State &state, int64_t fy,
                             std::string_view uom,
                             std::vector<SecEdgarQ2OutputView> &out);

// Read path: top-LIMIT rows for (fy, uom).  Returns false when the uom is not
// materialised (caller must use the exact SQL path).
bool FinalizeSecEdgarQ2(const SecEdgarQ2State &state, int64_t fy,
                        std::string_view uom,
                        std::vector<SecEdgarQ2OutputRow> &out);

class SecEdgarQ2TxnBuffer final : public core::AuxTxnBuffer {
public:
  struct NumDelta {
    uint64_t adsh_off, tag_off, uom_off;
    uint32_t adsh_len, tag_len, uom_len;
    int64_t key;
    int8_t sign;
  };
  struct SubDelta {
    std::string adsh;
    std::string name;
    int64_t fy;
    uint64_t batch; // mutation batch (statement) the row came from
    bool fy_is_null;
    bool name_is_null;
    int8_t sign;
  };

  bool HasPendingChanges() const override { return pending; }
  void Clear() override;

  bool RequiresRebuild() const { return requires_rebuild; }
  void MarkRebuild();
  void AddNum(std::string_view adsh, std::string_view tag, std::string_view uom,
              int64_t key, int8_t sign);
  void AddSub(SubDelta delta);
  uint64_t NextBatch() { return ++batch_counter; }
  const std::vector<NumDelta> &Nums() const { return nums; }
  const std::vector<SubDelta> &Subs() const { return subs; }
  std::string_view Str(uint64_t off, uint32_t len) const {
    return std::string_view(pool.data() + off, len);
  }

private:
  bool pending = false;
  bool requires_rebuild = false;
  uint64_t batch_counter = 0;
  std::string pool;
  std::vector<NumDelta> nums;
  std::vector<SubDelta> subs;
};

class SecEdgarQ2Module : public core::AuxQueryModule {
public:
  static constexpr const char *kId = core::SecEdgarQ2Layout::kAccelerator;
  static constexpr core::PartitionId kStatePartition =
      core::SecEdgarQ2Layout::kState;

  explicit SecEdgarQ2Module(SecEdgarQ2Config config,
                            SecEdgarQ2InitialScan *initial_scan = nullptr)
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

  // Exposed for tests / tools.
  std::shared_ptr<const SecEdgarQ2State> BuildState() const;

protected:
  virtual const char *AcceleratorName() const { return kId; }

  SecEdgarQ2Config config;
  SecEdgarQ2InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
