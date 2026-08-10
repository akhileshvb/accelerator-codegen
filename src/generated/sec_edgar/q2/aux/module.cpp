#include "sec_edgar/q2/aux/module.hpp"
#include "accelerator_codegen/stateful/core/byte_stream.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {

using core::ByteReader;
using core::ByteWriter;

constexpr uint32_t kFormatVersion = 1;
constexpr uint32_t kMaxMapMagic = 0x4d303251; // Q2M
constexpr uint32_t kCohortMagic = 0x43303251;  // Q2C
constexpr uint32_t kCandidatesMagic = 0x4b303251; // Q2K
constexpr int32_t kTargetFy = 2022;
constexpr std::size_t kOutputLimit = 100;


bool ReadString(const core::AuxRow &row, std::size_t index, std::string &value) {
  if (index >= row.values.size()) {
    return false;
  }
  const auto &cell = row.values[index];
  if (cell.is_null || !std::holds_alternative<std::string>(cell.payload)) {
    return false;
  }
  value = std::get<std::string>(cell.payload);
  return true;
}

bool ReadDouble(const core::AuxRow &row, std::size_t index, double &value,
                bool &is_null) {
  if (index >= row.values.size()) {
    return false;
  }
  const auto &cell = row.values[index];
  if (cell.is_null) {
    is_null = true;
    return true;
  }
  is_null = false;
  if (std::holds_alternative<double>(cell.payload)) {
    value = std::get<double>(cell.payload);
    return true;
  }
  if (std::holds_alternative<int64_t>(cell.payload)) {
    value = static_cast<double>(std::get<int64_t>(cell.payload));
    return true;
  }
  if (std::holds_alternative<core::ScaledDecimal>(cell.payload)) {
    const auto decimal = std::get<core::ScaledDecimal>(cell.payload);
    value = static_cast<double>(decimal.value);
    for (int32_t scale = 0; scale < decimal.scale; ++scale) {
      value /= 10.0;
    }
    return true;
  }
  return false;
}

bool ReadInt32(const core::AuxRow &row, std::size_t index, int32_t &value) {
  if (index >= row.values.size() || row.values[index].is_null) {
    return false;
  }
  const auto &cell = row.values[index];
  if (std::holds_alternative<int64_t>(cell.payload)) {
    const auto raw = std::get<int64_t>(cell.payload);
    if (raw < std::numeric_limits<int32_t>::min() ||
        raw > std::numeric_limits<int32_t>::max()) {
      return false;
    }
    value = static_cast<int32_t>(raw);
    return true;
  }
  return false;
}

struct NumRow {
  std::string adsh;
  std::string tag;
  std::string uom;
  double value = 0.0;
  bool value_is_null = true;
};

bool ReadNumRow(const core::AuxRow &row, NumRow &result) {
  // Mutation rows use the full num table layout from generic table id 103.
  return ReadString(row, 0, result.adsh) && ReadString(row, 1, result.tag) &&
         ReadString(row, 5, result.uom) &&
         ReadDouble(row, 7, result.value, result.value_is_null);
}

struct SubRow {
  std::string adsh;
  std::string name;
  int32_t fy = 0;
};

bool ReadSubRow(const core::AuxRow &row, SubRow &result) {
  return ReadString(row, 0, result.adsh) && ReadString(row, 1, result.name) &&
         ReadInt32(row, 2, result.fy);
}

bool IsRelevantNum(const NumRow &row) {
  return row.uom == "pure" && !row.value_is_null;
}

void RefreshCandidates(SecEdgarQ2State &state,
                       const std::function<void(
                           const std::function<void(const SecEdgarQ2NumInput &)> &)>
                           &scan_num) {
  state.candidates.clear();
  scan_num([&](const SecEdgarQ2NumInput &input) {
    if (input.uom != "pure" || input.value_is_null) {
      return;
    }
    if (state.adsh_to_name.find(input.adsh) == state.adsh_to_name.end()) {
      return;
    }
    const SecEdgarQ2AdshTagKey key{input.adsh, input.tag};
    const auto max_it = state.max_by_adsh_tag.find(key);
    if (max_it == state.max_by_adsh_tag.end() || max_it->second != input.value) {
      return;
    }
    state.candidates.push_back(
        {state.adsh_to_name.at(input.adsh), input.tag, input.value});
  });
}

