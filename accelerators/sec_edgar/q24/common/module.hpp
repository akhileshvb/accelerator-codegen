#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_query_module.hpp"
#include "sec_edgar/q24/layout.hpp"
#include "sec_edgar/q24/common/state.hpp"

namespace accelerator_codegen::aux_state::query {

// Adds a num row of the build scan (columns: adsh, tag, version, uom, value,
// ddate; all nullable) to `orphans` unless a pre row matches it or it cannot
// reach any bucket. `Row` is a build scan row (IsNull, TextView, Double, Int).
template <class Row>
void AddNumOrphan(const Q24PreBase &base, Q24OrphanAccumulator &orphans, const Row &row) {
  int32_t year;
  if (row.IsNull(3) || row.IsNull(4) || row.IsNull(5) || !Q24DdateYear(row.Int(5), year)) return;
  const auto cell = [&](int column) {
    if (row.IsNull(column)) return Q24Str::Null();
    const auto text = row.TextView(column);
    return Q24Str::Of(text.data(), text.size());
  };
  const Q24Str a = cell(0), t = cell(1), v = cell(2), uom = cell(3);
  if (!base.Contains(a, t, v)) orphans.Add(a, t, v, year, uom, row.Double(4));
}

} // namespace accelerator_codegen::aux_state::query

namespace accelerator_codegen::aux_state::query {

// Table ids are engine-assigned. Mutation row layouts (the module's contract):
//   num rows : physical column order  adsh(0) tag(1) version(2) ddate(3)
//              qtrs(4) uom(5) coreg(6) value(7)      [footnote not carried]
//   pre rows : projection [adsh(0) tag(1) version(2)]
// Updates carry `updated_columns` in the same index space.
struct SecEdgarQ24Config {
  core::TableId num_table = 0;
  core::TableId pre_table = 0;
  // Physical attribute numbers captured per table (TableDependency::
  // mutation_attnums); empty = the engine's default capture. PostgreSQL sets
  // these (the DuckDB capture projects pre itself): the module then sees the
  // layouts above.
  std::vector<int32_t> num_attnums;
  std::vector<int32_t> pre_attnums;
};

struct SecEdgarQ24NumInput {
  std::string adsh;
  std::string tag;
  std::string version;
  std::string uom;
  double value = 0.0;
  bool value_is_null = true;
  int32_t ddate = 0;
};

struct SecEdgarQ24PreInput {
  std::string adsh;
  std::string tag;
  std::string version;
};

// Batched (column-oriented) rows handed to Build by engines that can stream
// vectors. Strings are views valid only inside the callback.
struct SecEdgarQ24NumBatch {
  std::size_t count = 0;
  const Q24Str *adsh = nullptr;
  const Q24Str *tag = nullptr;
  const Q24Str *version = nullptr;
  const double *value = nullptr;
  const Q24Str *uom = nullptr;   // may hold NULLs (such rows are skipped)
  const int32_t *ddate = nullptr; // never NULL in a delivered row
};

struct SecEdgarQ24PreBatch {
  std::size_t count = 0;
  const Q24Str *adsh = nullptr;
  const Q24Str *tag = nullptr;
  const Q24Str *version = nullptr;
};

// Post-transaction truth for one (adsh, tag, version) triple, read from the
// base tables: the number of pre rows with this triple, and its num rows with
// value IS NOT NULL, uom IS NOT NULL and a bucketable ddate, aggregated per
// (year, uom) bucket (year = floor(ddate / 10000), see Q24DdateYear).
struct SecEdgarQ24TripleKey {
  std::string adsh;
  std::string tag;
  std::string version;
};

struct SecEdgarQ24TripleBucket {
  int32_t year = 0;
  std::string uom;
  int64_t num_rows = 0;
  double num_sum = 0.0;
};

struct SecEdgarQ24TripleState {
  int64_t pre_rows = 0;
  std::vector<SecEdgarQ24TripleBucket> buckets;
};

class SecEdgarQ24InitialScan {
public:
  virtual ~SecEdgarQ24InitialScan() = default;
  // Portable row-at-a-time scans (all rows of the table).
  virtual void ScanNum(
      const std::function<void(const SecEdgarQ24NumInput &)> &) = 0;
  virtual void ScanPre(
      const std::function<void(const SecEdgarQ24PreInput &)> &) = 0;

