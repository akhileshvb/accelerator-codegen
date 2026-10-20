#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_query_module.hpp"
#include "prims/chunked_cow_array.hpp"
#include "prims/string_blob.hpp"
#include "sec_edgar/q1/layout.hpp"

namespace duckdb {
class Connection;
class DataChunk;
}

namespace accelerator_codegen::aux_state::query {

// SEC EDGAR Q1:
//   SELECT stmt, rfile, COUNT(*) cnt, COUNT(DISTINCT adsh) num_filings,
//          AVG(line) avg_line_num
//   FROM pre WHERE stmt IS NOT NULL GROUP BY stmt, rfile ORDER BY cnt DESC
// The query has no parameters; the state is therefore parameter independent.
//
// State: per (stmt, rfile) group the row count, the line sum / non-null count
// (AVG) and a refcount per distinct adsh (COUNT(DISTINCT adsh) = number of
// non-zero refcounts, so inserts AND deletes are exact). Accessions are
// dictionary-encoded: a canonical accession "DDDDDDDDDD-DD-DDDDDD" is an exact
// bijection to an integer key (not a hash: no collisions), any other string
// is interned in a string dictionary; both map to a dense 64-bit id.
//
// Mutation row contract (compact pre row): [stmt, rfile, adsh, line].

struct SecEdgarQ1Config {
  core::TableId pre_table = 0;
  // Postgres: physical (0-based) attribute numbers of pre.{stmt, rfile, adsh,
  // line}, in the order of the compact mutation row. Empty (DuckDB): the engine
  // projects the compact row itself.
  std::vector<int32_t> pre_attnums;
  // Set: AVG(line) is exact numeric text from this formatter (sum of line,
  // count); null: a double as DuckDB computes it.
  using AvgTextFn = std::string (*)(__int128 sum, int64_t count);
  AvgTextFn avg_text = nullptr;
};

namespace q1 {
class Builder;
}

struct SecEdgarQ1PreInput {
  std::string stmt;
  std::string rfile;
  bool rfile_is_null = false;
  bool stmt_is_null = false;
  std::string adsh;
  int32_t line = 0;
  bool adsh_is_null = false;
  bool line_is_null = false;
};

class SecEdgarQ1InitialScan {
public:
  virtual ~SecEdgarQ1InitialScan() = default;
  virtual void ScanPre(
      const std::function<void(const SecEdgarQ1PreInput &)> &) const = 0;
  // Optional fast path: stream (stmt VARCHAR, rfile VARCHAR, adsh VARCHAR,
  // line INTEGER) DataChunks. Returns false when unsupported (ScanPre is used).
  virtual bool
  ScanPreChunks(const std::function<void(::duckdb::DataChunk &)> &) const {
    return false;
  }
  // Optional engine-native fast path: feeds the whole table to `builder`
  // (q1::Builder::ConsumeChunk / ConsumeRowView). Returns false when
  // unsupported (ScanPre is used).
  virtual bool ScanPreInto(q1::Builder &) const { return false; }
  // Optional partitioned scan: fills `builder` from one q1::Builder per
  // partition, merged in partition order (core::PartitionedBuild). Returns
  // false when unsupported or when the build cap is one worker.
  virtual bool ScanPrePartitioned(const core::AuxEngineServices &, q1::Builder &) const {
    return false;
  }
};

namespace q1 {

using Int128 = __int128;
using Id = uint64_t; // dense accession id (dictionary rank at build, then append order)

constexpr unsigned kPageBits = 9; // 512 refcounts per copy-on-write chunk
constexpr uint32_t kPageSize = 1u << kPageBits;
constexpr uint8_t kEscape = 0xFF; // low byte 255: the exact count lives in `ext`
constexpr Id kNoId = ~Id(0);
// Accession keys: canonical accessions are < 10^18 < 2^60; bit 63 tags an
// index into the "other" (non-canonical) string dictionary.
constexpr uint64_t kOtherKeyBit = uint64_t(1) << 63;

// Refcounts of (group, accession id), copy-on-write in 512-slot chunks so that
// a maintenance step copies O(touched chunks), not O(state). A count below 255
// is stored in one byte; a count of 255 or more stores 255 in `low` and the
// exact 64-bit count in `ext` (chunks of `ext` exist only where such counts
// occur). Both arrays are core::ChunkedCowArray.
struct GroupRefs {
  core::ChunkedCowArray<uint8_t, kPageBits> low;
  core::ChunkedCowArray<uint64_t, kPageBits> ext;

