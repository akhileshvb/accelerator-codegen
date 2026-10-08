#pragma once

// SEC-EDGAR Q3 auxiliary state (single partition, immutable snapshots).
//
//   SELECT s.name, s.cik, SUM(n.value) FROM num n JOIN sub s USING (adsh)
//   WHERE n.uom = 'USD' AND s.fy = 2022 AND n.value IS NOT NULL
//   GROUP BY s.name, s.cik HAVING SUM(n.value) > (AVG of per-cik sums)
//   ORDER BY 3 DESC LIMIT 100
//
// The query has no template parameters (uom / fy are literals), so the state
// is parameter independent.
//
// State (all pieces are copy-on-write so a delta touches O(rows) memory):
//   * AdshIndex: for every adsh seen in num (USD, non-null value) or sub:
//     (sum, row count, group id or -1 when the filing is not in the fy=2022
//     cohort).  Keeping filings that are *not* in the cohort makes sub
//     INSERT/DELETE/UPDATE (including fy moves) fully incremental.
//   * GroupDir: dictionary of (name, cik) groups and distinct ciks.
//   * Totals: per-group and per-cik (sum, row count) in SoA chunks, plus a
//     lazily computed, cached top-100 result.
// Group / cik existence is driven by row counts, so a group whose last row is
// deleted disappears exactly like in the SQL result.

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_query_module.hpp"
#include "sec_edgar/q3/layout.hpp"

namespace accelerator_codegen::aux_state::query {

struct SecEdgarQ3Config {
  core::TableId num_table = 0;
  core::TableId sub_table = 0;
};

// Per-adsh aggregate of num rows with uom = 'USD' AND value IS NOT NULL.
struct SecEdgarQ3NumAggInput {
  std::string adsh;
  double sum = 0.0;
  int64_t count = 0;
};

struct SecEdgarQ3SubInput {
  std::string adsh;
  std::string name;
  bool name_is_null = false;
  int32_t cik = 0;
  bool cik_is_null = false;
  int32_t fy = 0;
  bool fy_is_null = false;
};

class SecEdgarQ3InitialScan {
public:
  virtual ~SecEdgarQ3InitialScan() = default;
  // One callback per distinct adsh: SUM(value), COUNT(*) over
  // uom = 'USD' AND value IS NOT NULL (GROUP BY adsh).
  virtual void ScanNumAggregates(
      const std::function<void(const SecEdgarQ3NumAggInput &)> &) = 0;
  virtual void
  ScanSub(const std::function<void(const SecEdgarQ3SubInput &)> &) = 0;
};

struct SecEdgarQ3OutputRow {
  std::string name;
  bool name_is_null = false;
  int32_t cik = 0;
  bool cik_is_null = false;
  double total_value = 0.0;
};

// ---- adsh index -----------------------------------------------------------

struct SecEdgarQ3Entry {
  uint64_t key = 0;   // packed adsh (see SecEdgarQ3PackAdsh) or overflow id
  double sum = 0.0;   // SUM(value) of qualifying num rows
  int32_t cnt = 0;    // number of qualifying num rows
  int32_t group = -1; // group id when sub.fy = 2022 for this adsh, else -1
};

struct SecEdgarQ3Shard {
  std::vector<SecEdgarQ3Entry> entries; // sorted by key
};

// Two-level fan-out (16 x 16 leaf shards): a delta clones one top array, one
// mid array and the touched leaf shard instead of 256 shared pointers.
inline constexpr std::size_t kSecEdgarQ3Fanout = 16;
inline constexpr uint64_t kSecEdgarQ3OverflowBit = 1ULL << 63;

using SecEdgarQ3OverflowMap = std::unordered_map<std::string, uint32_t>;

struct SecEdgarQ3IndexMid {
  std::array<std::shared_ptr<const SecEdgarQ3Shard>, kSecEdgarQ3Fanout> shards;
};

struct SecEdgarQ3AdshIndex {
  std::array<std::shared_ptr<const SecEdgarQ3IndexMid>, kSecEdgarQ3Fanout> mids;
  // adsh strings that do not match the SEC "0000000000-00-000000" pattern.
  std::shared_ptr<const SecEdgarQ3OverflowMap> overflow;
  uint64_t entry_count = 0;
};

// Packs 'DDDDDDDDDD-DD-DDDDDD' into a 60 bit key; false for any other string.
bool SecEdgarQ3PackAdsh(const char *data, std::size_t size, uint64_t &key);

// ---- groups / totals ------------------------------------------------------

struct SecEdgarQ3GroupDir {
  std::string name_pool;
  std::vector<uint32_t> name_off{0};   // groups + 1 offsets into name_pool
  std::vector<uint8_t> group_flags;    // bit0 name NULL, bit1 cik NULL
  std::vector<int32_t> group_cik;
  std::vector<uint32_t> group_cik_id;  // index into cik_value
  std::vector<int32_t> cik_value;
  std::vector<uint8_t> cik_is_null;

