#include "accelerator_codegen/aux_state/query/sec_edgar/sec_edgar_q24_common.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>


namespace accelerator_codegen::aux_state::query {
namespace {

constexpr uint32_t kFormatVersion = 1;
constexpr uint32_t kPreMagic = 0x50323451;        // Q24P
constexpr uint32_t kTagVersionMagic = 0x54323451; // Q24T
constexpr uint32_t kAggregateMagic = 0x41323451;  // Q24A
constexpr int32_t kFirstDate = 20230101;
constexpr int32_t kLastDate = 20231231;
constexpr int64_t kMinimumCount = 11;
constexpr std::size_t kOutputLimit = 100;

struct Writer {
  std::vector<uint8_t> bytes;

  void U32(uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
      bytes.push_back(static_cast<uint8_t>((value >> shift) & 0xff));
    }
  }

  void U64(uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
      bytes.push_back(static_cast<uint8_t>((value >> shift) & 0xff));
    }
  }

  void I64(int64_t value) { U64(static_cast<uint64_t>(value)); }

  void Double(double value) {
    uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    U64(bits);
  }

  void String(const std::string &value) {
    U32(static_cast<uint32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
  }
};

struct Reader {
  const std::vector<uint8_t> &bytes;
  std::size_t offset = 0;

  bool CanRead(std::size_t count) const {
    return count <= bytes.size() - std::min(offset, bytes.size());
  }

  bool U32(uint32_t &value) {
    if (!CanRead(4)) {
      return false;
    }
    value = static_cast<uint32_t>(bytes[offset]) |
            (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
            (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
            (static_cast<uint32_t>(bytes[offset + 3]) << 24);
    offset += 4;
    return true;
  }

  bool U64(uint64_t &value) {
    if (!CanRead(8)) {
      return false;
    }
    value = 0;
    for (int shift = 0; shift < 64; shift += 8) {
      value |= static_cast<uint64_t>(bytes[offset++]) << shift;
    }
    return true;
  }

  bool I64(int64_t &value) {
    uint64_t bits = 0;
    if (!U64(bits)) {
      return false;
    }
    value = static_cast<int64_t>(bits);
    return true;
  }

  bool Double(double &value) {
    uint64_t bits = 0;
    if (!U64(bits)) {
      return false;
    }
    std::memcpy(&value, &bits, sizeof(value));
    return true;
  }

  bool String(std::string &value) {
    uint32_t length = 0;
    if (!U32(length) || !CanRead(length)) {
      return false;
    }
    value.assign(reinterpret_cast<const char *>(bytes.data() + offset), length);
    offset += length;
    return true;
  }
};

void Begin(Writer &writer, uint32_t magic, uint32_t count) {
  writer.U32(magic);
  writer.U32(kFormatVersion);
  writer.U32(count);
}

bool Begin(Reader &reader, uint32_t magic, uint32_t &count) {
  uint32_t actual_magic = 0;
  uint32_t version = 0;
  return reader.U32(actual_magic) && reader.U32(version) && reader.U32(count) &&
         actual_magic == magic && version == kFormatVersion;
}

bool ReadString(const core::AuxRow &row, std::size_t index,
                std::string &value) {
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
    for (int32_t i = 0; i < decimal.scale; ++i) {
      value /= 10.0;
    }
    return true;
  }
  return false;
}

bool ReadDate(const core::AuxRow &row, std::size_t index, int32_t &value) {
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
  if (std::holds_alternative<core::AuxDate>(cell.payload)) {
    value = std::get<core::AuxDate>(cell.payload).days_since_epoch;
    return true;
  }
  return false;
}

struct NumRow {
  SecEdgarQ24MembershipKey membership;
  SecEdgarQ24TagVersionKey tag_version;
  std::string uom;
  double value = 0.0;
  bool value_is_null = true;
  int32_t ddate = 0;
};

bool ReadNumRow(const core::AuxRow &row, NumRow &result) {
  if (!ReadString(row, 0, result.membership.adsh) ||
      !ReadString(row, 1, result.membership.tag) ||
      !ReadString(row, 2, result.membership.version) ||
      !ReadString(row, 5, result.uom) ||
      !ReadDouble(row, 7, result.value, result.value_is_null) ||
      !ReadDate(row, 3, result.ddate)) {
    return false;
  }
  result.tag_version.tag = result.membership.tag;
  result.tag_version.version = result.membership.version;
  return true;
}

bool ReadPreRow(const core::AuxRow &row, SecEdgarQ24MembershipKey &result) {
  return ReadString(row, 0, result.adsh) && ReadString(row, 1, result.tag) &&
         ReadString(row, 2, result.version);
}

bool IsRelevant(const NumRow &row) {
  return row.uom == "USD" && !row.value_is_null && row.ddate >= kFirstDate &&
         row.ddate <= kLastDate;
}

uint32_t FindGroup(const std::vector<SecEdgarQ24TagVersionKey> &groups,
                   const SecEdgarQ24TagVersionKey &key) {
  const auto it = std::lower_bound(groups.begin(), groups.end(), key);
  if (it == groups.end() || *it == key) {
    return it == groups.end() ? std::numeric_limits<uint32_t>::max()
                              : static_cast<uint32_t>(it - groups.begin());
  }
  return std::numeric_limits<uint32_t>::max();
}

std::shared_ptr<const SecEdgarQ24StateImage>
MakeStateImage(SecEdgarQ24ImageKind kind,
               const std::shared_ptr<const SecEdgarQ24State> &state) {
  auto image = std::make_shared<SecEdgarQ24StateImage>();
  image->kind = kind;
  image->state = state;
  return image;
}

const SecEdgarQ24StateImage *AsQ24Image(const core::StateImage &image,
                                        SecEdgarQ24ImageKind kind) {
  const auto *payload = image.As<SecEdgarQ24StateImage>();
  return payload && payload->kind == kind && payload->state ? payload
                                                              : nullptr;
}

void EmitStateImages(const SecEdgarQ24State &state,
                     core::StateBuilder &builder) {
  const auto typed_state = std::make_shared<const SecEdgarQ24State>(state);
  builder.Write(core::StateImage::Make(
      core::SecEdgarQ24Layout::kAccelerator,
      core::SecEdgarQ24Layout::kPreMembership, 0,
      MakeStateImage(SecEdgarQ24ImageKind::PreMembership, typed_state)));
  builder.Write(core::StateImage::Make(
      core::SecEdgarQ24Layout::kAccelerator,
      core::SecEdgarQ24Layout::kTagVersions, 0,
      MakeStateImage(SecEdgarQ24ImageKind::TagVersions, typed_state)));
  builder.Write(core::StateImage::Make(
      core::SecEdgarQ24Layout::kAccelerator,
      core::SecEdgarQ24Layout::kGroupAggregates, 0,
      MakeStateImage(SecEdgarQ24ImageKind::Aggregates, typed_state)));
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
          core::SecEdgarQ24Layout::kGroupAggregates));
  return plan;
}

} // namespace

