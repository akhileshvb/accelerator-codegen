#include "sec_edgar/q6/duckdb/aux/module.hpp"
#include "accelerator_codegen/stateful/core/byte_stream.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>
#include "sec_edgar/q6/layout.hpp"

namespace accelerator_codegen::aux_state::query {
namespace {

using core::ByteReader;
using core::ByteWriter;

constexpr uint32_t kFormatVersion = 1;
constexpr uint32_t kCohortMagic = 0x43303651; // Q6C
constexpr uint32_t kPreMagic = 0x50303651;    // Q6P
constexpr uint32_t kAggregateMagic = 0x41303651; // Q6A
constexpr int32_t kTargetFy = 2023;
constexpr std::size_t kOutputLimit = 200;


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
  std::string version;
  std::string uom;
  double value = 0.0;
  bool value_is_null = true;
};

bool ReadNumRow(const core::AuxRow &row, NumRow &result) {
  return ReadString(row, 0, result.adsh) && ReadString(row, 1, result.tag) &&
         ReadString(row, 2, result.version) && ReadString(row, 5, result.uom) &&
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

struct PreRow {
  std::string adsh;
  std::string tag;
  std::string version;
  std::string stmt;
  std::string plabel;
};

bool ReadPreRow(const core::AuxRow &row, PreRow &result) {
  return ReadString(row, 0, result.adsh) && ReadString(row, 1, result.tag) &&
         ReadString(row, 2, result.version) && ReadString(row, 3, result.stmt) &&
         ReadString(row, 4, result.plabel);
}

bool IsRelevantNum(const NumRow &row) {
  return row.uom == "USD" && !row.value_is_null;
}

void ApplyNumContribution(SecEdgarQ6State &state, const std::string &adsh,
                            const std::string &tag, const std::string &version,
                            double value, int8_t sign) {
  const auto name_it = state.adsh_to_name.find(adsh);
  if (name_it == state.adsh_to_name.end()) {
    return;
  }
  const SecEdgarQ6PreKey pre_key{adsh, tag, version};
  const auto pre_it = state.pre_plabels.find(pre_key);
  if (pre_it == state.pre_plabels.end()) {
    return;
  }
  const auto scaled = static_cast<double>(sign) * value;
  for (const auto &plabel : pre_it->second) {
    SecEdgarQ6GroupKey group_key{name_it->second, tag, plabel};
    auto &aggregate = state.aggregates[group_key];
    aggregate.sum += scaled;
    aggregate.count += sign;
  }
}

std::shared_ptr<const SecEdgarQ6StateImage>
MakeStateImage(SecEdgarQ6ImageKind kind,
               const std::shared_ptr<const SecEdgarQ6State> &state) {
  auto image = std::make_shared<SecEdgarQ6StateImage>();
  image->kind = kind;
  image->state = state;
  return image;
}

const SecEdgarQ6StateImage *AsQ6Image(const core::StateImage &image,
                                      SecEdgarQ6ImageKind kind) {
  const auto *payload = image.As<SecEdgarQ6StateImage>();
  return payload && payload->kind == kind && payload->state ? payload
                                                              : nullptr;
}

void EmitStateImages(const SecEdgarQ6State &state, core::StateBuilder &builder) {
  const auto typed_state = std::make_shared<const SecEdgarQ6State>(state);
  builder.Write(core::StateImage::Make(
      core::SecEdgarQ6Layout::kAccelerator,
      core::SecEdgarQ6Layout::kFy2023Cohort, 0,
      MakeStateImage(SecEdgarQ6ImageKind::Fy2023Cohort, typed_state)));
  builder.Write(core::StateImage::Make(
      core::SecEdgarQ6Layout::kAccelerator, core::SecEdgarQ6Layout::kPreIsBridge,
      0, MakeStateImage(SecEdgarQ6ImageKind::PreIsBridge, typed_state)));
  builder.Write(core::StateImage::Make(
      core::SecEdgarQ6Layout::kAccelerator,
      core::SecEdgarQ6Layout::kGroupAggregates, 0,
      MakeStateImage(SecEdgarQ6ImageKind::GroupAggregates, typed_state)));
  builder.Finish();
}

core::InvalidationPlan AllPartitions() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

core::InvalidationPlan AggregatePartitions() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
  plan.partitions.insert(
      static_cast<::accelerator_codegen::aux_state::PartitionKey>(
          core::SecEdgarQ6Layout::kGroupAggregates));
  return plan;
}

} // namespace