  // Optional fast paths. Return false when unsupported (module falls back to
  // the row-at-a-time scans). ScanQualifyingNumBatches must deliver every num
  // row with value IS NOT NULL and ddate IS NOT NULL (it may also drop rows
  // whose uom is NULL or whose ddate is not bucketable, and rows that have a
  // pre match); the module filters the rest itself.
  virtual bool ScanQualifyingNumBatches(
      const std::function<void(const SecEdgarQ24NumBatch &)> &) {
    return false;
  }
  virtual bool ScanPreBatches(
      const std::function<void(const SecEdgarQ24PreBatch &)> &) {
    return false;
  }
  // Optional: ingests every pre row with non-NULL (adsh, tag, version) into
  // `raw` (dictionary ids via FindOrAdd, appended to raw.rows as triples),
  // resolving each distinct string of a storage dictionary once. False when
  // unsupported (nothing ingested).
  virtual bool ScanPreIds(Q24RawPre &) { return false; }
  // Optional: as ScanQualifyingNumBatches, but delivers only the rows whose
  // (adsh, tag, version) is NOT in `pre` (the anti-join's survivors); the
  // engine tests membership itself (e.g. on dictionary ids cached per
  // compressed-storage dictionary instead of hashing every row's strings).
  virtual bool ScanNumOrphans(
      const Q24PreBase &,
      const std::function<void(const SecEdgarQ24NumBatch &)> &) {
    return false;
  }
  // Optional: the anti-join's survivors accumulated per partition on the build's
  // workers (AddNumOrphan per row, partials merged in partition order).
  // Return false when unsupported (nothing accumulated).
  virtual bool ScanNumOrphansPartitioned(const core::AuxEngineServices &, const Q24PreBase &,
                                         Q24OrphanAccumulator &) {
    return false;
  }
  // Optional: targeted lookups used by incremental pre DELETE / key UPDATE
  // (a removed pre triple can turn matched num rows into orphans, whose values
  // were not kept). Must see the committing transaction's own writes. Return
  // false when unsupported (the module then rebuilds).
  virtual bool LoadTripleStates(const std::vector<SecEdgarQ24TripleKey> &,
                                std::vector<SecEdgarQ24TripleState> &) {
    return false;
  }
};

// Image payload. `pre` is set on the PreMembership image, `groups` on the
// Aggregates image.
struct SecEdgarQ24State {
  std::shared_ptr<const Q24PreState> pre;
  std::shared_ptr<const Q24GroupState> groups;
};

// Kept for the footprint harness: TagVersions is no longer a partition (the
// groups live inside the aggregates image) and encodes to an empty buffer.
enum class SecEdgarQ24ImageKind : uint8_t {
  PreMembership,
  TagVersions,
  Aggregates,
};

struct SecEdgarQ24StateImage {
  SecEdgarQ24ImageKind kind = SecEdgarQ24ImageKind::PreMembership;
  std::shared_ptr<const SecEdgarQ24State> state;
};

std::vector<uint8_t> EncodeSecEdgarQ24PreMembership(const SecEdgarQ24State &);
std::vector<uint8_t> EncodeSecEdgarQ24TagVersions(const SecEdgarQ24State &);
std::vector<uint8_t> EncodeSecEdgarQ24Aggregates(const SecEdgarQ24State &);

// Ordered log of what a transaction did; replayed in Stage so interleavings of
// num and pre writes resolve exactly as sequential execution would.
class SecEdgarQ24TxnBuffer final : public core::AuxTxnBuffer {
public:
  struct Op {
    // PreTouch: a pre row with this triple was deleted / re-keyed; the
    // triple is resolved from the base tables at Stage.
    enum class Kind : uint8_t { Num, PreInsert, PreTouch } kind = Kind::Num;
    int8_t sign = 0;
    int32_t year = 0;   // Num ops: bucket of the row
    std::string uom;
    double value = 0.0;
    // enc(adsh) enc(tag) enc(version)
    std::string key;
  };

  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  const std::vector<Op> &Ops() const;
  void MarkRebuild();
  void AddOp(Op op);
  // Appends `from`'s ops (buffered after this buffer's).
  void Absorb(SecEdgarQ24TxnBuffer &&from);

private:
  bool requires_rebuild = false;
  std::vector<Op> ops;
};

class SecEdgarQ24Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = core::SecEdgarQ24Layout::kAccelerator;
  // Maximum number of pre triples kept in the post-build overlay before a
  // rebuild folds them into the immutable base.
  static constexpr std::size_t kMaxOverlayKeys = 8192;
  // Maximum distinct triples one transaction may resolve through table lookups
  // (pre DELETE / key UPDATE) before falling back to a rebuild.
  static constexpr std::size_t kMaxTouchedTriples = 256;
  // A num batch at least this large is pre-filtered against the published
  // pre membership (one pipelined probe) before its rows are keyed.
  static constexpr std::size_t kMatchFilterMinRows = 1;

  explicit SecEdgarQ24Module(SecEdgarQ24Config config,
                             SecEdgarQ24InitialScan *initial_scan = nullptr)
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

  // Serial build from the registered InitialScan (also used by the parallel
  // module as its portable fallback).
  SecEdgarQ24State BuildState(core::AuxEngineServices &) const;

  // Emit the two partition images under `accelerator` (the serial and parallel
  // modules publish under different ids).
  static void EmitStateImages(const char *accelerator,
                              const SecEdgarQ24State &state,
                              core::StateBuilder &builder);
  // Writes the top-100 rows of the (year, uom) bucket `bucket_key` (empty
  // key = empty result).
  static void EmitOutput(const Q24GroupState &groups,
                         const std::string &bucket_key, core::AuxRowSink &sink);
  // Maps the table function arguments (year BIGINT, uom VARCHAR) to a
  // bucket key. False: the result is empty (NULL or non-matching argument).
  static bool ParseArguments(const core::QueryArguments &, std::string &bucket_key);
  // Reads the published aggregates image; false when absent/invalid.
  bool LoadGroups(core::AuxEngineServices &,
                  std::shared_ptr<const Q24GroupState> &) const;

private:
  SecEdgarQ24Config config;
  SecEdgarQ24InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
