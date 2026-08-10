#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "accelerator_codegen/aux_state/core/aux_query_module.hpp"
#include "accelerator_codegen/aux_state/core/query_layouts.hpp"

namespace accelerator_codegen::aux_state::query {

struct SecEdgarQ1Config {
  core::TableId pre_table = 0;
};

struct SecEdgarQ1PreInput {
  std::string stmt;
  std::string rfile;
  bool rfile_is_null = false;
  bool stmt_is_null = false;
  std::string adsh;
  int32_t line = 0;
};

class SecEdgarQ1InitialScan {
public:
  virtual ~SecEdgarQ1InitialScan() = default;
  virtual void ScanPre(
      const std::function<void(const SecEdgarQ1PreInput &)> &) const = 0;
};

struct SecEdgarQ1GroupKey {
  std::string stmt;
  std::string rfile;
  bool rfile_is_null = false;

  bool operator==(const SecEdgarQ1GroupKey &other) const {
    return rfile_is_null == other.rfile_is_null && stmt == other.stmt &&
           rfile == other.rfile;
  }
  bool operator!=(const SecEdgarQ1GroupKey &other) const {
    return !(*this == other);
  }
  bool operator<(const SecEdgarQ1GroupKey &other) const {
    if (stmt != other.stmt) {
      return stmt < other.stmt;
    }
    if (rfile_is_null != other.rfile_is_null) {
      return rfile_is_null;
    }
    return rfile < other.rfile;
  }
};

struct SecEdgarQ1GroupKeyHash {
  std::size_t operator()(const SecEdgarQ1GroupKey &key) const noexcept;
};

struct SecEdgarQ1GroupAggregate {
  int64_t cnt = 0;
  int64_t line_sum = 0;
  std::unordered_set<std::string> distinct_adsh;
};

struct SecEdgarQ1OutputRow {
  std::string stmt;
  std::string rfile;
  bool rfile_is_null = false;
  int64_t cnt = 0;
  int64_t num_filings = 0;
  double avg_line_num = 0.0;
};

struct SecEdgarQ1State {
  std::vector<SecEdgarQ1GroupKey> group_keys;
  std::vector<SecEdgarQ1GroupAggregate> aggregates;
};

enum class SecEdgarQ1ImageKind : uint8_t {
  GroupKeys,
  GroupAggregates,
};

struct SecEdgarQ1StateImage {
  SecEdgarQ1ImageKind kind = SecEdgarQ1ImageKind::GroupKeys;
  std::shared_ptr<const SecEdgarQ1State> state;
};

std::vector<uint8_t> EncodeSecEdgarQ1GroupKeys(const SecEdgarQ1State &);
std::vector<uint8_t> EncodeSecEdgarQ1GroupAggregates(const SecEdgarQ1State &);

bool DecodeSecEdgarQ1GroupKeys(const std::vector<uint8_t> &, SecEdgarQ1State &);
bool DecodeSecEdgarQ1GroupAggregates(const std::vector<uint8_t> &,
                                     SecEdgarQ1State &);

std::vector<SecEdgarQ1OutputRow> BuildSecEdgarQ1Output(const SecEdgarQ1State &);

class SecEdgarQ1TxnBuffer final : public core::AuxTxnBuffer {
public:
  struct PreDelta {
    SecEdgarQ1GroupKey group;
    std::string adsh;
    int32_t line = 0;
    int8_t sign = 0;
    bool relevant = false;
  };

  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  const std::vector<PreDelta> &Deltas() const;
  void MarkRebuild();
  void AddDelta(PreDelta delta);

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<PreDelta> deltas;
};

class SecEdgarQ1Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = core::SecEdgarQ1Layout::kAccelerator;

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

private:
  SecEdgarQ1State BuildState(core::AuxEngineServices &) const;
  bool LoadState(core::AuxEngineServices &, SecEdgarQ1State &) const;
  void EmitOutput(const SecEdgarQ1State &, core::AuxRowSink &) const;

  SecEdgarQ1Config config;
  SecEdgarQ1InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