std::size_t SecEdgarQ6PreKeyHash::operator()(
    const SecEdgarQ6PreKey &key) const noexcept {
  return std::hash<std::string>{}(key.adsh) ^
         (std::hash<std::string>{}(key.tag) << 1) ^
         (std::hash<std::string>{}(key.version) << 2);
}

std::size_t SecEdgarQ6GroupKeyHash::operator()(
    const SecEdgarQ6GroupKey &key) const noexcept {
  return std::hash<std::string>{}(key.name) ^
         (std::hash<std::string>{}(key.tag) << 1) ^
         (std::hash<std::string>{}(key.plabel) << 2);
}

std::vector<uint8_t> EncodeSecEdgarQ6Fy2023Cohort(const SecEdgarQ6State &state) {
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

std::vector<uint8_t> EncodeSecEdgarQ6PreIsBridge(const SecEdgarQ6State &state) {
  std::vector<SecEdgarQ6PreKey> keys;
  keys.reserve(state.pre_plabels.size());
  for (const auto &[key, _] : state.pre_plabels) {
    keys.push_back(key);
  }
  std::sort(keys.begin(), keys.end(),
            [](const SecEdgarQ6PreKey &lhs, const SecEdgarQ6PreKey &rhs) {
              return lhs.adsh < rhs.adsh ||
                     (lhs.adsh == rhs.adsh &&
                      (lhs.tag < rhs.tag ||
                       (lhs.tag == rhs.tag && lhs.version < rhs.version)));
            });
  ByteWriter writer;
  core::WriteImageHeader(writer, kPreMagic, kFormatVersion, static_cast<uint32_t>(keys.size()));
  for (const auto &key : keys) {
    writer.String(key.adsh);
    writer.String(key.tag);
    writer.String(key.version);
    const auto &plabels = state.pre_plabels.at(key);
    writer.U32(static_cast<uint32_t>(plabels.size()));
    for (const auto &plabel : plabels) {
      writer.String(plabel);
    }
  }
  return std::move(writer.bytes);
}

std::vector<uint8_t>
EncodeSecEdgarQ6GroupAggregates(const SecEdgarQ6State &state) {
  std::vector<SecEdgarQ6GroupKey> keys;
  keys.reserve(state.aggregates.size());
  for (const auto &[key, _] : state.aggregates) {
    keys.push_back(key);
  }
  std::sort(keys.begin(), keys.end());
  ByteWriter writer;
  core::WriteImageHeader(writer, kAggregateMagic, kFormatVersion, static_cast<uint32_t>(keys.size()));
  for (const auto &key : keys) {
    const auto &aggregate = state.aggregates.at(key);
    writer.String(key.name);
    writer.String(key.tag);
    writer.String(key.plabel);
    writer.Double(aggregate.sum);
    writer.I64(aggregate.count);
  }
  return std::move(writer.bytes);
}

bool DecodeSecEdgarQ6Fy2023Cohort(const std::vector<uint8_t> &bytes,
                                  SecEdgarQ6State &state) {
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

bool DecodeSecEdgarQ6PreIsBridge(const std::vector<uint8_t> &bytes,
                                 SecEdgarQ6State &state) {
  ByteReader reader{bytes};
  uint32_t count = 0;
  if (!core::ReadImageHeader(reader, kPreMagic, kFormatVersion, count)) {
    return false;
  }
  state.pre_plabels.clear();
  for (uint32_t i = 0; i < count; ++i) {
    SecEdgarQ6PreKey key;
    uint32_t plabel_count = 0;
    if (!reader.String(key.adsh) || !reader.String(key.tag) ||
        !reader.String(key.version) || !reader.U32(plabel_count)) {
      return false;
    }
    std::vector<std::string> plabels;
    plabels.reserve(plabel_count);
    for (uint32_t j = 0; j < plabel_count; ++j) {
      std::string plabel;
      if (!reader.String(plabel)) {
        return false;
      }
      plabels.push_back(std::move(plabel));
    }
    state.pre_plabels.emplace(std::move(key), std::move(plabels));
  }
  return reader.offset == bytes.size();
}

bool DecodeSecEdgarQ6GroupAggregates(const std::vector<uint8_t> &bytes,
                                     SecEdgarQ6State &state) {
  ByteReader reader{bytes};
  uint32_t count = 0;
  if (!core::ReadImageHeader(reader, kAggregateMagic, kFormatVersion, count)) {
    return false;
  }
  state.aggregates.clear();
  for (uint32_t i = 0; i < count; ++i) {
    SecEdgarQ6GroupKey key;
    SecEdgarQ6Aggregate aggregate;
    if (!reader.String(key.name) || !reader.String(key.tag) ||
        !reader.String(key.plabel) || !reader.Double(aggregate.sum) ||
        !reader.I64(aggregate.count)) {
      return false;
    }
    state.aggregates.emplace(std::move(key), aggregate);
  }
  return reader.offset == bytes.size();
}

std::vector<SecEdgarQ6OutputRow>
BuildSecEdgarQ6Output(const SecEdgarQ6State &state) {
  std::vector<SecEdgarQ6GroupKey> active;
  active.reserve(state.aggregates.size());
  for (const auto &[key, aggregate] : state.aggregates) {
    if (aggregate.count != 0) {
      active.push_back(key);
    }
  }
  const auto compare = [&](const SecEdgarQ6GroupKey &lhs,
                           const SecEdgarQ6GroupKey &rhs) {
    const auto &left = state.aggregates.at(lhs);
    const auto &right = state.aggregates.at(rhs);
    if (left.sum != right.sum) {
      return left.sum > right.sum;
    }
    return lhs < rhs;
  };
  const auto output_count = std::min(active.size(), kOutputLimit);
  if (active.size() > kOutputLimit) {
    std::partial_sort(active.begin(),
                      active.begin() + static_cast<ptrdiff_t>(output_count),
                      active.end(), compare);
    active.resize(output_count);
  } else {
    std::sort(active.begin(), active.end(), compare);
  }
  std::vector<SecEdgarQ6OutputRow> output;
  output.reserve(output_count);
  for (const auto &key : active) {
    const auto &aggregate = state.aggregates.at(key);
    output.push_back({key.name, "IS", key.tag, key.plabel, aggregate.sum,
                      aggregate.count});
  }
  return output;
}

bool SecEdgarQ6TxnBuffer::HasPendingChanges() const { return pending; }

void SecEdgarQ6TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  deltas.clear();
}

bool SecEdgarQ6TxnBuffer::RequiresRebuild() const { return requires_rebuild; }

const std::vector<SecEdgarQ6TxnBuffer::NumDelta> &
SecEdgarQ6TxnBuffer::Deltas() const {
  return deltas;
}

void SecEdgarQ6TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  deltas.clear();
}