bool ApplyPureNumInsert(SecEdgarQ2State &state, const std::string &adsh,
                        const std::string &tag, double value) {
  const auto cohort_it = state.adsh_to_name.find(adsh);
  if (cohort_it == state.adsh_to_name.end()) {
    return true;
  }
  const SecEdgarQ2AdshTagKey group_key{adsh, tag};
  const auto max_it = state.max_by_adsh_tag.find(group_key);
  const double old_max = max_it == state.max_by_adsh_tag.end()
                             ? -std::numeric_limits<double>::infinity()
                             : max_it->second;
  if (value > old_max) {
    if (max_it == state.max_by_adsh_tag.end()) {
      state.max_by_adsh_tag.emplace(group_key, value);
    } else {
      max_it->second = value;
    }
    auto &rows = state.candidates;
    rows.erase(std::remove_if(rows.begin(), rows.end(),
                              [&](const SecEdgarQ2CandidateRow &row) {
                                return row.name == cohort_it->second &&
                                       row.tag == tag;
                              }),
               rows.end());
    rows.push_back({cohort_it->second, tag, value});
  } else if (value == old_max) {
    state.candidates.push_back({cohort_it->second, tag, value});
  }
  return true;
}

std::shared_ptr<const SecEdgarQ2StateImage>
MakeStateImage(SecEdgarQ2ImageKind kind,
               const std::shared_ptr<const SecEdgarQ2State> &state) {
  auto image = std::make_shared<SecEdgarQ2StateImage>();
  image->kind = kind;
  image->state = state;
  return image;
}

const SecEdgarQ2StateImage *AsQ2Image(const core::StateImage &image,
                                      SecEdgarQ2ImageKind kind) {
  const auto *payload = image.As<SecEdgarQ2StateImage>();
  return payload && payload->kind == kind && payload->state ? payload
                                                              : nullptr;
}

void EmitStateImages(const SecEdgarQ2State &state, core::StateBuilder &builder) {
  const auto typed_state = std::make_shared<const SecEdgarQ2State>(state);
  builder.Write(core::StateImage::Make(
      core::SecEdgarQ2Layout::kAccelerator, core::SecEdgarQ2Layout::kMaxPureMap,
      0, MakeStateImage(SecEdgarQ2ImageKind::MaxPureMap, typed_state)));
  builder.Write(core::StateImage::Make(
      core::SecEdgarQ2Layout::kAccelerator,
      core::SecEdgarQ2Layout::kFy2022Cohort, 0,
      MakeStateImage(SecEdgarQ2ImageKind::Fy2022Cohort, typed_state)));
  builder.Write(core::StateImage::Make(
      core::SecEdgarQ2Layout::kAccelerator, core::SecEdgarQ2Layout::kCandidates,
      0, MakeStateImage(SecEdgarQ2ImageKind::Candidates, typed_state)));
  builder.Finish();
}

core::InvalidationPlan AllPartitions() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

core::InvalidationPlan CandidatePartitions() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
  plan.partitions.insert(
      static_cast<::accelerator_codegen::aux_state::PartitionKey>(
          core::SecEdgarQ2Layout::kMaxPureMap));
  plan.partitions.insert(
      static_cast<::accelerator_codegen::aux_state::PartitionKey>(
          core::SecEdgarQ2Layout::kCandidates));
  return plan;
}

} // namespace

std::size_t SecEdgarQ2AdshTagKeyHash::operator()(
    const SecEdgarQ2AdshTagKey &key) const noexcept {
  return std::hash<std::string>{}(key.adsh) ^
         (std::hash<std::string>{}(key.tag) << 1);
}

