#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "accelerator_codegen/aux_state/core/aux_query_module.hpp"
#include "accelerator_codegen/aux_state/core/query_layouts.hpp"

namespace accelerator_codegen::aux_state::query {

struct SecEdgarQ3Config {
  core::TableId num_table = 0;
  core::TableId sub_table = 0;
};

struct SecEdgarQ3NumInput {
  std::string adsh;
  std::string uom;
  double value = 0.0;
  bool value_is_null = true;
};

struct SecEdgarQ3SubInput {
  std::string adsh;
  std::string name;
  int32_t cik = 0;
  int32_t fy = 0;
};

class SecEdgarQ3InitialScan {
public:
  virtual ~SecEdgarQ3InitialScan() = default;
  virtual void ScanNum(
      const std::function<void(const SecEdgarQ3NumInput &)> &) = 0;
  virtual void ScanSub(
      const std::function<void(const SecEdgarQ3SubInput &)> &) = 0;
};

struct SecEdgarQ3NameCikKey {
  std::string name;
  int32_t cik = 0;

  bool operator==(const SecEdgarQ3NameCikKey &other) const {
    return cik == other.cik && name == other.name;
  }
};

struct SecEdgarQ3NameCikKeyHash {
  std::size_t operator()(const SecEdgarQ3NameCikKey &key) const noexcept;
};

struct SecEdgarQ3FilingInfo {
  std::string name;
  int32_t cik = 0;
  uint32_t group_id = 0;
};

struct SecEdgarQ3OutputRow {
  std::string name;
  int32_t cik = 0;
  double total_value = 0.0;
};

struct SecEdgarQ3State {
  std::unordered_map<std::string, SecEdgarQ3FilingInfo> adsh_to_filing;
  std::unordered_map<int32_t, double> cik_totals;
  std::vector<std::string> group_names;
  std::vector<int32_t> group_ciks;
  std::vector<double> group_sums;
};

enum class SecEdgarQ3ImageKind : uint8_t {
  Fy2022Cohort,
  CikTotals,
};

struct SecEdgarQ3StateImage {
  SecEdgarQ3ImageKind kind = SecEdgarQ3ImageKind::Fy2022Cohort;
  std::shared_ptr<const SecEdgarQ3State> state;
};

std::vector<uint8_t> EncodeSecEdgarQ3Fy2022Cohort(const SecEdgarQ3State &);
std::vector<uint8_t> EncodeSecEdgarQ3CikTotals(const SecEdgarQ3State &);

bool DecodeSecEdgarQ3Fy2022Cohort(const std::vector<uint8_t> &, SecEdgarQ3State &);
bool DecodeSecEdgarQ3CikTotals(const std::vector<uint8_t> &, SecEdgarQ3State &);

std::vector<SecEdgarQ3OutputRow> BuildSecEdgarQ3Output(const SecEdgarQ3State &);

class SecEdgarQ3TxnBuffer final : public core::AuxTxnBuffer {
public:
  struct NumDelta {
    std::string adsh;
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

class SecEdgarQ3Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = core::SecEdgarQ3Layout::kAccelerator;

  explicit SecEdgarQ3Module(SecEdgarQ3Config config,
                            SecEdgarQ3InitialScan *initial_scan = nullptr)
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
  SecEdgarQ3State BuildState(core::AuxEngineServices &) const;
  void EmitOutput(const SecEdgarQ3State &, core::AuxRowSink &) const;

  SecEdgarQ3Config config;
  SecEdgarQ3InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