  uint64_t Get(Id id) const;
  // Sets the exact count (>= 0).
  void Set(Id id, uint64_t count);
  uint64_t MemoryBytes() const;
};

struct GroupData {
  std::string stmt;
  std::string rfile;
  bool rfile_is_null = false;
  int64_t cnt = 0;
  Int128 line_sum = 0; // exact (DuckDB sums INTEGER into a HUGEINT)
  int64_t line_cnt = 0; // non-null lines (AVG denominator)
  int64_t distinct = 0; // COUNT(DISTINCT adsh)
  std::shared_ptr<const GroupRefs> refs;
};

// Sorted accession keys; the id of keys[i] is i unless `ids` is non-empty.
struct DictBase {
  std::vector<uint64_t> keys; // sorted
  std::vector<Id> ids;        // parallel to keys; EMPTY == identity (id == index)
  Id IdAt(std::size_t index) const { return ids.empty() ? Id(index) : ids[index]; }
};

// Non-canonical accession strings -> index. Append-only (an index is never
// reused). An immutable base plus a small copy-on-write overlay; the overlay is
// merged into a new base once it outgrows a fraction of the base.
class OtherIds {
public:
  static constexpr uint64_t kNone = ~uint64_t(0);
  uint64_t Find(std::string_view text) const;
  uint64_t Intern(std::string_view text); // existing or new index
  uint64_t size() const;
  uint64_t MemoryBytes() const;
  // Visits (text, index) in index order.
  void ForEach(const std::function<void(std::string_view, uint64_t)> &fn) const;

private:
  std::shared_ptr<const core::StringDict> base_;
  std::shared_ptr<const std::vector<std::string>> overlay_;
};

struct State {
  std::vector<GroupData> groups;
  std::shared_ptr<const DictBase> dict;
  // Ids created since the last compaction, sorted by key (copy-on-write).
  std::shared_ptr<const std::vector<std::pair<uint64_t, Id>>> overlay;
  OtherIds others;
  Id next_id = 0;
  Id ids_at_build = 0;

  Id FindId(uint64_t key) const;
  Id GetOrCreateId(uint64_t key);
  int FindGroup(const std::string &stmt, const std::string &rfile,
                bool rfile_is_null) const;
  int GetOrCreateGroup(const std::string &stmt, const std::string &rfile,
                       bool rfile_is_null);
  void CompactDictionary();
  uint64_t MemoryBytes() const;
};

struct OutputRow {
  std::string stmt;
  std::string rfile;
  bool rfile_is_null = false;
  int64_t cnt = 0;
  int64_t num_filings = 0;
  double avg_line_num = 0.0;
  bool avg_is_null = false;
  Int128 line_sum = 0; // exact AVG numerator
  int64_t line_cnt = 0; // AVG denominator
};

// AVG(INTEGER) exactly as DuckDB finalizes it: long double division of the
// exact sum by the count, rounded to double.
double AvgLine(Int128 sum, int64_t count);

std::vector<OutputRow> BuildOutput(const State &);
std::vector<uint8_t> Encode(const State &);
bool Decode(const std::vector<uint8_t> &, State &);

// Incremental build used by Build()/ExecuteFallback (single and parallel).
class Builder {
public:
  Builder();
  ~Builder();
  Builder(Builder &&) noexcept;
  Builder &operator=(Builder &&) noexcept;