std::size_t SecEdgarQ24MembershipKeyHash::operator()(
    const SecEdgarQ24MembershipKey &key) const noexcept {
  return std::hash<std::string>{}(key.adsh) ^
         (std::hash<std::string>{}(key.tag) << 1) ^
         (std::hash<std::string>{}(key.version) << 2);
}

std::size_t SecEdgarQ24TagVersionKeyHash::operator()(
    const SecEdgarQ24TagVersionKey &key) const noexcept {
  return std::hash<std::string>{}(key.tag) ^
         (std::hash<std::string>{}(key.version) << 1);
}

std::vector<uint8_t>
EncodeSecEdgarQ24PreMembership(const SecEdgarQ24State &state) {
  std::vector<SecEdgarQ24MembershipKey> keys(state.pre_membership.begin(),
                                             state.pre_membership.end());
  std::sort(keys.begin(), keys.end(), [](const auto &lhs, const auto &rhs) {
    return lhs.adsh < rhs.adsh ||
           (lhs.adsh == rhs.adsh &&
            (lhs.tag < rhs.tag ||
             (lhs.tag == rhs.tag && lhs.version < rhs.version)));
  });
  Writer writer;
  Begin(writer, kPreMagic, static_cast<uint32_t>(keys.size()));
  for (const auto &key : keys) {
    writer.String(key.adsh);
    writer.String(key.tag);
    writer.String(key.version);
  }
  return std::move(writer.bytes);
}

std::vector<uint8_t>
EncodeSecEdgarQ24TagVersions(const SecEdgarQ24State &state) {
  Writer writer;
  Begin(writer, kTagVersionMagic,
        static_cast<uint32_t>(state.tag_versions.size()));
  for (const auto &key : state.tag_versions) {
    writer.String(key.tag);
    writer.String(key.version);
  }
  return std::move(writer.bytes);
}

std::vector<uint8_t>
EncodeSecEdgarQ24Aggregates(const SecEdgarQ24State &state) {
  Writer writer;
  Begin(writer, kAggregateMagic,
        static_cast<uint32_t>(state.aggregates.size()));
  for (const auto &aggregate : state.aggregates) {
    writer.I64(aggregate.count);
    writer.Double(aggregate.total);
  }
  return std::move(writer.bytes);
}

