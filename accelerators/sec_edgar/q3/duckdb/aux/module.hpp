#pragma once

// SEC-EDGAR Q3 auxiliary state (single partition, immutable snapshots).
//
//   SELECT s.name, s.cik, SUM(n.value) FROM num n JOIN sub s USING (adsh)
//   WHERE n.uom = :uom AND s.fy = :fy AND n.value IS NOT NULL
//   GROUP BY s.name, s.cik HAVING SUM(n.value) > (AVG of per-cik sums)
//   ORDER BY 3 DESC LIMIT 100
//
// Parameters: accel_sec_edgar_q3_aux() = (fy 2022, uom 'USD');
// accel_sec_edgar_q3_aux(fy BIGINT, uom VARCHAR) answers any (fy, uom) exactly.
//
// State (all pieces are copy-on-write so a delta touches O(rows) memory):
//   * Sub index: adsh -> (group id of (name, cik), fy) for every sub row with a
//     non-NULL fy and a non-NULL adsh.
//   * Num indexes, one per unit of measure (uom dictionary): adsh -> (sum,
//     row count) over the rows with that uom and a non-NULL value.  Sums are
//     double-double (sum + comp), so add/remove cycles do not drift; counts
//     are 64 bit.
//   * GroupDir: dictionary of (name, cik) groups and distinct ciks (append only).
//   * Cohorts: per-(fy, uom) per-group and per-cik (sum, row count) in SoA
//     chunks plus a lazily computed, cached top-100 result.  A cohort is built
//     from the two indexes on first use (and for the defaults at build time),
//     kept in a bounded per-version cache, and repaired incrementally by every
//     delta (a num row touches the one cohort (fy of its filing, its uom); a
//     sub change touches the cohorts of its fy).  Evicted / unseen cohorts are
//     rebuilt from the indexes, so any (fy, uom) is answered exactly from
//     state.
// Group / cik existence is driven by row counts, so a group whose last row is
// deleted disappears exactly like in the SQL result.

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_query_module.hpp"
#include "prims/chunked_blob_map.hpp"
#include "prims/chunked_cow_array.hpp"
#include "prims/flat_hash_map.hpp"
#include "sec_edgar/q3/layout.hpp"