  // DuckDB only (defined in duckdb/aux/chunk_builder.cpp).
  void ConsumeChunk(::duckdb::DataChunk &chunk);
  void ConsumeRow(const SecEdgarQ1PreInput &row);
  // Zero-copy row: the views need to stay valid for the call only. Consecutive
  // rows of one (group, accession) reuse the previous lookups. A NULL stmt row
  // is ignored; a NULL adsh/rfile/line passes a null pointer / flag.
  void ConsumeRowView(const char *stmt, std::size_t stmt_len, bool stmt_null,
                      const char *rfile, std::size_t rfile_len, bool rfile_null,
                      const char *adsh, std::size_t adsh_len, bool adsh_null,
                      int32_t line, bool line_null);
  void Merge(Builder &&other);
  // Per-group image construction runs on `services`' workers (serial if null).
  State Finish(const core::AuxEngineServices *services = nullptr);

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};

// Adds a row of a partitioned build scan (columns: stmt, rfile, adsh, line; all
// nullable). `Row` is the engine's build scan row (IsNull, TextView, Int).
template <class Row> void AddPreRow(Builder &builder, const Row &row) {
  if (row.IsNull(0)) return;
  const bool rfile_null = row.IsNull(1), adsh_null = row.IsNull(2), line_null = row.IsNull(3);
  const auto stmt = row.TextView(0);
  const auto rfile = rfile_null ? std::string_view() : row.TextView(1);
  const auto adsh = adsh_null ? std::string_view() : row.TextView(2);
  builder.ConsumeRowView(stmt.data(), stmt.size(), false, rfile.data(), rfile.size(), rfile_null,
                         adsh.data(), adsh.size(), adsh_null,
                         line_null ? 0 : static_cast<int32_t>(row.Int(3)), line_null);
}

} // namespace q1

using SecEdgarQ1State = q1::State;
using SecEdgarQ1OutputRow = q1::OutputRow;

struct SecEdgarQ1StateImage {
  std::shared_ptr<const SecEdgarQ1State> state;
};

std::vector<uint8_t> EncodeSecEdgarQ1State(const SecEdgarQ1State &);
bool DecodeSecEdgarQ1State(const std::vector<uint8_t> &, SecEdgarQ1State &);
std::vector<SecEdgarQ1OutputRow> BuildSecEdgarQ1Output(const SecEdgarQ1State &);

class SecEdgarQ1TxnBuffer final : public core::AuxTxnBuffer {
public:
  struct LocalGroup {
    std::string stmt;
    std::string rfile;
    bool rfile_is_null = false;
  };
  // Signed contribution of one or more base rows to one (group, adsh key).
  struct Delta {
    uint32_t group = 0; // index into Groups()
    uint64_t key = 0;   // adsh key; ignored when d_refc == 0
    int32_t d_cnt = 0;
    int32_t d_refc = 0;
    int32_t d_line_cnt = 0;
    int64_t d_line_sum = 0;
  };

  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  void MarkRebuild();
  const std::vector<Delta> &Deltas() const { return deltas; }
  const std::vector<LocalGroup> &Groups() const { return groups; }
  const std::vector<std::string> &OtherAdsh() const { return others; }

  uint32_t InternGroup(const std::string &stmt, const std::string &rfile,
                       bool rfile_is_null);
  uint64_t InternAdsh(const std::string &adsh);
  void AddDelta(const Delta &delta);
  // Appends `from`'s deltas (buffered after this buffer's) re-interned here.
  void Absorb(SecEdgarQ1TxnBuffer &&from);

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<Delta> deltas;
  std::vector<LocalGroup> groups;
  std::unordered_map<std::string, uint32_t> group_index;
  std::vector<std::string> others;
  std::unordered_map<std::string, uint32_t> other_index;
};

// Applies a staged buffer to `state` in place. Returns false when the deltas
// are inconsistent with the state (caller must rebuild).
bool ApplySecEdgarQ1Deltas(const core::AuxEngineServices &services, SecEdgarQ1State &state,
                           const SecEdgarQ1TxnBuffer &buffer);

class SecEdgarQ1Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = core::SecEdgarQ1Layout::kAccelerator;
  // The whole state is one immutable image.
  static constexpr core::PartitionId kStatePartition =
      core::SecEdgarQ1Layout::kState;

  explicit SecEdgarQ1Module(SecEdgarQ1Config config,
                            SecEdgarQ1InitialScan *initial_scan = nullptr)
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

  // Shared by the parallel module: stage using this module's delta logic and
  // publish under `accelerator_id`.
  core::StagedMaintenanceOutcome
  StageForAccelerator(core::AuxEngineServices &, core::AuxTxnBuffer &,
                      const char *accelerator_id);
  // `accelerator_id` null: this module's own id.
  std::shared_ptr<const SecEdgarQ1State>
  LoadState(core::AuxEngineServices &, core::Version *version = nullptr,
            const char *accelerator_id = nullptr) const;
  static void EmitOutput(const SecEdgarQ1State &, core::AuxRowSink &);
  // AVG(line) as the numeric text `avg_text` gives.
  static void EmitOutputNumeric(const SecEdgarQ1State &, core::AuxRowSink &,
                                SecEdgarQ1Config::AvgTextFn avg_text);

  SecEdgarQ1State BuildState(core::AuxEngineServices &) const;

private:
  void Emit(const SecEdgarQ1State &state, core::AuxRowSink &sink) const {
    config.avg_text ? EmitOutputNumeric(state, sink, config.avg_text) : EmitOutput(state, sink);
  }

  SecEdgarQ1Config config;
  SecEdgarQ1InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