void SecEdgarQ6TxnBuffer::AddDelta(NumDelta delta) {
  pending = true;
  if (delta.relevant) {
    deltas.push_back(std::move(delta));
  }
}

core::AcceleratorId SecEdgarQ6Module::Id() const { return kId; }

std::vector<core::TableDependency> SecEdgarQ6Module::Dependencies() const {
  return {{config.num_table, {0, 1, 2, 3, 4}},
          {config.sub_table, {0, 1, 2}},
          {config.pre_table, {0, 1, 2, 3, 4}}};
}

std::unique_ptr<core::AuxTxnBuffer> SecEdgarQ6Module::CreateTxnBuffer() const {
  return std::make_unique<SecEdgarQ6TxnBuffer>();
}

void SecEdgarQ6Module::Build(core::AuxEngineServices &services,
                             core::StateBuilder &builder) {
  EmitStateImages(BuildState(services), builder);
}

core::InvalidationPlan
SecEdgarQ6Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.sub_table || mutation.table == config.pre_table) {
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
  return AggregatePartitions();
}

void SecEdgarQ6Module::AccumulateMutation(const core::MutationBatch &mutation,
                                          core::AuxEngineServices &,
                                          core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<SecEdgarQ6TxnBuffer &>(base_buffer);
  if (mutation.table == config.sub_table || mutation.table == config.pre_table) {
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
    buffer.AddDelta({{parsed.adsh, parsed.tag, parsed.version}, parsed.value,
                     sign, IsRelevantNum(parsed)});
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
    if (mutation.old_rows.size() != mutation.new_rows.size()) {
      buffer.MarkRebuild();
      return;
    }
    for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
      add(mutation.old_rows[i], -1);
      add(mutation.new_rows[i], 1);
    }
    break;
  }
}