std::vector<uint8_t> EncodeSecEdgarQ2MaxPureMap(const SecEdgarQ2State &state) {
  std::vector<SecEdgarQ2AdshTagKey> keys;
  keys.reserve(state.max_by_adsh_tag.size());
  for (const auto &[key, _] : state.max_by_adsh_tag) {
    keys.push_back(key);
  }
  std::sort(keys.begin(), keys.end(),
            [](const auto &lhs, const auto &rhs) {
              return lhs.adsh < rhs.adsh ||
                     (lhs.adsh == rhs.adsh && lhs.tag < rhs.tag);
            });
  ByteWriter writer;
  core::WriteImageHeader(writer, kMaxMapMagic, kFormatVersion, static_cast<uint32_t>(keys.size()));
  for (const auto &key : keys) {
    writer.String(key.adsh);
    writer.String(key.tag);
    writer.Double(state.max_by_adsh_tag.at(key));
  }
  return std::move(writer.bytes);
}

std::vector<uint8_t> EncodeSecEdgarQ2Fy2022Cohort(const SecEdgarQ2State &state) {
  std::vector<std::pair<std::string, std::string>> entries(
      state.adsh_to_name.begin(), state.adsh_to_name.end());
  std::sort(entries.begin(), entries.end());
  ByteWriter writer;
  core::WriteImageHeader(writer, kCohortMagic, kFormatVersion, static_cast<uint32_t>(entries.size()));
  for (const auto &[adsh, name] : entries) {
    writer.String(adsh);
    writer.String(name);
  }
  return std::move(writer.bytes);
}

std::vector<uint8_t> EncodeSecEdgarQ2Candidates(const SecEdgarQ2State &state) {
  ByteWriter writer;
  core::WriteImageHeader(writer, kCandidatesMagic, kFormatVersion, static_cast<uint32_t>(state.candidates.size()));
  for (const auto &row : state.candidates) {
    writer.String(row.name);
    writer.String(row.tag);
    writer.Double(row.value);
  }
  return std::move(writer.bytes);
}

bool DecodeSecEdgarQ2MaxPureMap(const std::vector<uint8_t> &bytes,
                                SecEdgarQ2State &state) {
  ByteReader reader{bytes};
  uint32_t count = 0;
  if (!core::ReadImageHeader(reader, kMaxMapMagic, kFormatVersion, count)) {
    return false;
  }
  state.max_by_adsh_tag.clear();
  for (uint32_t i = 0; i < count; ++i) {
    SecEdgarQ2AdshTagKey key;
    double value = 0.0;
    if (!reader.String(key.adsh) || !reader.String(key.tag) ||
        !reader.Double(value)) {
      return false;
    }
    state.max_by_adsh_tag.emplace(std::move(key), value);
  }
  return reader.offset == bytes.size();
}

bool DecodeSecEdgarQ2Fy2022Cohort(const std::vector<uint8_t> &bytes,
                                  SecEdgarQ2State &state) {
  ByteReader reader{bytes};
  uint32_t count = 0;
  if (!core::ReadImageHeader(reader, kCohortMagic, kFormatVersion, count)) {
    return false;
  }
  state.adsh_to_name.clear();
  for (uint32_t i = 0; i < count; ++i) {
    std::string adsh;
    std::string name;
    if (!reader.String(adsh) || !reader.String(name)) {
      return false;
    }
    state.adsh_to_name.emplace(std::move(adsh), std::move(name));
  }
  return reader.offset == bytes.size();
}

bool DecodeSecEdgarQ2Candidates(const std::vector<uint8_t> &bytes,
                                SecEdgarQ2State &state) {
  ByteReader reader{bytes};
  uint32_t count = 0;
  if (!core::ReadImageHeader(reader, kCandidatesMagic, kFormatVersion, count)) {
    return false;
  }
  state.candidates.clear();
  state.candidates.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    SecEdgarQ2CandidateRow row;
    if (!reader.String(row.name) || !reader.String(row.tag) ||
        !reader.Double(row.value)) {
      return false;
    }
    state.candidates.push_back(std::move(row));
  }
  return reader.offset == bytes.size();
}

