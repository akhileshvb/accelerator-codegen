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

struct SecEdgarQ4Config {
  core::TableId num_table = 0;
  core::TableId sub_table = 0;
  core::TableId tag_table = 0;
  core::TableId pre_table = 0;
};

struct SecEdgarQ4SubInput {
  std::string adsh;
  int32_t cik = 0;
  int32_t sic = 0;
};

struct SecEdgarQ4TagInput {
  std::string tag;
  std::string version;
  int32_t abstract_flag = 0;
  std::string tlabel;
};

struct SecEdgarQ4PreInput {
  std::string adsh;
  std::string tag;
  std::string version;
  std::string stmt;
};

struct SecEdgarQ4NumInput {
  std::string adsh;
  std::string tag;
  std::string version;
  std::string uom;
  double value = 0.0;
  bool value_is_null = true;
};

class SecEdgarQ4InitialScan {
public:
  virtual ~SecEdgarQ4InitialScan() = default;
  virtual void ScanSub(
      const std::function<void(const SecEdgarQ4SubInput &)> &) const = 0;
  virtual void ScanTag(
      const std::function<void(const SecEdgarQ4TagInput &)> &) const = 0;
  virtual void ScanPre(
      const std::function<void(const SecEdgarQ4PreInput &)> &) const = 0;
  virtual void ScanNum(
      const std::function<void(const SecEdgarQ4NumInput &)> &) const = 0;
};

struct SecEdgarQ4SubInfo {
  int32_t cik = 0;
  int32_t sic = 0;
  uint32_t cik_dense_id = 0;
};

struct SecEdgarQ4TagVersionKey {
  std::string tag;
  std::string version;

  bool operator==(const SecEdgarQ4TagVersionKey &other) const {
    return tag == other.tag && version == other.version;
  }
  bool operator<(const SecEdgarQ4TagVersionKey &other) const {
    return tag < other.tag || (tag == other.tag && version < other.version);
  }
};

struct SecEdgarQ4TagVersionKeyHash {
  std::size_t operator()(const SecEdgarQ4TagVersionKey &key) const noexcept;
};

struct SecEdgarQ4ResolvedPreTriple {
  uint32_t group_id = 0;
  uint32_t cik_dense_id = 0;
  uint32_t multiplicity = 0;
};

struct SecEdgarQ4GroupKey {
  int32_t sic = 0;
  uint32_t tlabel_id = 0;

  bool operator==(const SecEdgarQ4GroupKey &other) const {
    return sic == other.sic && tlabel_id == other.tlabel_id;
  }
  bool operator!=(const SecEdgarQ4GroupKey &other) const {
    return !(*this == other);
  }
  bool operator<(const SecEdgarQ4GroupKey &other) const {
    return sic < other.sic || (sic == other.sic && tlabel_id < other.tlabel_id);
  }
};

struct SecEdgarQ4GroupKeyHash {
  std::size_t operator()(const SecEdgarQ4GroupKey &key) const noexcept;
};

struct SecEdgarQ4GroupAggregate {
  int64_t count = 0;
  double sum = 0.0;
  std::vector<uint64_t> cik_bits;
  uint32_t distinct_cik_count = 0;
};

struct SecEdgarQ4OutputRow {
  int32_t sic = 0;
  std::string tlabel;
  std::string stmt = "EQ";
  int64_t num_companies = 0;
  double total_value = 0.0;
  double avg_value = 0.0;
};

struct SecEdgarQ4State {
  std::unordered_map<std::string, SecEdgarQ4SubInfo> sub_lookup;
  std::unordered_map<int32_t, uint32_t> cik_dense_index;
  uint32_t cik_dense_count = 0;
  uint32_t cik_bit_word_count = 0;

  std::vector<SecEdgarQ4TagVersionKey> tag_versions;
  std::unordered_map<SecEdgarQ4TagVersionKey, uint32_t, SecEdgarQ4TagVersionKeyHash>
      tag_version_index;
  std::vector<std::string> tlabels;
  std::vector<uint32_t> tlabel_ids_by_tag_version;

  std::unordered_map<std::string,
                     std::unordered_map<uint32_t, SecEdgarQ4ResolvedPreTriple>>
      pre_by_adsh;

  std::vector<SecEdgarQ4GroupKey> group_keys;
  std::vector<SecEdgarQ4GroupAggregate> aggregates;
};

enum class SecEdgarQ4ImageKind : uint8_t {
  SicCohort,
  PreEqBridge,
  TagTlabel,
  GroupAggregates,
};

struct SecEdgarQ4StateImage {
  SecEdgarQ4ImageKind kind = SecEdgarQ4ImageKind::SicCohort;
  std::shared_ptr<const SecEdgarQ4State> state;
};

std::vector<uint8_t> EncodeSecEdgarQ4SicCohort(const SecEdgarQ4State &);
std::vector<uint8_t> EncodeSecEdgarQ4PreEqBridge(const SecEdgarQ4State &);
std::vector<uint8_t> EncodeSecEdgarQ4TagTlabel(const SecEdgarQ4State &);
std::vector<uint8_t> EncodeSecEdgarQ4GroupAggregates(const SecEdgarQ4State &);

bool DecodeSecEdgarQ4SicCohort(const std::vector<uint8_t> &, SecEdgarQ4State &);
bool DecodeSecEdgarQ4PreEqBridge(const std::vector<uint8_t> &, SecEdgarQ4State &);
bool DecodeSecEdgarQ4TagTlabel(const std::vector<uint8_t> &, SecEdgarQ4State &);
bool DecodeSecEdgarQ4GroupAggregates(const std::vector<uint8_t> &,
                                     SecEdgarQ4State &);

std::vector<SecEdgarQ4OutputRow> BuildSecEdgarQ4Output(const SecEdgarQ4State &);

class SecEdgarQ4TxnBuffer final : public core::AuxTxnBuffer {
public:
  struct NumDelta {
    std::string adsh;
    SecEdgarQ4TagVersionKey tag_version;
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

class SecEdgarQ4Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = core::SecEdgarQ4Layout::kAccelerator;

  explicit SecEdgarQ4Module(SecEdgarQ4Config config,
                            SecEdgarQ4InitialScan *initial_scan = nullptr)
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
  SecEdgarQ4State BuildState(core::AuxEngineServices &) const;
  bool LoadState(core::AuxEngineServices &, SecEdgarQ4State &) const;
  void EmitOutput(const SecEdgarQ4State &, core::AuxRowSink &) const;

  SecEdgarQ4Config config;
  SecEdgarQ4InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