core::StagedMaintenanceOutcome
SecEdgarQ6Module::StageMaintenance(core::AuxEngineServices &services,
                                   const core::InvalidationPlan &,
                                   core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<SecEdgarQ6TxnBuffer &>(base_buffer);
  core::StagedMaintenanceOutcome outcome;
  if (buffer.RequiresRebuild()) {
    const auto decision = services.ChooseMaintenance(
        {0, 0, false, false, true, true, false,
         core::MaintenanceAction::NoOp,
         services.ExpectedQueriesBeforeNextWrite()});
    outcome.action = decision.action == core::MaintenanceAction::ApplyDelta
                         ? core::MaintenanceAction::RebuildAll
                         : decision.action;
    outcome.invalidations = AllPartitions();
    outcome.pending_rebuild = outcome.invalidations;
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
      visibility, {core::SecEdgarQ6Layout::kFy2023Cohort,
                   core::SecEdgarQ6Layout::kPreIsBridge,
                   core::SecEdgarQ6Layout::kGroupAggregates});
  SecEdgarQ6State state;
  core::Version aggregate_version = 0;
  bool have_cohort = false;
  bool have_pre = false;
  bool have_aggregates = false;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == core::SecEdgarQ6Layout::kFy2023Cohort) {
      const auto *typed = AsQ6Image(image, SecEdgarQ6ImageKind::Fy2023Cohort);
      if (typed) {
        state.adsh_to_name = typed->state->adsh_to_name;
        have_cohort = true;
      }
    } else if (image.partition == core::SecEdgarQ6Layout::kPreIsBridge) {
      const auto *typed = AsQ6Image(image, SecEdgarQ6ImageKind::PreIsBridge);
      if (typed) {
        state.pre_plabels = typed->state->pre_plabels;
        have_pre = true;
      }
    } else if (image.partition == core::SecEdgarQ6Layout::kGroupAggregates) {
      const auto *typed = AsQ6Image(image, SecEdgarQ6ImageKind::GroupAggregates);
      if (typed) {
        state.aggregates = typed->state->aggregates;
        have_aggregates = true;
      }
      aggregate_version = image.version;
    }
  }
  if (!have_cohort || !have_pre || !have_aggregates) {
    outcome.action = core::MaintenanceAction::RebuildAll;
    outcome.invalidations = AllPartitions();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }
  for (const auto &delta : buffer.Deltas()) {
    ApplyNumContribution(state, delta.membership.adsh, delta.membership.tag,
                         delta.membership.version, delta.value, delta.sign);
  }
  outcome.action = core::MaintenanceAction::ApplyDelta;
  const auto typed_state = std::make_shared<const SecEdgarQ6State>(state);
  outcome.updates.push_back(core::StateImage::Make(
      core::SecEdgarQ6Layout::kAccelerator,
      core::SecEdgarQ6Layout::kGroupAggregates, aggregate_version,
      MakeStateImage(SecEdgarQ6ImageKind::GroupAggregates, typed_state)));
  outcome.expected_version = 0;
  return outcome;
}