std::vector<SecEdgarQ2OutputRow>
BuildSecEdgarQ2Output(const SecEdgarQ2State &state) {
  std::vector<SecEdgarQ2CandidateRow> candidates = state.candidates;
  std::sort(candidates.begin(), candidates.end(),
            [](const SecEdgarQ2CandidateRow &lhs, const SecEdgarQ2CandidateRow &rhs) {
              if (lhs.value != rhs.value) {
                return lhs.value > rhs.value;
              }
              if (lhs.name != rhs.name) {
                return lhs.name < rhs.name;
              }
              return lhs.tag < rhs.tag;
            });
  if (candidates.size() > kOutputLimit) {
    candidates.resize(kOutputLimit);
  }
  std::vector<SecEdgarQ2OutputRow> output;
  output.reserve(candidates.size());
  for (const auto &row : candidates) {
    output.push_back({row.name, row.tag, row.value});
  }
  return output;
}

bool SecEdgarQ2TxnBuffer::HasPendingChanges() const { return pending; }

void SecEdgarQ2TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  deltas.clear();
}

bool SecEdgarQ2TxnBuffer::RequiresRebuild() const { return requires_rebuild; }

const std::vector<SecEdgarQ2TxnBuffer::NumDelta> &
SecEdgarQ2TxnBuffer::Deltas() const {
  return deltas;
}

void SecEdgarQ2TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  deltas.clear();
}

void SecEdgarQ2TxnBuffer::AddDelta(NumDelta delta) {
  pending = true;
  if (delta.relevant) {
    deltas.push_back(std::move(delta));
  }
}

core::AcceleratorId SecEdgarQ2Module::Id() const { return kId; }

std::vector<core::TableDependency> SecEdgarQ2Module::Dependencies() const {
  return {{config.num_table, {0, 1, 2, 3}},
          {config.sub_table, {0, 1, 2}}};
}

std::unique_ptr<core::AuxTxnBuffer> SecEdgarQ2Module::CreateTxnBuffer() const {
  return std::make_unique<SecEdgarQ2TxnBuffer>();
}

void SecEdgarQ2Module::Build(core::AuxEngineServices &services,
                             core::StateBuilder &builder) {
  EmitStateImages(BuildState(services), builder);
}

core::InvalidationPlan
SecEdgarQ2Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.sub_table) {
    return AllPartitions();
  }
  if (mutation.table != config.num_table) {
    return {};
  }
  if (mutation.kind == core::MutationKind::Update &&
      !mutation.updated_columns.empty()) {
    bool relevant = false;
    for (const auto column : mutation.updated_columns) {
      if (column == 5 || column == 7) {
        relevant = true;
        break;
      }
    }
    if (!relevant) {
      return {};
    }
  }
  return CandidatePartitions();
}

void SecEdgarQ2Module::AccumulateMutation(const core::MutationBatch &mutation,
                                          core::AuxEngineServices &,
                                          core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<SecEdgarQ2TxnBuffer &>(base_buffer);
  if (mutation.table == config.sub_table) {
    buffer.MarkRebuild();
    return;
  }
  if (mutation.table != config.num_table) {
    return;
  }
  auto add = [&](const core::AuxRow &row, int8_t sign) {
    NumRow parsed;
    if (!ReadNumRow(row, parsed)) {
      buffer.MarkRebuild();
      return;
    }
    buffer.AddDelta({parsed.adsh, parsed.tag, parsed.value, sign,
                     IsRelevantNum(parsed)});
  };
  switch (mutation.kind) {
  case core::MutationKind::Insert:
    for (const auto &row : mutation.new_rows) {
      add(row, 1);
    }
    break;
  case core::MutationKind::Delete:
    for (const auto &row : mutation.old_rows) {
      add(row, -1);
    }
    break;
  case core::MutationKind::Update:
    for (const auto &row : mutation.new_rows) {
      add(row, 1);
    }
    break;
  }
}

