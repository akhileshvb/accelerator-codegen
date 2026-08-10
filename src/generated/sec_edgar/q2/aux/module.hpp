#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_query_module.hpp"
#include "accelerator_codegen/stateful/core/query_layouts.hpp"

namespace accelerator_codegen::aux_state::query {

struct SecEdgarQ2Config {
  core::TableId num_table = 0;
  core::TableId sub_table = 0;
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
};

class SecEdgarQ2InitialScan {
public:
  virtual ~SecEdgarQ2InitialScan() = default;
  virtual void ScanNum(
      const std::function<void(const SecEdgarQ2NumInput &)> &) = 0;
  virtual void ScanSub(
      const std::function<void(const SecEdgarQ2SubInput &)> &) = 0;
};

struct SecEdgarQ2AdshTagKey {
  std::string adsh;
  std::string tag;

  bool operator==(const SecEdgarQ2AdshTagKey &other) const {
    return adsh == other.adsh && tag == other.tag;
  }
};

struct SecEdgarQ2AdshTagKeyHash {
  std::size_t operator()(const SecEdgarQ2AdshTagKey &key) const noexcept;
};

struct SecEdgarQ2CandidateRow {
  std::string name;
  std::string tag;
  double value = 0.0;
};

struct SecEdgarQ2OutputRow {
  std::string name;
  std::string tag;
  double value = 0.0;
};

struct SecEdgarQ2State {
  std::unordered_map<SecEdgarQ2AdshTagKey, double, SecEdgarQ2AdshTagKeyHash>
      max_by_adsh_tag;
  std::unordered_map<std::string, std::string> adsh_to_name;
  std::vector<SecEdgarQ2CandidateRow> candidates;
};

enum class SecEdgarQ2ImageKind : uint8_t {
  MaxPureMap,
  Fy2022Cohort,
  Candidates,
};

struct SecEdgarQ2StateImage {
  SecEdgarQ2ImageKind kind = SecEdgarQ2ImageKind::MaxPureMap;
  std::shared_ptr<const SecEdgarQ2State> state;
};

std::vector<uint8_t> EncodeSecEdgarQ2MaxPureMap(const SecEdgarQ2State &);
std::vector<uint8_t> EncodeSecEdgarQ2Fy2022Cohort(const SecEdgarQ2State &);
std::vector<uint8_t> EncodeSecEdgarQ2Candidates(const SecEdgarQ2State &);

bool DecodeSecEdgarQ2MaxPureMap(const std::vector<uint8_t> &, SecEdgarQ2State &);
bool DecodeSecEdgarQ2Fy2022Cohort(const std::vector<uint8_t> &, SecEdgarQ2State &);
bool DecodeSecEdgarQ2Candidates(const std::vector<uint8_t> &, SecEdgarQ2State &);

std::vector<SecEdgarQ2OutputRow> BuildSecEdgarQ2Output(const SecEdgarQ2State &);

class SecEdgarQ2TxnBuffer final : public core::AuxTxnBuffer {
public:
  struct NumDelta {
    std::string adsh;
    std::string tag;
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

class SecEdgarQ2Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = core::SecEdgarQ2Layout::kAccelerator;

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

private:
  SecEdgarQ2State BuildState(core::AuxEngineServices &) const;
  void EmitOutput(const SecEdgarQ2State &, core::AuxRowSink &) const;

  SecEdgarQ2Config config;
  SecEdgarQ2InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