bool DecodeSecEdgarQ24PreMembership(const std::vector<uint8_t> &bytes,
                                    SecEdgarQ24State &state) {
  Reader reader{bytes};
  uint32_t count = 0;
  if (!Begin(reader, kPreMagic, count)) {
    return false;
  }
  state.pre_membership.clear();
  for (uint32_t i = 0; i < count; ++i) {
    SecEdgarQ24MembershipKey key;
    if (!reader.String(key.adsh) || !reader.String(key.tag) ||
        !reader.String(key.version)) {
      return false;
    }
    state.pre_membership.insert(std::move(key));
  }
  return reader.offset == bytes.size();
}

bool DecodeSecEdgarQ24TagVersions(const std::vector<uint8_t> &bytes,
                                  SecEdgarQ24State &state) {
  Reader reader{bytes};
  uint32_t count = 0;
  if (!Begin(reader, kTagVersionMagic, count)) {
    return false;
  }
  state.tag_versions.clear();
  state.tag_versions.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    SecEdgarQ24TagVersionKey key;
    if (!reader.String(key.tag) || !reader.String(key.version)) {
      return false;
    }
    state.tag_versions.push_back(std::move(key));
  }
  return reader.offset == bytes.size() &&
         std::is_sorted(state.tag_versions.begin(), state.tag_versions.end());
}

bool DecodeSecEdgarQ24Aggregates(const std::vector<uint8_t> &bytes,
                                 SecEdgarQ24State &state) {
  Reader reader{bytes};
  uint32_t count = 0;
  if (!Begin(reader, kAggregateMagic, count)) {
    return false;
  }
  state.aggregates.clear();
  state.aggregates.resize(count);
  for (auto &aggregate : state.aggregates) {
    if (!reader.I64(aggregate.count) || !reader.Double(aggregate.total)) {
      return false;
    }
  }
  return reader.offset == bytes.size();
}

std::vector<SecEdgarQ24OutputRow>
BuildSecEdgarQ24Output(const SecEdgarQ24State &state) {
  std::vector<uint32_t> candidates;
  for (uint32_t id = 0;
       id < state.aggregates.size() && id < state.tag_versions.size(); ++id) {
    if (state.aggregates[id].count >= kMinimumCount) {
      candidates.push_back(id);
    }
  }
  const auto compare = [&](uint32_t lhs, uint32_t rhs) {
    const auto &left = state.aggregates[lhs];
    const auto &right = state.aggregates[rhs];
    if (left.count != right.count) {
      return left.count > right.count;
    }
    return state.tag_versions[lhs] < state.tag_versions[rhs];
  };
  if (candidates.size() > kOutputLimit) {
    std::partial_sort(candidates.begin(),
                      candidates.begin() + static_cast<ptrdiff_t>(kOutputLimit),
                      candidates.end(), compare);
    candidates.resize(kOutputLimit);
  } else {
    std::sort(candidates.begin(), candidates.end(), compare);
  }
  std::vector<SecEdgarQ24OutputRow> output;
  output.reserve(candidates.size());
  for (const auto id : candidates) {
    const auto &key = state.tag_versions[id];
    const auto &aggregate = state.aggregates[id];
    output.push_back(
        {key.tag, key.version, aggregate.count, aggregate.total});
  }
  return output;
}

bool SecEdgarQ24TxnBuffer::HasPendingChanges() const { return pending; }

void SecEdgarQ24TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  deltas.clear();
}

bool SecEdgarQ24TxnBuffer::RequiresRebuild() const { return requires_rebuild; }

const std::vector<SecEdgarQ24TxnBuffer::NumDelta> &
SecEdgarQ24TxnBuffer::Deltas() const {
  return deltas;
}

void SecEdgarQ24TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  deltas.clear();
}

void SecEdgarQ24TxnBuffer::AddDelta(NumDelta delta) {
  pending = true;
  if (delta.relevant) {
    deltas.push_back(std::move(delta));
  }
}

core::AcceleratorId SecEdgarQ24Module::Id() const { return kId; }

std::vector<core::TableDependency> SecEdgarQ24Module::Dependencies() const {
  return {{config.num_table, {0, 1, 2, 3, 4, 5}},
          {config.pre_table, {0, 1, 2}}};
}

std::unique_ptr<core::AuxTxnBuffer> SecEdgarQ24Module::CreateTxnBuffer() const {
  return std::make_unique<SecEdgarQ24TxnBuffer>();
}