namespace accelerator_codegen::aux_state::query {

struct SecEdgarQ3Config {
  core::TableId num_table = 0;
  core::TableId sub_table = 0;
  // Where the module finds its columns in a captured row: num (adsh, uom,
  // value), sub (adsh, name, cik, fy). Default: positions in the full row of
  // the DuckDB catalog layout; an engine that captures a projection (the
  // PostgreSQL binding) lists the physical attnums to project and the
  // positions in the projected row.
  std::vector<int32_t> num_attnums;
  std::vector<int32_t> sub_attnums;
  std::vector<core::ColumnId> num_pos = {0, 5, 7};
  std::vector<core::ColumnId> sub_pos = {0, 1, 2, 3};
};

// Per-(adsh, uom) aggregate of num rows with value IS NOT NULL.
struct SecEdgarQ3NumAggInput {
  std::string adsh;
  std::string uom;
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

// Partial of a partitioned build: double-double SUM(value) and COUNT(*) per
// (adsh, uom) over rows with value IS NOT NULL. Partials merge in partition
// order, so with a fixed partition count the sums do not depend on the threads.
class SecEdgarQ3NumTotals {
public:
  struct Total {
    double sum = 0.0, comp = 0.0;
    int64_t count = 0;
  };
  void Add(std::string_view adsh, std::string_view uom, double value);
  void Merge(SecEdgarQ3NumTotals &&from);
  // fn(adsh, uom, total) for every group.
  void ForEach(const std::function<void(std::string_view, std::string_view, const Total &)> &fn) const;

private:
  std::unordered_map<std::string, Total> totals; // uom, '\0', adsh
};

// Adds a row of a partitioned num scan (columns: adsh, uom, value; all nullable).
// `Row` is the engine's build scan row (IsNull, TextView, Double).
template <class Row> void AddNumRow(SecEdgarQ3NumTotals &partial, const Row &row) {
  if (row.IsNull(0) || row.IsNull(1) || row.IsNull(2)) return;
  partial.Add(row.TextView(0), row.TextView(1), row.Double(2));
}

class SecEdgarQ3InitialScan {
public:
  virtual ~SecEdgarQ3InitialScan() = default;
  // Optional: the same aggregates from a partitioned scan of num. Return false
  // when unsupported (ScanNumAggregates is used).
  virtual bool ScanNumTotals(const core::AuxEngineServices &, SecEdgarQ3NumTotals &) {
    return false;
  }
  // One callback per distinct (adsh, uom): SUM(value), COUNT(*) over
  // value IS NOT NULL (GROUP BY adsh, uom); adsh and uom non-NULL.
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

// Entry of a num index (sum / comp / cnt) or of the sub index (group / fy).
struct SecEdgarQ3Entry {
  double sum = 0.0;   // SUM(value) of qualifying num rows (rounded value)
  double comp = 0.0;  // compensation: exact sum = sum + comp (double-double)
  int64_t cnt = 0;    // number of qualifying num rows
  int32_t group = -1; // sub index: group id of the filing (-1: num index)
  int32_t fy = 0;     // sub index: the filing's fiscal year
};

// Keys are 64-bit: an adsh of the SEC pattern "0000000000-00-000000" packs
// into 60 bits; every other string gets an id from the overflow dictionary
// (key = overflow bit | id). Both are exact (no hash collisions).
inline constexpr uint64_t kSecEdgarQ3OverflowBit = 1ULL << 63;

// Packs 'DDDDDDDDDD-DD-DDDDDD' into a 60 bit key; false for any other string.
bool SecEdgarQ3PackAdsh(const char *data, std::size_t size, uint64_t &key);

// Interned non-conforming adsh strings (copy-on-write, hash chains verified
// against the stored string).
struct SecEdgarQ3OverflowDict {
  core::ChunkedBlobMap strings;                   // id -> adsh bytes
  core::ShardedCowHashMap<uint32_t, 6> by_hash;   // hash -> newest id + 1
  core::ChunkedCowArray<uint32_t, 10> next;       // id -> older id + 1
  uint32_t count = 0;
};

// Hash index key -> entry. The entries live in 2^(top_bits+mid_bits) leaves
// (FlatHashMap) reached through a two-level directory so that a delta clones
// one top array, one mid array and the touched leaves. The leaf count is
// sized from the entry count at build (about 2k entries per leaf).
using SecEdgarQ3Leaf = core::FlatHashMap<SecEdgarQ3Entry>;

struct SecEdgarQ3IndexMid {
  std::vector<std::shared_ptr<const SecEdgarQ3Leaf>> leaves;
};

struct SecEdgarQ3AdshIndex {
  uint32_t top_bits = 0;
  uint32_t mid_bits = 0;
  std::vector<std::shared_ptr<const SecEdgarQ3IndexMid>> tops;
  SecEdgarQ3OverflowDict overflow;
  uint64_t entry_count = 0;
};

// ---- groups / totals ------------------------------------------------------

struct SecEdgarQ3GroupMeta {
  int32_t cik = 0;
  uint32_t cik_id = 0;  // index into cik_meta
  uint32_t next = 0;    // older group with the same hash + 1
  uint8_t flags = 0;    // bit0 name NULL, bit1 cik NULL
};

struct SecEdgarQ3CikMeta {
  int32_t value = 0;
  uint8_t is_null = 0;
};

// Dictionary of (name, cik) groups (append only) and distinct ciks.
struct SecEdgarQ3GroupDir {
  core::ChunkedBlobMap names;                       // group -> name (absent: NULL)
  core::ChunkedCowArray<SecEdgarQ3GroupMeta, 10> meta;
  core::ChunkedCowArray<SecEdgarQ3CikMeta, 10> cik_meta;
  core::ShardedCowHashMap<uint32_t, 10> by_hash;    // hash(name, cik) -> newest group + 1
  core::ShardedCowHashMap<uint32_t, 8> cik_ids;     // cik (NULL: INT64_MIN) -> cik id + 1
  uint32_t group_count = 0;
  uint32_t cik_count = 0;