core::StagedMaintenanceOutcome
SecEdgarQ2Module::StageMaintenance(core::AuxEngineServices &services,
                                   const core::InvalidationPlan &,
                                   core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<SecEdgarQ2TxnBuffer &>(base_buffer);
  core::StagedMaintenanceOutcome outcome;
  if (buffer.RequiresRebuild()) {
    outcome.action = core::MaintenanceAction::RebuildAll;
    class RebuildCollector final : public core::StateBuilder {
    public:
      explicit RebuildCollector(core::StagedMaintenanceOutcome &outcome_p)
          : outcome(outcome_p) {}
      void Write(core::StateImage image) override {
        outcome.updates.push_back(std::move(image));
      }
      void Finish() override {}
      core::StagedMaintenanceOutcome &outcome;
    } collector(outcome);
    EmitStateImages(BuildState(services), collector);
    outcome.expected_version = 0;
    return outcome;
  }
  if (buffer.Deltas().empty()) {
    return outcome;
  }

  const auto decision = services.ChooseMaintenance(
      {static_cast<uint64_t>(buffer.Deltas().size()), 1, true, false, true,
       false, false, core::MaintenanceAction::NoOp,
       services.ExpectedQueriesBeforeNextWrite()});
  if (decision.action != core::MaintenanceAction::ApplyDelta) {
    outcome.action = decision.action;
    outcome.invalidations = AllPartitions();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  const auto visibility = services.CurrentVisibility();
  const auto snapshot = services.StateStore().ReadPartitions(
      visibility, {core::SecEdgarQ2Layout::kMaxPureMap,
                     core::SecEdgarQ2Layout::kFy2022Cohort,
                     core::SecEdgarQ2Layout::kCandidates});
  SecEdgarQ2State state;
  core::Version max_map_version = 0;
  core::Version candidates_version = 0;
  bool have_max = false;
  bool have_cohort = false;
  bool have_candidates = false;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == core::SecEdgarQ2Layout::kMaxPureMap) {
      const auto *typed = AsQ2Image(image, SecEdgarQ2ImageKind::MaxPureMap);
      if (typed) {
        state.max_by_adsh_tag = typed->state->max_by_adsh_tag;
        have_max = true;
      }
      max_map_version = image.version;
    } else if (image.partition == core::SecEdgarQ2Layout::kFy2022Cohort) {
      const auto *typed = AsQ2Image(image, SecEdgarQ2ImageKind::Fy2022Cohort);
      if (typed) {
        state.adsh_to_name = typed->state->adsh_to_name;
        have_cohort = true;
      }
    } else if (image.partition == core::SecEdgarQ2Layout::kCandidates) {
      const auto *typed = AsQ2Image(image, SecEdgarQ2ImageKind::Candidates);
      if (typed) {
        state.candidates = typed->state->candidates;
        have_candidates = true;
      }
      candidates_version = image.version;
    }
  }
  if (!have_max || !have_cohort || !have_candidates) {
    outcome.action = core::MaintenanceAction::RebuildAll;
    outcome.invalidations = AllPartitions();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }
  for (const auto &delta : buffer.Deltas()) {
    if (delta.sign <= 0) {
      outcome.action = core::MaintenanceAction::RebuildAll;
      outcome.invalidations = AllPartitions();
      outcome.pending_rebuild = outcome.invalidations;
      return outcome;
    }
    if (!ApplyPureNumInsert(state, delta.adsh, delta.tag, delta.value)) {
      outcome.action = core::MaintenanceAction::RebuildAll;
      outcome.invalidations = AllPartitions();
      outcome.pending_rebuild = outcome.invalidations;
      return outcome;
    }
  }
  outcome.action = core::MaintenanceAction::ApplyDelta;
  const auto typed_state = std::make_shared<const SecEdgarQ2State>(state);
  outcome.updates.push_back(core::StateImage::Make(
      core::SecEdgarQ2Layout::kAccelerator, core::SecEdgarQ2Layout::kMaxPureMap,
      max_map_version,
      MakeStateImage(SecEdgarQ2ImageKind::MaxPureMap, typed_state)));
  outcome.updates.push_back(core::StateImage::Make(
      core::SecEdgarQ2Layout::kAccelerator, core::SecEdgarQ2Layout::kCandidates,
      candidates_version,
      MakeStateImage(SecEdgarQ2ImageKind::Candidates, typed_state)));
  outcome.expected_version = 0;
  return outcome;
}

