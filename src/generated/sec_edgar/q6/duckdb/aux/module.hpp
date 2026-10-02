#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_query_module.hpp"
#include "sec_edgar/q6/layout.hpp"

namespace accelerator_codegen::aux_state::query {

struct SecEdgarQ6Config {
  core::TableId num_table = 0;
  core::TableId sub_table = 0;
  core::TableId pre_table = 0;
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
};

struct SecEdgarQ6PreInput {
  std::string adsh;
  std::string tag;
  std::string version;
  std::string stmt;
  std::string plabel;
};

class SecEdgarQ6InitialScan {
public:
  virtual ~SecEdgarQ6InitialScan() = default;
  virtual void ScanNum(
      const std::function<void(const SecEdgarQ6NumInput &)> &) = 0;
  virtual void ScanSub(
      const std::function<void(const SecEdgarQ6SubInput &)> &) = 0;
  virtual void ScanPre(
      const std::function<void(const SecEdgarQ6PreInput &)> &) = 0;
};

struct SecEdgarQ6PreKey {
  std::string adsh;
  std::string tag;
  std::string version;

  bool operator==(const SecEdgarQ6PreKey &other) const {
    return adsh == other.adsh && tag == other.tag && version == other.version;
  }
};

struct SecEdgarQ6PreKeyHash {
  std::size_t operator()(const SecEdgarQ6PreKey &key) const noexcept;
};

struct SecEdgarQ6GroupKey {
  std::string name;
  std::string tag;
  std::string plabel;

  bool operator==(const SecEdgarQ6GroupKey &other) const {
    return name == other.name && tag == other.tag && plabel == other.plabel;
  }
  bool operator<(const SecEdgarQ6GroupKey &other) const {
    return name < other.name || (name == other.name && (tag < other.tag ||
                                                        (tag == other.tag &&
                                                         plabel < other.plabel)));
  }
};

struct SecEdgarQ6GroupKeyHash {
  std::size_t operator()(const SecEdgarQ6GroupKey &key) const noexcept;
};

struct SecEdgarQ6Aggregate {
  double sum = 0.0;
  int64_t count = 0;
};

struct SecEdgarQ6OutputRow {
  std::string name;
  std::string stmt = "IS";
  std::string tag;
  std::string plabel;
  double total_value = 0.0;
  int64_t cnt = 0;
};

struct SecEdgarQ6State {
  std::unordered_map<std::string, std::string> adsh_to_name;
  std::unordered_map<SecEdgarQ6PreKey, std::vector<std::string>, SecEdgarQ6PreKeyHash>
      pre_plabels;
  std::unordered_map<SecEdgarQ6GroupKey, SecEdgarQ6Aggregate, SecEdgarQ6GroupKeyHash>
      aggregates;
};

enum class SecEdgarQ6ImageKind : uint8_t {
  Fy2023Cohort,
  PreIsBridge,
  GroupAggregates,
};

struct SecEdgarQ6StateImage {
  SecEdgarQ6ImageKind kind = SecEdgarQ6ImageKind::Fy2023Cohort;
  std::shared_ptr<const SecEdgarQ6State> state;
};

std::vector<uint8_t> EncodeSecEdgarQ6Fy2023Cohort(const SecEdgarQ6State &);
std::vector<uint8_t> EncodeSecEdgarQ6PreIsBridge(const SecEdgarQ6State &);
std::vector<uint8_t> EncodeSecEdgarQ6GroupAggregates(const SecEdgarQ6State &);

bool DecodeSecEdgarQ6Fy2023Cohort(const std::vector<uint8_t> &, SecEdgarQ6State &);
bool DecodeSecEdgarQ6PreIsBridge(const std::vector<uint8_t> &, SecEdgarQ6State &);
bool DecodeSecEdgarQ6GroupAggregates(const std::vector<uint8_t> &, SecEdgarQ6State &);

std::vector<SecEdgarQ6OutputRow> BuildSecEdgarQ6Output(const SecEdgarQ6State &);

class SecEdgarQ6TxnBuffer final : public core::AuxTxnBuffer {
public:
  struct NumDelta {
    SecEdgarQ6PreKey membership;
    double value = 0.0;
    int8_t sign = 0;
    bool relevant = false;
  };

  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  const std::vector<NumDelta> &Deltas() const;
  void MarkRebuild();
  void AddDelta(NumDelta delta);

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<NumDelta> deltas;
};

class SecEdgarQ6Module final : public core::AuxQueryModule {
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

private:
  SecEdgarQ6State BuildState(core::AuxEngineServices &) const;
  void EmitOutput(const SecEdgarQ6State &, core::AuxRowSink &) const;

  SecEdgarQ6Config config;
  SecEdgarQ6InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