  std::size_t GroupCount() const { return group_flags.size(); }
  std::size_t CikCount() const { return cik_value.size(); }
};

inline constexpr std::size_t kSecEdgarQ3AggChunk = 512;

struct alignas(64) SecEdgarQ3AggChunkData {
  double sum[kSecEdgarQ3AggChunk];
  int32_t cnt[kSecEdgarQ3AggChunk];
};

struct SecEdgarQ3AggArray {
  std::vector<std::shared_ptr<const SecEdgarQ3AggChunkData>> chunks;
  uint32_t size = 0;
};

struct SecEdgarQ3Result {
  struct Row {
    uint32_t group = 0;
    double total = 0.0;
  };
  std::vector<Row> rows; // final top-100 in output order
};

struct SecEdgarQ3Totals {
  std::shared_ptr<const SecEdgarQ3GroupDir> dir;
  SecEdgarQ3AggArray groups;
  SecEdgarQ3AggArray ciks;

  // Lazily computed output (immutable once set).
  std::shared_ptr<const SecEdgarQ3Result> Result() const;
  // The same result as ready-to-emit rows (computed once per state version).
  std::shared_ptr<const std::vector<core::AuxRow>> OutputRows() const;

private:
  mutable std::mutex result_mutex;
  mutable std::shared_ptr<const SecEdgarQ3Result> result;
  mutable std::shared_ptr<const std::vector<core::AuxRow>> output_rows;
};

struct SecEdgarQ3State {
  std::shared_ptr<const SecEdgarQ3AdshIndex> index;
  std::shared_ptr<const SecEdgarQ3Totals> totals;
};

enum class SecEdgarQ3ImageKind : uint8_t {
  Fy2022Cohort, // kept for the shared footprint harness; not published
  CikTotals,    // the single published image: full state
};

struct SecEdgarQ3StateImage {
  SecEdgarQ3ImageKind kind = SecEdgarQ3ImageKind::CikTotals;
  std::shared_ptr<const SecEdgarQ3State> state;
};

// Footprint harness hooks (shared aux_state_footprint.cpp): serialized size of
// the compact state.
std::vector<uint8_t> EncodeSecEdgarQ3Fy2022Cohort(const SecEdgarQ3State &);
std::vector<uint8_t> EncodeSecEdgarQ3CikTotals(const SecEdgarQ3State &);

// Approximate resident bytes (vectors' capacity, chunk storage, dictionaries).
uint64_t SecEdgarQ3ResidentBytes(const SecEdgarQ3State &);

// Computes the final answer (top 100) from a state.
std::vector<SecEdgarQ3OutputRow> BuildSecEdgarQ3Output(const SecEdgarQ3State &);

// Scalar / SIMD kernels (exposed for tests and microbenchmarks).
struct SecEdgarQ3Kernels {
  // sum of sums and number of entries with cnt > 0
  static void AvgInputsScalar(const SecEdgarQ3AggArray &, double &sum,
                              uint64_t &count);
  static void AvgInputsAvx2(const SecEdgarQ3AggArray &, double &sum,
                            uint64_t &count);
  static void AvgInputs(const SecEdgarQ3AggArray &, double &sum,
                        uint64_t &count);
  // appends group ids with cnt > 0 and sum > threshold
  static void CollectScalar(const SecEdgarQ3AggArray &, double threshold,
                            std::vector<SecEdgarQ3Result::Row> &out);
  static void CollectAvx2(const SecEdgarQ3AggArray &, double threshold,
                          std::vector<SecEdgarQ3Result::Row> &out);
  static void Collect(const SecEdgarQ3AggArray &, double threshold,
                      std::vector<SecEdgarQ3Result::Row> &out);
  static bool HasAvx2();
};

class SecEdgarQ3TxnBuffer final : public core::AuxTxnBuffer {
public:
  enum class OpKind : uint8_t { Num, SubRemove, SubAdd };

  struct Op {
    OpKind kind = OpKind::Num;
    int8_t sign = 1;       // Num only: row-count delta (-1, 0, +1)
    uint8_t flags = 0;     // SubAdd: bit0 name NULL, bit1 cik NULL
    uint64_t key = 0;      // packed adsh, or overflow bit | local string index
    double value = 0.0;    // Num only: delta of SUM(value)
    int32_t cik = 0;       // SubAdd only
    uint32_t name_idx = 0; // SubAdd only: index into strings
  };

  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  void MarkRebuild();
  void AddOp(Op op);
  uint32_t InternString(std::string value);
  uint64_t KeyFor(const std::string &adsh);

  const std::vector<Op> &Ops() const { return ops; }
  const std::vector<std::string> &Strings() const { return strings; }

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<Op> ops;
  std::vector<std::string> strings;
};

class SecEdgarQ3Module : public core::AuxQueryModule {
public:
  static constexpr const char *kId = core::SecEdgarQ3Layout::kAccelerator;
  // The single published partition.
  static constexpr core::PartitionId kStatePartition =
      core::SecEdgarQ3Layout::kCikTotals;

  explicit SecEdgarQ3Module(SecEdgarQ3Config config,
                            SecEdgarQ3InitialScan *initial_scan = nullptr,
                            core::AcceleratorId accelerator_id = kId)
      : config(config), initial_scan(initial_scan),
        accelerator_id(std::move(accelerator_id)) {}

  core::AcceleratorId Id() const override { return accelerator_id; }
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

  // Full (re)computation of the state from the base tables.
  SecEdgarQ3State BuildState(core::AuxEngineServices &) const;

protected:
  void EmitOutput(const SecEdgarQ3State &, core::AuxRowSink &) const;

  SecEdgarQ3Config config;
  SecEdgarQ3InitialScan *initial_scan = nullptr;
  core::AcceleratorId accelerator_id;
};

} // namespace accelerator_codegen::aux_state::query