bool SecEdgarQ2Module::CheckValidity(core::AuxEngineServices &services,
                                     const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void SecEdgarQ2Module::Execute(core::AuxEngineServices &services,
                               const core::QueryArguments &arguments,
                               core::AuxRowSink &sink) {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(),
      {core::SecEdgarQ2Layout::kMaxPureMap, core::SecEdgarQ2Layout::kFy2022Cohort,
       core::SecEdgarQ2Layout::kCandidates});
  SecEdgarQ2State state;
  bool have_max = false;
  bool have_cohort = false;
  bool have_candidates = false;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == core::SecEdgarQ2Layout::kMaxPureMap) {
      const auto *typed = AsQ2Image(image, SecEdgarQ2ImageKind::MaxPureMap);
      if (typed) {
        state.max_by_adsh_tag = typed->state->max_by_adsh_tag;
        have_max = true;
      }
    } else if (image.partition == core::SecEdgarQ2Layout::kFy2022Cohort) {
      const auto *typed = AsQ2Image(image, SecEdgarQ2ImageKind::Fy2022Cohort);
      if (typed) {
        state.adsh_to_name = typed->state->adsh_to_name;
        have_cohort = true;
      }
    } else if (image.partition == core::SecEdgarQ2Layout::kCandidates) {
      const auto *typed = AsQ2Image(image, SecEdgarQ2ImageKind::Candidates);
      if (typed) {
        state.candidates = typed->state->candidates;
        have_candidates = true;
      }
    }
  }
  if (!snapshot.available || !have_max || !have_cohort || !have_candidates) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  EmitOutput(state, sink);
}

void SecEdgarQ2Module::ExecuteFallback(core::AuxEngineServices &services,
                                       const core::QueryArguments &,
                                       core::AuxRowSink &sink) {
  EmitOutput(BuildState(services), sink);
}

SecEdgarQ2State SecEdgarQ2Module::BuildState(core::AuxEngineServices &services) const {
  (void)services;
  SecEdgarQ2State state;
  if (!initial_scan) {
    throw std::logic_error(
        "SEC EDGAR Q2 requires a typed initial scan source");
  }
  initial_scan->ScanSub([&](const SecEdgarQ2SubInput &input) {
    if (input.fy == kTargetFy) {
      state.adsh_to_name.emplace(input.adsh, input.name);
    }
  });
  initial_scan->ScanNum([&](const SecEdgarQ2NumInput &input) {
    if (input.uom != "pure" || input.value_is_null) {
      return;
    }
    const SecEdgarQ2AdshTagKey key{input.adsh, input.tag};
    auto it = state.max_by_adsh_tag.find(key);
    if (it == state.max_by_adsh_tag.end() || input.value > it->second) {
      state.max_by_adsh_tag[key] = input.value;
    }
  });
  RefreshCandidates(state, [&](const auto &consumer) {
    initial_scan->ScanNum(consumer);
  });
  return state;
}

void SecEdgarQ2Module::EmitOutput(const SecEdgarQ2State &state,
                                  core::AuxRowSink &sink) const {
  sink.Begin({{{"name", core::AuxType::String, false},
               {"tag", core::AuxType::String, false},
               {"value", core::AuxType::Double, false}}});
  for (const auto &row : BuildSecEdgarQ2Output(state)) {
    sink.WriteRow({{core::AuxValue::Of(core::AuxType::String, row.name),
                    core::AuxValue::Of(core::AuxType::String, row.tag),
                    core::AuxValue::Of(core::AuxType::Double, row.value)}});
  }
  sink.End();
}

} // namespace accelerator_codegen::aux_state::query
