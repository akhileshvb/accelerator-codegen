#include "sec_edgar/q1/aux/module.hpp"
#include "accelerator_codegen/stateful/core/byte_stream.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {

using core::ByteReader;
using core::ByteWriter;

constexpr uint32_t kFormatVersion = 1;
constexpr uint32_t kGroupKeyMagic = 0x51314151;      // Q1AK
constexpr uint32_t kGroupAggregateMagic = 0x47314151; // Q1AG


bool ReadString(const core::AuxRow &row, std::size_t index, std::string &value,
                bool &is_null) {
  if (index >= row.values.size()) {
    return false;
  }
  const auto &cell = row.values[index];
  if (cell.is_null) {
    is_null = true;
    value.clear();
    return true;
  }
  is_null = false;
  if (!std::holds_alternative<std::string>(cell.payload)) {
    return false;
  }
  value = std::get<std::string>(cell.payload);
  return true;
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

struct PreRow {
  SecEdgarQ1GroupKey group;
  std::string adsh;
  int32_t line = 0;
  bool relevant = false;
};

bool ReadPreRow(const core::AuxRow &row, PreRow &result) {
  bool stmt_is_null = false;
  bool rfile_is_null = false;
  bool adsh_is_null = false;
  if (!ReadString(row, 0, result.group.stmt, stmt_is_null) ||
      !ReadString(row, 1, result.group.rfile, rfile_is_null) ||
      !ReadString(row, 2, result.adsh, adsh_is_null) ||
      !ReadInt32(row, 3, result.line)) {
    return false;
  }
  result.group.rfile_is_null = rfile_is_null;
  result.relevant = !stmt_is_null;
  return true;
}

uint32_t FindGroup(const std::vector<SecEdgarQ1GroupKey> &groups,
                   const SecEdgarQ1GroupKey &key) {
  const auto it = std::lower_bound(groups.begin(), groups.end(), key);
  if (it == groups.end() || *it != key) {
    return std::numeric_limits<uint32_t>::max();
  }
  return static_cast<uint32_t>(it - groups.begin());
}

void ApplyRow(SecEdgarQ1GroupAggregate &aggregate, int32_t line,
              const std::string &adsh, int8_t sign) {
  aggregate.cnt += sign;
  aggregate.line_sum += sign * static_cast<int64_t>(line);
  if (adsh.empty()) {
    return;
  }
  if (sign > 0) {
    aggregate.distinct_adsh.insert(adsh);
  } else if (sign < 0) {
    aggregate.distinct_adsh.erase(adsh);
  }
}

std::shared_ptr<const SecEdgarQ1StateImage>
MakeStateImage(SecEdgarQ1ImageKind kind,
               const std::shared_ptr<const SecEdgarQ1State> &state) {
  auto image = std::make_shared<SecEdgarQ1StateImage>();
  image->kind = kind;
  image->state = state;
  return image;
}

const SecEdgarQ1StateImage *AsQ1Image(const core::StateImage &image,
                                      SecEdgarQ1ImageKind kind) {
  const auto *payload = image.As<SecEdgarQ1StateImage>();
  return payload && payload->kind == kind && payload->state ? payload
                                                              : nullptr;
}

void EmitStateImages(const SecEdgarQ1State &state, core::StateBuilder &builder) {
  const auto typed_state = std::make_shared<const SecEdgarQ1State>(state);
  builder.Write(core::StateImage::Make(
      core::SecEdgarQ1Layout::kAccelerator, core::SecEdgarQ1Layout::kGroupKeys,
      0, MakeStateImage(SecEdgarQ1ImageKind::GroupKeys, typed_state)));
  builder.Write(core::StateImage::Make(
      core::SecEdgarQ1Layout::kAccelerator,
      core::SecEdgarQ1Layout::kGroupAggregates, 0,
      MakeStateImage(SecEdgarQ1ImageKind::GroupAggregates, typed_state)));
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
          core::SecEdgarQ1Layout::kGroupAggregates));
  return plan;
}

} // namespace

std::size_t SecEdgarQ1GroupKeyHash::operator()(
    const SecEdgarQ1GroupKey &key) const noexcept {
  return std::hash<std::string>{}(key.stmt) ^
         (std::hash<std::string>{}(key.rfile) << 1) ^
         (key.rfile_is_null ? 0x9e3779b9UL : 0UL);
}