bool SecEdgarQ6Module::CheckValidity(core::AuxEngineServices &services,
                                     const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void SecEdgarQ6Module::Execute(core::AuxEngineServices &services,
                               const core::QueryArguments &arguments,
                               core::AuxRowSink &sink) {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(),
      {core::SecEdgarQ6Layout::kFy2023Cohort, core::SecEdgarQ6Layout::kPreIsBridge,
       core::SecEdgarQ6Layout::kGroupAggregates});
  SecEdgarQ6State state;
  bool have_cohort = false;
  bool have_pre = false;
  bool have_aggregates = false;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == core::SecEdgarQ6Layout::kFy2023Cohort) {
      const auto *typed = AsQ6Image(image, SecEdgarQ6ImageKind::Fy2023Cohort);
      if (typed) {
        state.adsh_to_name = typed->state->adsh_to_name;
        have_cohort = true;
      }
    } else if (image.partition == core::SecEdgarQ6Layout::kPreIsBridge) {
      const auto *typed = AsQ6Image(image, SecEdgarQ6ImageKind::PreIsBridge);
      if (typed) {
        state.pre_plabels = typed->state->pre_plabels;
        have_pre = true;
      }
    } else if (image.partition == core::SecEdgarQ6Layout::kGroupAggregates) {
      const auto *typed = AsQ6Image(image, SecEdgarQ6ImageKind::GroupAggregates);
      if (typed) {
        state.aggregates = typed->state->aggregates;
        have_aggregates = true;
      }
    }
  }
  if (!snapshot.available || !have_cohort || !have_pre || !have_aggregates) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  EmitOutput(state, sink);
}

void SecEdgarQ6Module::ExecuteFallback(core::AuxEngineServices &services,
                                       const core::QueryArguments &,
                                       core::AuxRowSink &sink) {
  EmitOutput(BuildState(services), sink);
}

SecEdgarQ6State SecEdgarQ6Module::BuildState(core::AuxEngineServices &services) const {
  (void)services;
  SecEdgarQ6State state;
  if (!initial_scan) {
    throw std::logic_error(
        "SEC EDGAR Q6 requires a typed initial scan source");
  }
  initial_scan->ScanSub([&](const SecEdgarQ6SubInput &input) {
    if (input.fy == kTargetFy) {
      state.adsh_to_name.emplace(input.adsh, input.name);
    }
  });
  // Restrict the IS bridge to the fy2023 cohort. Non-cohort adshs never
  // contribute to aggregates (ApplyNumContribution checks adsh_to_name).
  initial_scan->ScanPre([&](const SecEdgarQ6PreInput &input) {
    if (input.stmt != "IS") {
      return;
    }
    if (state.adsh_to_name.find(input.adsh) == state.adsh_to_name.end()) {
      return;
    }
    state.pre_plabels[{input.adsh, input.tag, input.version}].push_back(
        input.plabel);
  });
  initial_scan->ScanNum([&](const SecEdgarQ6NumInput &input) {
    if (input.uom != "USD" || input.value_is_null) {
      return;
    }
    ApplyNumContribution(state, input.adsh, input.tag, input.version,
                           input.value, 1);
  });
  return state;
}

void SecEdgarQ6Module::EmitOutput(const SecEdgarQ6State &state,
                                  core::AuxRowSink &sink) const {
  sink.Begin({{{"name", core::AuxType::String, false},
               {"stmt", core::AuxType::String, false},
               {"tag", core::AuxType::String, false},
               {"plabel", core::AuxType::String, false},
               {"total_value", core::AuxType::Double, false},
               {"cnt", core::AuxType::Int64, false}}});
  for (const auto &row : BuildSecEdgarQ6Output(state)) {
    sink.WriteRow({{core::AuxValue::Of(core::AuxType::String, row.name),
                    core::AuxValue::Of(core::AuxType::String, row.stmt),
                    core::AuxValue::Of(core::AuxType::String, row.tag),
                    core::AuxValue::Of(core::AuxType::String, row.plabel),
                    core::AuxValue::Of(core::AuxType::Double, row.total_value),
                    core::AuxValue::Of(core::AuxType::Int64, row.cnt)}});
  }
  sink.End();
}

} // namespace accelerator_codegen::aux_state::query
