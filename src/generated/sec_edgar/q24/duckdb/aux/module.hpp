#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_query_module.hpp"
#include "sec_edgar/q24/layout.hpp"
#include "sec_edgar/q24/duckdb/aux/state.hpp"

namespace accelerator_codegen::aux_state::query {

// Table ids are engine-assigned. Mutation row layouts (the module's contract):
//   num rows : physical column order  adsh(0) tag(1) version(2) ddate(3)
//              qtrs(4) uom(5) coreg(6) value(7)      [footnote not carried]
//   pre rows : projection [adsh(0) tag(1) version(2)]
// Updates carry `updated_columns` in the same index space.
struct SecEdgarQ24Config {
  core::TableId num_table = 0;
  core::TableId pre_table = 0;
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
};

struct SecEdgarQ24PreBatch {
  std::size_t count = 0;
  const Q24Str *adsh = nullptr;
  const Q24Str *tag = nullptr;
  const Q24Str *version = nullptr;
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
  // the row-at-a-time scans). ScanQualifyingNumBatches must deliver exactly
  // the num rows with uom = kSecEdgarQ24Uom, ddate in
  // [kSecEdgarQ24FirstDate, kSecEdgarQ24LastDate] and value IS NOT NULL.
  virtual bool ScanQualifyingNumBatches(
      const std::function<void(const SecEdgarQ24NumBatch &)> &) {
    return false;
  }
  virtual bool ScanPreBatches(
      const std::function<void(const SecEdgarQ24PreBatch &)> &) {
    return false;
  }
};

struct SecEdgarQ24OutputRow {
  std::string tag;
  std::string version;
  bool tag_null = false;
  bool version_null = false;
  int64_t count = 0;
  double total = 0.0;
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

std::vector<SecEdgarQ24OutputRow>
BuildSecEdgarQ24Output(const SecEdgarQ24State &);

// Ordered log of what a transaction did; replayed in Stage so interleavings of
// num and pre writes resolve exactly as sequential execution would.
class SecEdgarQ24TxnBuffer final : public core::AuxTxnBuffer {
public:
  struct Op {
    enum class Kind : uint8_t { Num, PreInsert } kind = Kind::Num;
    int8_t sign = 0;
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
  static void EmitOutput(const Q24GroupState &groups, core::AuxRowSink &sink);
  // Reads the published aggregates image; false when absent/invalid.
  bool LoadGroups(core::AuxEngineServices &,
                  std::shared_ptr<const Q24GroupState> &) const;

private:
  SecEdgarQ24Config config;
  SecEdgarQ24InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