  std::size_t GroupCount() const { return group_count; }
  std::size_t CikCount() const { return cik_count; }
  bool NameIsNull(uint32_t g) const { return (meta.Get(g).flags & 1) != 0; }
  bool CikIsNull(uint32_t g) const { return (meta.Get(g).flags & 2) != 0; }
  int32_t Cik(uint32_t g) const { return meta.Get(g).cik; }
  std::string_view Name(uint32_t g) const {
    std::string_view v;
    names.Get(g, v);
    return v;
  }
};

inline constexpr std::size_t kSecEdgarQ3AggChunk = 256;

// Structure of arrays so the kernels stream sums and counts. `comp` is the
// compensation term of the (sum, comp) double-double accumulator.
struct alignas(64) SecEdgarQ3AggChunkData {
  double sum[kSecEdgarQ3AggChunk];
  double comp[kSecEdgarQ3AggChunk];
  int64_t cnt[kSecEdgarQ3AggChunk];
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

// One cohort: the per-group / per-cik aggregates of a (fy, uom) pair.
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

// Unit-of-measure dictionary (append only; ids index State::uom_index).
struct SecEdgarQ3UomDict {
  std::vector<std::string> names;
  std::unordered_map<std::string, uint32_t> ids;
};

inline constexpr std::size_t kSecEdgarQ3MaxCohorts = 64;
// Cohorts materialized at build (see SecEdgarQ3Module::BuildState).
inline constexpr std::size_t kSecEdgarQ3PrebuiltCohorts = 24;
inline constexpr int32_t kSecEdgarQ3DefaultFy = 2022;
inline constexpr const char *kSecEdgarQ3DefaultUom = "USD";

inline uint64_t SecEdgarQ3CohortKey(int32_t fy, uint32_t uom_id) {
  return (static_cast<uint64_t>(static_cast<uint32_t>(fy)) << 32) | uom_id;
}

// Cohorts materialized for one state version (bounded, oldest evicted first,
// a hit moves a cohort to the back).  The per-version cache is the only
// mutable part of a state; a delta copies it and repairs the copies.
struct SecEdgarQ3CohortCache {
  std::mutex mutex;
  std::vector<std::pair<uint64_t, std::shared_ptr<const SecEdgarQ3Totals>>> items;
};

struct SecEdgarQ3State {
  std::shared_ptr<const SecEdgarQ3AdshIndex> subs;
  std::shared_ptr<const SecEdgarQ3UomDict> uom_dict;
  std::vector<std::shared_ptr<const SecEdgarQ3AdshIndex>> uom_index;
  std::shared_ptr<const SecEdgarQ3GroupDir> dir;
  std::shared_ptr<SecEdgarQ3CohortCache> cohorts;
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

// The cohort of (fy, uom), materialized from the indexes on first use; null
// when no row can match (unknown uom, fy outside INTEGER).
std::shared_ptr<const SecEdgarQ3Totals>
SecEdgarQ3CohortFor(const core::AuxEngineServices &, const SecEdgarQ3State &, int64_t fy,
                    const std::string &uom);

// Computes the final answer (top 100) for (fy, uom) from a state.
std::vector<SecEdgarQ3OutputRow>
BuildSecEdgarQ3Output(const core::AuxEngineServices &, const SecEdgarQ3State &,
                      int64_t fy = kSecEdgarQ3DefaultFy,
                      const std::string &uom = kSecEdgarQ3DefaultUom);

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
    double value = 0.0;    // Num only: delta of SUM(value), high part
    double value_lo = 0.0; // Num only: low part (exact double-double delta)
    int32_t cik = 0;       // SubAdd only
    int32_t fy = 0;        // SubAdd only
    uint32_t name_idx = 0; // SubAdd only: index into strings
    uint32_t uom_idx = 0;  // Num only: index into strings
  };

  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  void MarkRebuild();
  void AddOp(Op op);
  uint32_t InternString(std::string value);
  uint32_t InternUom(const std::string &uom);
  uint64_t KeyFor(const std::string &adsh);
  // Appends `from`'s ops (buffered after this buffer's) re-interned here.
  void Absorb(SecEdgarQ3TxnBuffer &&from);

  const std::vector<Op> &Ops() const { return ops; }
  const std::vector<std::string> &Strings() const { return strings; }

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<Op> ops;
  std::vector<std::string> strings;
  std::unordered_map<std::string, uint32_t> uom_strings;
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
  void EmitOutput(const core::AuxEngineServices &, const SecEdgarQ3State &, int64_t fy,
                  const std::string &uom, core::AuxRowSink &) const;
  void EmitRows(core::AuxRowSink &,
                const std::shared_ptr<const std::vector<core::AuxRow>> &) const;

  SecEdgarQ3Config config;
  SecEdgarQ3InitialScan *initial_scan = nullptr;
  core::AcceleratorId accelerator_id;
};

} // namespace accelerator_codegen::aux_state::query