void SecEdgarQ24Module::Build(core::AuxEngineServices &services,
                              core::StateBuilder &builder) {
  EmitStateImages(BuildState(services), builder);
}

core::InvalidationPlan SecEdgarQ24Module::InterpretMutation(
    const core::MutationBatch &mutation) const {
  if (mutation.table == config.pre_table) {
    return AllPartitions();
  }
  if (mutation.table != config.num_table) {
    return {};
  }
  if (mutation.kind == core::MutationKind::Update &&
      !mutation.updated_columns.empty()) {
    bool relevant = false;
    for (const auto column : mutation.updated_columns) {
      if (column <= 5) {
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

void SecEdgarQ24Module::AccumulateMutation(
    const core::MutationBatch &mutation, core::AuxEngineServices &services,
    core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<SecEdgarQ24TxnBuffer &>(base_buffer);
  if (mutation.table == config.pre_table) {
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
    const bool relevant = IsRelevant(parsed);
    bool excluded_by_pre = false;
    if (relevant && initial_scan) {
      initial_scan->ScanPre([&](const SecEdgarQ24PreInput &pre_row) {
        if (pre_row.adsh == parsed.membership.adsh &&
            pre_row.tag == parsed.membership.tag &&
            pre_row.version == parsed.membership.version)
          excluded_by_pre = true;
      });
    }
    buffer.AddDelta({std::move(parsed.membership),
                     std::move(parsed.tag_version), parsed.value, sign,
                     relevant, excluded_by_pre});
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
SecEdgarQ24Module::StageMaintenance(core::AuxEngineServices &services,
                                    const core::InvalidationPlan &,
                                    core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<SecEdgarQ24TxnBuffer &>(base_buffer);
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
      visibility, {core::SecEdgarQ24Layout::kTagVersions,
                   core::SecEdgarQ24Layout::kGroupAggregates});
  SecEdgarQ24State state;
  core::Version aggregate_version = 0;
  bool have_tags = false;
  bool have_aggregates = false;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == core::SecEdgarQ24Layout::kTagVersions) {
      const auto *typed = AsQ24Image(image, SecEdgarQ24ImageKind::TagVersions);
      if (typed) {
        state.tag_versions = typed->state->tag_versions;
        have_tags = true;
      }
    } else if (image.partition == core::SecEdgarQ24Layout::kGroupAggregates) {
      const auto *typed = AsQ24Image(image, SecEdgarQ24ImageKind::Aggregates);
      if (typed) {
        state.aggregates = typed->state->aggregates;
        have_aggregates = true;
      }
      aggregate_version = image.version;
    }
  }
  if (!have_tags || !have_aggregates ||
      state.aggregates.size() != state.tag_versions.size()) {
    outcome.action = core::MaintenanceAction::RebuildAll;
    outcome.invalidations = AllPartitions();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }
  for (const auto &delta : buffer.Deltas()) {
    if (delta.excluded_by_pre) {
      continue;
    }
    const auto group = FindGroup(state.tag_versions, delta.tag_version);
    if (group == std::numeric_limits<uint32_t>::max()) {
      outcome.action = core::MaintenanceAction::RebuildAll;
      outcome.invalidations = AllPartitions();
      outcome.pending_rebuild = outcome.invalidations;
      return outcome;
    }
    state.aggregates[group].count += delta.sign;
    state.aggregates[group].total +=
        static_cast<double>(delta.sign) * delta.value;
  }
  outcome.action = core::MaintenanceAction::ApplyDelta;
  outcome.expected_version = 0;
  const auto typed_state = std::make_shared<const SecEdgarQ24State>(state);
  outcome.updates.push_back(core::StateImage::Make(
      core::SecEdgarQ24Layout::kAccelerator,
      core::SecEdgarQ24Layout::kGroupAggregates, aggregate_version,
      MakeStateImage(SecEdgarQ24ImageKind::Aggregates, typed_state)));
  return outcome;
}

bool SecEdgarQ24Module::CheckValidity(core::AuxEngineServices &services,
                                      const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void SecEdgarQ24Module::Execute(core::AuxEngineServices &services,
                                const core::QueryArguments &arguments,
                                core::AuxRowSink &sink) {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {core::SecEdgarQ24Layout::kTagVersions,
                                     core::SecEdgarQ24Layout::kGroupAggregates});
  SecEdgarQ24State state;
  bool have_tags = false;
  bool have_aggregates = false;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == core::SecEdgarQ24Layout::kTagVersions) {
      const auto *typed = AsQ24Image(image, SecEdgarQ24ImageKind::TagVersions);
      if (typed) {
        state.tag_versions = typed->state->tag_versions;
        have_tags = true;
      }
    } else if (image.partition == core::SecEdgarQ24Layout::kGroupAggregates) {
      const auto *typed = AsQ24Image(image, SecEdgarQ24ImageKind::Aggregates);
      if (typed) {
        state.aggregates = typed->state->aggregates;
        have_aggregates = true;
      }
    }
  }
  if (!snapshot.available || !have_tags || !have_aggregates ||
      state.tag_versions.size() != state.aggregates.size()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  EmitOutput(state, sink);
}

void SecEdgarQ24Module::ExecuteFallback(core::AuxEngineServices &services,
                                        const core::QueryArguments &,
                                        core::AuxRowSink &sink) {
  EmitOutput(BuildState(services), sink);
}

SecEdgarQ24State
SecEdgarQ24Module::BuildState(core::AuxEngineServices &services) const {
  (void)services;
  SecEdgarQ24State state;
  if (!initial_scan)
    throw std::logic_error(
        "SEC EDGAR Q24 requires a typed initial scan source");
  initial_scan->ScanPre([&](const SecEdgarQ24PreInput &input) {
    state.pre_membership.insert(
        {input.adsh, input.tag, input.version});
  });
  std::set<SecEdgarQ24TagVersionKey> groups;
  initial_scan->ScanNum([&](const SecEdgarQ24NumInput &input) {
    if (input.uom == "USD" && !input.value_is_null &&
        input.ddate >= kFirstDate && input.ddate <= kLastDate)
      groups.insert({input.tag, input.version});
  });
  state.tag_versions.assign(groups.begin(), groups.end());
  state.aggregates.resize(state.tag_versions.size());
  initial_scan->ScanNum([&](const SecEdgarQ24NumInput &input) {
    if (input.uom != "USD" || input.value_is_null || input.ddate < kFirstDate ||
        input.ddate > kLastDate)
      return;
    if (state.pre_membership.find(
            {input.adsh, input.tag, input.version}) != state.pre_membership.end())
      return;
    const auto group = FindGroup(state.tag_versions, {input.tag, input.version});
    if (group != std::numeric_limits<uint32_t>::max()) {
      state.aggregates[group].count++;
      state.aggregates[group].total += input.value;
    }
  });
  return state;
}

bool SecEdgarQ24Module::LoadState(core::AuxEngineServices &services,
                                  SecEdgarQ24State &state) const {
  const auto snapshot =
      services.StateStore().Read(services.CurrentVisibility());
  bool pre = false;
  bool tags = false;
  bool aggregates = false;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == core::SecEdgarQ24Layout::kPreMembership) {
      const auto *typed = AsQ24Image(image, SecEdgarQ24ImageKind::PreMembership);
      if (typed) {
        state.pre_membership = typed->state->pre_membership;
        pre = true;
      }
    } else if (image.partition == core::SecEdgarQ24Layout::kTagVersions) {
      const auto *typed = AsQ24Image(image, SecEdgarQ24ImageKind::TagVersions);
      if (typed) {
        state.tag_versions = typed->state->tag_versions;
        tags = true;
      }
    } else if (image.partition == core::SecEdgarQ24Layout::kGroupAggregates) {
      const auto *typed = AsQ24Image(image, SecEdgarQ24ImageKind::Aggregates);
      if (typed) {
        state.aggregates = typed->state->aggregates;
        aggregates = true;
      }
    }
  }
  return pre && tags && aggregates &&
         state.tag_versions.size() == state.aggregates.size();
}

void SecEdgarQ24Module::EmitOutput(const SecEdgarQ24State &state,
                                   core::AuxRowSink &sink) const {
  sink.Begin({{{"tag", core::AuxType::String, false},
               {"version", core::AuxType::String, false},
               {"cnt", core::AuxType::Int64, false},
               {"total", core::AuxType::Double, false}}});
  // The top-k rows are derived at read time from maintained aggregates. They
  // are intentionally not persisted as auxiliary state.
  for (const auto &row : BuildSecEdgarQ24Output(state)) {
    sink.WriteRow({{core::AuxValue::Of(core::AuxType::String, row.tag),
                    core::AuxValue::Of(core::AuxType::String, row.version),
                    core::AuxValue::Of(core::AuxType::Int64, row.count),
                    core::AuxValue::Of(core::AuxType::Double, row.total)}});
  }
  sink.End();
}

} // namespace accelerator_codegen::aux_state::query