std::vector<uint8_t> EncodeSecEdgarQ1GroupKeys(const SecEdgarQ1State &state) {
  ByteWriter writer;
  core::WriteImageHeader(writer, kGroupKeyMagic, kFormatVersion, static_cast<uint32_t>(state.group_keys.size()));
  for (const auto &key : state.group_keys) {
    writer.String(key.stmt);
    writer.U8(key.rfile_is_null ? 1 : 0);
    writer.String(key.rfile);
  }
  return std::move(writer.bytes);
}

std::vector<uint8_t>
EncodeSecEdgarQ1GroupAggregates(const SecEdgarQ1State &state) {
  ByteWriter writer;
  core::WriteImageHeader(writer, kGroupAggregateMagic, kFormatVersion, static_cast<uint32_t>(state.aggregates.size()));
  for (const auto &aggregate : state.aggregates) {
    writer.I64(aggregate.cnt);
    writer.I64(aggregate.line_sum);
    writer.U32(static_cast<uint32_t>(aggregate.distinct_adsh.size()));
    std::vector<std::string> adshs(aggregate.distinct_adsh.begin(),
                                   aggregate.distinct_adsh.end());
    std::sort(adshs.begin(), adshs.end());
    for (const auto &adsh : adshs) {
      writer.String(adsh);
    }
  }
  return std::move(writer.bytes);
}

bool DecodeSecEdgarQ1GroupKeys(const std::vector<uint8_t> &bytes,
                                 SecEdgarQ1State &state) {
  ByteReader reader{bytes};
  uint32_t count = 0;
  if (!core::ReadImageHeader(reader, kGroupKeyMagic, kFormatVersion, count)) {
    return false;
  }
  state.group_keys.clear();
  state.group_keys.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    SecEdgarQ1GroupKey key;
    uint8_t rfile_is_null = 0;
    if (!reader.String(key.stmt) || !reader.U8(rfile_is_null) ||
        !reader.String(key.rfile)) {
      return false;
    }
    key.rfile_is_null = rfile_is_null != 0;
    state.group_keys.push_back(std::move(key));
  }
  return reader.offset == bytes.size() &&
         std::is_sorted(state.group_keys.begin(), state.group_keys.end());
}

bool DecodeSecEdgarQ1GroupAggregates(const std::vector<uint8_t> &bytes,
                                     SecEdgarQ1State &state) {
  ByteReader reader{bytes};
  uint32_t count = 0;
  if (!core::ReadImageHeader(reader, kGroupAggregateMagic, kFormatVersion, count)) {
    return false;
  }
  state.aggregates.clear();
  state.aggregates.resize(count);
  for (auto &aggregate : state.aggregates) {
    uint32_t adsh_count = 0;
    if (!reader.I64(aggregate.cnt) || !reader.I64(aggregate.line_sum) ||
        !reader.U32(adsh_count)) {
      return false;
    }
    aggregate.distinct_adsh.clear();
    for (uint32_t i = 0; i < adsh_count; ++i) {
      std::string adsh;
      if (!reader.String(adsh)) {
        return false;
      }
      aggregate.distinct_adsh.insert(std::move(adsh));
    }
  }
  return reader.offset == bytes.size();
}

std::vector<SecEdgarQ1OutputRow>
BuildSecEdgarQ1Output(const SecEdgarQ1State &state) {
  std::vector<SecEdgarQ1OutputRow> output;
  output.reserve(state.group_keys.size());
  for (std::size_t i = 0; i < state.group_keys.size(); ++i) {
    if (i >= state.aggregates.size() || state.aggregates[i].cnt <= 0) {
      continue;
    }
    const auto &key = state.group_keys[i];
    const auto &aggregate = state.aggregates[i];
    SecEdgarQ1OutputRow row;
    row.stmt = key.stmt;
    row.rfile = key.rfile;
    row.rfile_is_null = key.rfile_is_null;
    row.cnt = aggregate.cnt;
    row.num_filings = static_cast<int64_t>(aggregate.distinct_adsh.size());
    if (aggregate.cnt > 0) {
      row.avg_line_num =
          static_cast<double>(aggregate.line_sum) /
          static_cast<double>(aggregate.cnt);
    }
    output.push_back(std::move(row));
  }
  std::sort(output.begin(), output.end(),
            [](const SecEdgarQ1OutputRow &lhs, const SecEdgarQ1OutputRow &rhs) {
              if (lhs.cnt != rhs.cnt) {
                return lhs.cnt > rhs.cnt;
              }
              if (lhs.stmt != rhs.stmt) {
                return lhs.stmt < rhs.stmt;
              }
              return lhs.rfile < rhs.rfile;
            });
  return output;
}

bool SecEdgarQ1TxnBuffer::HasPendingChanges() const { return pending; }

void SecEdgarQ1TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  deltas.clear();
}

