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

class SecEdgarQ24InitialScan {
public:
  virtual ~SecEdgarQ24InitialScan() = default;
  virtual void ScanNum(
      const std::function<void(const SecEdgarQ24NumInput &)> &) = 0;
  virtual void ScanPre(
      const std::function<void(const SecEdgarQ24PreInput &)> &) = 0;
};

struct SecEdgarQ24MembershipKey {
  std::string adsh;
  std::string tag;
  std::string version;

  bool operator==(const SecEdgarQ24MembershipKey &other) const {
    return adsh == other.adsh && tag == other.tag && version == other.version;
  }
};

struct SecEdgarQ24MembershipKeyHash {
  std::size_t operator()(const SecEdgarQ24MembershipKey &key) const noexcept;
};

struct SecEdgarQ24TagVersionKey {
  std::string tag;
  std::string version;

  bool operator==(const SecEdgarQ24TagVersionKey &other) const {
    return tag == other.tag && version == other.version;
  }
  bool operator<(const SecEdgarQ24TagVersionKey &other) const {
    return tag < other.tag || (tag == other.tag && version < other.version);
  }
};

struct SecEdgarQ24TagVersionKeyHash {
  std::size_t operator()(const SecEdgarQ24TagVersionKey &key) const noexcept;
};

struct SecEdgarQ24Aggregate {
  int64_t count = 0;
  double total = 0.0;
};

struct SecEdgarQ24OutputRow {
  std::string tag;
  std::string version;
  int64_t count = 0;
  double total = 0.0;
};

struct SecEdgarQ24State {
  std::unordered_set<SecEdgarQ24MembershipKey, SecEdgarQ24MembershipKeyHash>
      pre_membership;
  std::vector<SecEdgarQ24TagVersionKey> tag_versions;
  std::vector<SecEdgarQ24Aggregate> aggregates;
};

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

bool DecodeSecEdgarQ24PreMembership(const std::vector<uint8_t> &,
                                    SecEdgarQ24State &);
bool DecodeSecEdgarQ24TagVersions(const std::vector<uint8_t> &,
                                  SecEdgarQ24State &);
bool DecodeSecEdgarQ24Aggregates(const std::vector<uint8_t> &,
                                 SecEdgarQ24State &);

std::vector<SecEdgarQ24OutputRow>
BuildSecEdgarQ24Output(const SecEdgarQ24State &);

class SecEdgarQ24TxnBuffer final : public core::AuxTxnBuffer {
public:
  struct NumDelta {
    SecEdgarQ24MembershipKey membership;
    SecEdgarQ24TagVersionKey tag_version;
    double value = 0.0;
    int8_t sign = 0;
    bool relevant = false;
    bool excluded_by_pre = false;
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

class SecEdgarQ24Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = core::SecEdgarQ24Layout::kAccelerator;

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

private:
  SecEdgarQ24State BuildState(core::AuxEngineServices &) const;
  bool LoadState(core::AuxEngineServices &, SecEdgarQ24State &) const;
  void EmitOutput(const SecEdgarQ24State &, core::AuxRowSink &) const;

  SecEdgarQ24Config config;
  SecEdgarQ24InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