bool SecEdgarQ1TxnBuffer::RequiresRebuild() const { return requires_rebuild; }

const std::vector<SecEdgarQ1TxnBuffer::PreDelta> &
SecEdgarQ1TxnBuffer::Deltas() const {
  return deltas;
}

void SecEdgarQ1TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  deltas.clear();
}

void SecEdgarQ1TxnBuffer::AddDelta(PreDelta delta) {
  pending = true;
  if (delta.relevant) {
    deltas.push_back(std::move(delta));
  }
}

core::AcceleratorId SecEdgarQ1Module::Id() const { return kId; }

std::vector<core::TableDependency> SecEdgarQ1Module::Dependencies() const {
  return {{config.pre_table, {0, 1, 2, 3}}};
}

std::unique_ptr<core::AuxTxnBuffer> SecEdgarQ1Module::CreateTxnBuffer() const {
  return std::make_unique<SecEdgarQ1TxnBuffer>();
}

void SecEdgarQ1Module::Build(core::AuxEngineServices &services,
                             core::StateBuilder &builder) {
  EmitStateImages(BuildState(services), builder);
}

core::InvalidationPlan
SecEdgarQ1Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table != config.pre_table) {
    return {};
  }
  if (mutation.kind == core::MutationKind::Update &&
      !mutation.updated_columns.empty()) {
    bool relevant = false;
    for (const auto column : mutation.updated_columns) {
      if (column <= 3) {
        relevant = true;
        break;
      }
    }
    if (!relevant) {
      return {};
    }
    for (const auto column : mutation.updated_columns) {
      if (column == 0 || column == 1) {
        return AllPartitions();
      }
    }
  }
  return AggregatePartitions();
}

void SecEdgarQ1Module::AccumulateMutation(
    const core::MutationBatch &mutation, core::AuxEngineServices &,
    core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<SecEdgarQ1TxnBuffer &>(base_buffer);
  if (mutation.table != config.pre_table) {
    return;
  }
  auto add = [&](const core::AuxRow &row, int8_t sign) {
    PreRow parsed;
    if (!ReadPreRow(row, parsed)) {
      buffer.MarkRebuild();
      return;
    }
    buffer.AddDelta({std::move(parsed.group), std::move(parsed.adsh),
                     parsed.line, sign, parsed.relevant});
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
SecEdgarQ1Module::StageMaintenance(core::AuxEngineServices &services,
                                   const core::InvalidationPlan &,
                                   core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<SecEdgarQ1TxnBuffer &>(base_buffer);
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
      visibility, {core::SecEdgarQ1Layout::kGroupKeys,
                   core::SecEdgarQ1Layout::kGroupAggregates});
  SecEdgarQ1State state;
  core::Version aggregate_version = 0;
  bool have_keys = false;
  bool have_aggregates = false;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == core::SecEdgarQ1Layout::kGroupKeys) {
      const auto *typed = AsQ1Image(image, SecEdgarQ1ImageKind::GroupKeys);
      if (typed) {
        state.group_keys = typed->state->group_keys;
        have_keys = true;
      }
    } else if (image.partition == core::SecEdgarQ1Layout::kGroupAggregates) {
      const auto *typed =
          AsQ1Image(image, SecEdgarQ1ImageKind::GroupAggregates);
      if (typed) {
        state.aggregates = typed->state->aggregates;
        have_aggregates = true;
      }
      aggregate_version = image.version;
    }
  }
  if (!have_keys || !have_aggregates ||
      state.aggregates.size() != state.group_keys.size()) {
    outcome.action = core::MaintenanceAction::RebuildAll;
    outcome.invalidations = AllPartitions();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  for (const auto &delta : buffer.Deltas()) {
    const auto group = FindGroup(state.group_keys, delta.group);
    if (group == std::numeric_limits<uint32_t>::max()) {
      outcome.action = core::MaintenanceAction::RebuildAll;
      outcome.invalidations = AllPartitions();
      outcome.pending_rebuild = outcome.invalidations;
      return outcome;
    }
    ApplyRow(state.aggregates[group], delta.line, delta.adsh, delta.sign);
  }

  outcome.action = core::MaintenanceAction::ApplyDelta;
  outcome.expected_version = 0;
  const auto typed_state = std::make_shared<const SecEdgarQ1State>(state);
  outcome.updates.push_back(core::StateImage::Make(
      core::SecEdgarQ1Layout::kAccelerator,
      core::SecEdgarQ1Layout::kGroupAggregates, aggregate_version,
      MakeStateImage(SecEdgarQ1ImageKind::GroupAggregates, typed_state)));
  return outcome;
}

bool SecEdgarQ1Module::CheckValidity(core::AuxEngineServices &services,
                                     const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void SecEdgarQ1Module::Execute(core::AuxEngineServices &services,
                               const core::QueryArguments &arguments,
                               core::AuxRowSink &sink) {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {core::SecEdgarQ1Layout::kGroupKeys,
                                     core::SecEdgarQ1Layout::kGroupAggregates});
  SecEdgarQ1State state;
  bool have_keys = false;
  bool have_aggregates = false;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == core::SecEdgarQ1Layout::kGroupKeys) {
      const auto *typed = AsQ1Image(image, SecEdgarQ1ImageKind::GroupKeys);
      if (typed) {
        state.group_keys = typed->state->group_keys;
        have_keys = true;
      }
    } else if (image.partition == core::SecEdgarQ1Layout::kGroupAggregates) {
      const auto *typed =
          AsQ1Image(image, SecEdgarQ1ImageKind::GroupAggregates);
      if (typed) {
        state.aggregates = typed->state->aggregates;
        have_aggregates = true;
      }
    }
  }
  if (!snapshot.available || !have_keys || !have_aggregates ||
      state.group_keys.size() != state.aggregates.size()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  EmitOutput(state, sink);
}

void SecEdgarQ1Module::ExecuteFallback(core::AuxEngineServices &services,
                                       const core::QueryArguments &,
                                       core::AuxRowSink &sink) {
  EmitOutput(BuildState(services), sink);
}

SecEdgarQ1State
SecEdgarQ1Module::BuildState(core::AuxEngineServices &services) const {
  (void)services;
  SecEdgarQ1State state;
  if (!initial_scan) {
    throw std::logic_error(
        "SEC EDGAR Q1 requires a typed initial scan source");
  }
  std::set<SecEdgarQ1GroupKey> groups;
  initial_scan->ScanPre([&](const SecEdgarQ1PreInput &input) {
    if (input.stmt_is_null) {
      return;
    }
    groups.insert({input.stmt, input.rfile, input.rfile_is_null});
  });
  state.group_keys.assign(groups.begin(), groups.end());
  state.aggregates.resize(state.group_keys.size());
  initial_scan->ScanPre([&](const SecEdgarQ1PreInput &input) {
    if (input.stmt_is_null) {
      return;
    }
    const auto group =
        FindGroup(state.group_keys, {input.stmt, input.rfile, input.rfile_is_null});
    if (group != std::numeric_limits<uint32_t>::max()) {
      ApplyRow(state.aggregates[group], input.line, input.adsh, 1);
    }
  });
  return state;
}

bool SecEdgarQ1Module::LoadState(core::AuxEngineServices &services,
                                 SecEdgarQ1State &state) const {
  const auto snapshot =
      services.StateStore().Read(services.CurrentVisibility());
  bool keys = false;
  bool aggregates = false;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == core::SecEdgarQ1Layout::kGroupKeys) {
      const auto *typed = AsQ1Image(image, SecEdgarQ1ImageKind::GroupKeys);
      if (typed) {
        state.group_keys = typed->state->group_keys;
        keys = true;
      }
    } else if (image.partition == core::SecEdgarQ1Layout::kGroupAggregates) {
      const auto *typed =
          AsQ1Image(image, SecEdgarQ1ImageKind::GroupAggregates);
      if (typed) {
        state.aggregates = typed->state->aggregates;
        aggregates = true;
      }
    }
  }
  return keys && aggregates &&
         state.group_keys.size() == state.aggregates.size();
}

void SecEdgarQ1Module::EmitOutput(const SecEdgarQ1State &state,
                                  core::AuxRowSink &sink) const {
  sink.Begin({{{"stmt", core::AuxType::String, false},
               {"rfile", core::AuxType::String, true},
               {"cnt", core::AuxType::Int64, false},
               {"num_filings", core::AuxType::Int64, false},
               {"avg_line_num", core::AuxType::Double, false}}});
  for (const auto &row : BuildSecEdgarQ1Output(state)) {
    sink.WriteRow(
        {{core::AuxValue::Of(core::AuxType::String, row.stmt),
          row.rfile_is_null
              ? core::AuxValue::Null(core::AuxType::String)
              : core::AuxValue::Of(core::AuxType::String, row.rfile),
          core::AuxValue::Of(core::AuxType::Int64, row.cnt),
          core::AuxValue::Of(core::AuxType::Int64, row.num_filings),
          core::AuxValue::Of(core::AuxType::Double, row.avg_line_num)}});
  }
  sink.End();
}

} // namespace accelerator_codegen::aux_state::query
