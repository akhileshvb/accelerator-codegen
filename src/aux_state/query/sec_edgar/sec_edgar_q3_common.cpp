#include "accelerator_codegen/aux_state/query/sec_edgar/sec_edgar_q3_common.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {

constexpr uint32_t kFormatVersion = 1;
constexpr uint32_t kCohortMagic = 0x43303351; // Q3C
constexpr uint32_t kTotalsMagic = 0x54303351;  // Q3T
constexpr int32_t kTargetFy = 2022;
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

  void I32(int32_t value) { U32(static_cast<uint32_t>(value)); }

  void I64(int64_t value) { U64(static_cast<uint64_t>(value)); }

  void Double(double value) {
    uint64_t bits = 0;
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

  bool I32(int32_t &value) {
    uint32_t bits = 0;
    if (!U32(bits)) {
      return false;
    }
    value = static_cast<int32_t>(bits);
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
  std::string uom;
  double value = 0.0;
  bool value_is_null = true;
};

bool ReadNumRow(const core::AuxRow &row, NumRow &result) {
  return ReadString(row, 0, result.adsh) && ReadString(row, 5, result.uom) &&
         ReadDouble(row, 7, result.value, result.value_is_null);
}

struct SubRow {
  std::string adsh;
  std::string name;
  int32_t cik = 0;
  int32_t fy = 0;
};

bool ReadSubRow(const core::AuxRow &row, SubRow &result) {
  return ReadString(row, 0, result.adsh) && ReadString(row, 1, result.name) &&
         ReadInt32(row, 2, result.cik) && ReadInt32(row, 3, result.fy);
}

bool IsRelevantNum(const NumRow &row) {
  return row.uom == "USD" && !row.value_is_null;
}

uint32_t FindOrCreateGroup(SecEdgarQ3State &state, const std::string &name,
                           int32_t cik) {
  for (uint32_t group_id = 0; group_id < state.group_names.size(); ++group_id) {
    if (state.group_names[group_id] == name && state.group_ciks[group_id] == cik) {
      return group_id;
    }
  }
  const uint32_t group_id = static_cast<uint32_t>(state.group_names.size());
  state.group_names.push_back(name);
  state.group_ciks.push_back(cik);
  state.group_sums.push_back(0.0);
  return group_id;
}

bool ApplyUsdNumDelta(SecEdgarQ3State &state, const std::string &adsh,
                      double value, int8_t sign) {
  const auto filing_it = state.adsh_to_filing.find(adsh);
  if (filing_it == state.adsh_to_filing.end()) {
    return true;
  }
  const auto &filing = filing_it->second;
  if (filing.group_id >= state.group_sums.size()) {
    return false;
  }
  const auto delta = static_cast<double>(sign) * value;
  state.cik_totals[filing.cik] += delta;
  state.group_sums[filing.group_id] += delta;
  return true;
}

std::shared_ptr<const SecEdgarQ3StateImage>
MakeStateImage(SecEdgarQ3ImageKind kind,
               const std::shared_ptr<const SecEdgarQ3State> &state) {
  auto image = std::make_shared<SecEdgarQ3StateImage>();
  image->kind = kind;
  image->state = state;
  return image;
}

const SecEdgarQ3StateImage *AsQ3Image(const core::StateImage &image,
                                      SecEdgarQ3ImageKind kind) {
  const auto *payload = image.As<SecEdgarQ3StateImage>();
  return payload && payload->kind == kind && payload->state ? payload
                                                              : nullptr;
}

void EmitStateImages(const SecEdgarQ3State &state, core::StateBuilder &builder) {
  const auto typed_state = std::make_shared<const SecEdgarQ3State>(state);
  builder.Write(core::StateImage::Make(
      core::SecEdgarQ3Layout::kAccelerator,
      core::SecEdgarQ3Layout::kFy2022Cohort, 0,
      MakeStateImage(SecEdgarQ3ImageKind::Fy2022Cohort, typed_state)));
  builder.Write(core::StateImage::Make(
      core::SecEdgarQ3Layout::kAccelerator, core::SecEdgarQ3Layout::kCikTotals,
      0, MakeStateImage(SecEdgarQ3ImageKind::CikTotals, typed_state)));
  builder.Finish();
}

core::InvalidationPlan AllPartitions() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

core::InvalidationPlan TotalsPartition() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
  plan.partitions.insert(
      static_cast<::accelerator_codegen::aux_state::PartitionKey>(
          core::SecEdgarQ3Layout::kCikTotals));
  return plan;
}

} // namespace

std::size_t SecEdgarQ3NameCikKeyHash::operator()(
    const SecEdgarQ3NameCikKey &key) const noexcept {
  return std::hash<std::string>{}(key.name) ^
         (static_cast<std::size_t>(key.cik) << 1);
}

std::vector<uint8_t> EncodeSecEdgarQ3Fy2022Cohort(const SecEdgarQ3State &state) {
  std::vector<std::string> adshs;
  adshs.reserve(state.adsh_to_filing.size());
  for (const auto &[adsh, _] : state.adsh_to_filing) {
    adshs.push_back(adsh);
  }
  std::sort(adshs.begin(), adshs.end());
  Writer writer;
  Begin(writer, kCohortMagic, static_cast<uint32_t>(adshs.size()));
  for (const auto &adsh : adshs) {
    const auto &filing = state.adsh_to_filing.at(adsh);
    writer.String(adsh);
    writer.String(filing.name);
    writer.I32(filing.cik);
    writer.U32(filing.group_id);
  }
  return std::move(writer.bytes);
}

std::vector<uint8_t> EncodeSecEdgarQ3CikTotals(const SecEdgarQ3State &state) {
  std::vector<int32_t> ciks;
  ciks.reserve(state.cik_totals.size());
  for (const auto &[cik, _] : state.cik_totals) {
    ciks.push_back(cik);
  }
  std::sort(ciks.begin(), ciks.end());
  Writer writer;
  Begin(writer, kTotalsMagic, static_cast<uint32_t>(ciks.size()));
  writer.U32(static_cast<uint32_t>(state.group_names.size()));
  for (const auto &name : state.group_names) {
    writer.String(name);
  }
  for (const auto cik : state.group_ciks) {
    writer.I32(cik);
  }
  for (const auto sum : state.group_sums) {
    writer.Double(sum);
  }
  for (const auto cik : ciks) {
    writer.I32(cik);
    writer.Double(state.cik_totals.at(cik));
  }
  return std::move(writer.bytes);
}

bool DecodeSecEdgarQ3Fy2022Cohort(const std::vector<uint8_t> &bytes,
                                    SecEdgarQ3State &state) {
  Reader reader{bytes};
  uint32_t count = 0;
  if (!Begin(reader, kCohortMagic, count)) {
    return false;
  }
  state.adsh_to_filing.clear();
  for (uint32_t i = 0; i < count; ++i) {
    std::string adsh;
    SecEdgarQ3FilingInfo filing;
    uint32_t group_id = 0;
    if (!reader.String(adsh) || !reader.String(filing.name) ||
        !reader.I32(filing.cik) || !reader.U32(group_id)) {
      return false;
    }
    filing.group_id = group_id;
    state.adsh_to_filing.emplace(std::move(adsh), std::move(filing));
  }
  return reader.offset == bytes.size();
}

bool DecodeSecEdgarQ3CikTotals(const std::vector<uint8_t> &bytes,
                               SecEdgarQ3State &state) {
  Reader reader{bytes};
  uint32_t cik_count = 0;
  if (!Begin(reader, kTotalsMagic, cik_count)) {
    return false;
  }
  uint32_t group_count = 0;
  if (!reader.U32(group_count)) {
    return false;
  }
  state.group_names.clear();
  state.group_ciks.clear();
  state.group_sums.clear();
  state.group_names.reserve(group_count);
  state.group_ciks.reserve(group_count);
  state.group_sums.reserve(group_count);
  for (uint32_t i = 0; i < group_count; ++i) {
    std::string name;
    if (!reader.String(name)) {
      return false;
    }
    state.group_names.push_back(std::move(name));
  }
  for (uint32_t i = 0; i < group_count; ++i) {
    int32_t cik = 0;
    if (!reader.I32(cik)) {
      return false;
    }
    state.group_ciks.push_back(cik);
  }
  for (uint32_t i = 0; i < group_count; ++i) {
    double sum = 0.0;
    if (!reader.Double(sum)) {
      return false;
    }
    state.group_sums.push_back(sum);
  }
  state.cik_totals.clear();
  for (uint32_t i = 0; i < cik_count; ++i) {
    int32_t cik = 0;
    double total = 0.0;
    if (!reader.I32(cik) || !reader.Double(total)) {
      return false;
    }
    state.cik_totals.emplace(cik, total);
  }
  return reader.offset == bytes.size();
}

std::vector<SecEdgarQ3OutputRow>
BuildSecEdgarQ3Output(const SecEdgarQ3State &state) {
  double cik_sum_total = 0.0;
  for (const auto &[_, total] : state.cik_totals) {
    cik_sum_total += total;
  }
  const double avg_threshold =
      state.cik_totals.empty()
          ? 0.0
          : cik_sum_total / static_cast<double>(state.cik_totals.size());

  std::vector<SecEdgarQ3OutputRow> rows;
  rows.reserve(state.group_names.size());
  for (uint32_t group_id = 0; group_id < state.group_names.size(); ++group_id) {
    const auto total = state.group_sums[group_id];
    if (total <= avg_threshold) {
      continue;
    }
    rows.push_back({state.group_names[group_id], state.group_ciks[group_id],
                    total});
  }
  std::sort(rows.begin(), rows.end(),
            [](const SecEdgarQ3OutputRow &lhs, const SecEdgarQ3OutputRow &rhs) {
              if (lhs.total_value != rhs.total_value) {
                return lhs.total_value > rhs.total_value;
              }
              if (lhs.name != rhs.name) {
                return lhs.name < rhs.name;
              }
              return lhs.cik < rhs.cik;
            });
  if (rows.size() > kOutputLimit) {
    rows.resize(kOutputLimit);
  }
  return rows;
}

bool SecEdgarQ3TxnBuffer::HasPendingChanges() const { return pending; }

void SecEdgarQ3TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  deltas.clear();
}

bool SecEdgarQ3TxnBuffer::RequiresRebuild() const { return requires_rebuild; }

const std::vector<SecEdgarQ3TxnBuffer::NumDelta> &
SecEdgarQ3TxnBuffer::Deltas() const {
  return deltas;
}

void SecEdgarQ3TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  deltas.clear();
}

void SecEdgarQ3TxnBuffer::AddDelta(NumDelta delta) {
  pending = true;
  if (delta.relevant) {
    deltas.push_back(std::move(delta));
  }
}

core::AcceleratorId SecEdgarQ3Module::Id() const { return kId; }

std::vector<core::TableDependency> SecEdgarQ3Module::Dependencies() const {
  return {{config.num_table, {0, 1, 2}},
          {config.sub_table, {0, 1, 2, 3}}};
}

std::unique_ptr<core::AuxTxnBuffer> SecEdgarQ3Module::CreateTxnBuffer() const {
  return std::make_unique<SecEdgarQ3TxnBuffer>();
}

void SecEdgarQ3Module::Build(core::AuxEngineServices &services,
                             core::StateBuilder &builder) {
  EmitStateImages(BuildState(services), builder);
}

core::InvalidationPlan
SecEdgarQ3Module::InterpretMutation(const core::MutationBatch &mutation) const {
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
  return TotalsPartition();
}

void SecEdgarQ3Module::AccumulateMutation(const core::MutationBatch &mutation,
                                          core::AuxEngineServices &,
                                          core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<SecEdgarQ3TxnBuffer &>(base_buffer);
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
    buffer.AddDelta({parsed.adsh, parsed.value, sign, IsRelevantNum(parsed)});
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
SecEdgarQ3Module::StageMaintenance(core::AuxEngineServices &services,
                                   const core::InvalidationPlan &,
                                   core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<SecEdgarQ3TxnBuffer &>(base_buffer);
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
      visibility, {core::SecEdgarQ3Layout::kFy2022Cohort,
                   core::SecEdgarQ3Layout::kCikTotals});
  SecEdgarQ3State state;
  core::Version totals_version = 0;
  bool have_cohort = false;
  bool have_totals = false;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == core::SecEdgarQ3Layout::kFy2022Cohort) {
      const auto *typed = AsQ3Image(image, SecEdgarQ3ImageKind::Fy2022Cohort);
      if (typed) {
        state.adsh_to_filing = typed->state->adsh_to_filing;
        have_cohort = true;
      }
    } else if (image.partition == core::SecEdgarQ3Layout::kCikTotals) {
      const auto *typed = AsQ3Image(image, SecEdgarQ3ImageKind::CikTotals);
      if (typed) {
        state.cik_totals = typed->state->cik_totals;
        state.group_names = typed->state->group_names;
        state.group_ciks = typed->state->group_ciks;
        state.group_sums = typed->state->group_sums;
        have_totals = true;
      }
      totals_version = image.version;
    }
  }
  if (!have_cohort || !have_totals ||
      state.group_names.size() != state.group_sums.size()) {
    outcome.action = core::MaintenanceAction::RebuildAll;
    outcome.invalidations = AllPartitions();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }
  for (const auto &delta : buffer.Deltas()) {
    if (!ApplyUsdNumDelta(state, delta.adsh, delta.value, delta.sign)) {
      outcome.action = core::MaintenanceAction::RebuildAll;
      outcome.invalidations = AllPartitions();
      outcome.pending_rebuild = outcome.invalidations;
      return outcome;
    }
  }
  outcome.action = core::MaintenanceAction::ApplyDelta;
  const auto typed_state = std::make_shared<const SecEdgarQ3State>(state);
  outcome.updates.push_back(core::StateImage::Make(
      core::SecEdgarQ3Layout::kAccelerator, core::SecEdgarQ3Layout::kCikTotals,
      totals_version,
      MakeStateImage(SecEdgarQ3ImageKind::CikTotals, typed_state)));
  outcome.expected_version = 0;
  return outcome;
}

bool SecEdgarQ3Module::CheckValidity(core::AuxEngineServices &services,
                                     const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void SecEdgarQ3Module::Execute(core::AuxEngineServices &services,
                               const core::QueryArguments &arguments,
                               core::AuxRowSink &sink) {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(),
      {core::SecEdgarQ3Layout::kFy2022Cohort, core::SecEdgarQ3Layout::kCikTotals});
  SecEdgarQ3State state;
  bool have_cohort = false;
  bool have_totals = false;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == core::SecEdgarQ3Layout::kFy2022Cohort) {
      const auto *typed = AsQ3Image(image, SecEdgarQ3ImageKind::Fy2022Cohort);
      if (typed) {
        state.adsh_to_filing = typed->state->adsh_to_filing;
        have_cohort = true;
      }
    } else if (image.partition == core::SecEdgarQ3Layout::kCikTotals) {
      const auto *typed = AsQ3Image(image, SecEdgarQ3ImageKind::CikTotals);
      if (typed) {
        state.cik_totals = typed->state->cik_totals;
        state.group_names = typed->state->group_names;
        state.group_ciks = typed->state->group_ciks;
        state.group_sums = typed->state->group_sums;
        have_totals = true;
      }
    }
  }
  if (!snapshot.available || !have_cohort || !have_totals ||
      state.group_names.size() != state.group_sums.size()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  EmitOutput(state, sink);
}

void SecEdgarQ3Module::ExecuteFallback(core::AuxEngineServices &services,
                                       const core::QueryArguments &,
                                       core::AuxRowSink &sink) {
  EmitOutput(BuildState(services), sink);
}

SecEdgarQ3State SecEdgarQ3Module::BuildState(core::AuxEngineServices &services) const {
  (void)services;
  SecEdgarQ3State state;
  if (!initial_scan) {
    throw std::logic_error(
        "SEC EDGAR Q3 requires a typed initial scan source");
  }
  initial_scan->ScanSub([&](const SecEdgarQ3SubInput &input) {
    if (input.fy != kTargetFy) {
      return;
    }
    const auto group_id = FindOrCreateGroup(state, input.name, input.cik);
    state.adsh_to_filing.emplace(
        input.adsh, SecEdgarQ3FilingInfo{input.name, input.cik, group_id});
  });
  initial_scan->ScanNum([&](const SecEdgarQ3NumInput &input) {
    if (input.uom != "USD" || input.value_is_null) {
      return;
    }
    ApplyUsdNumDelta(state, input.adsh, input.value, 1);
  });
  return state;
}

void SecEdgarQ3Module::EmitOutput(const SecEdgarQ3State &state,
                                  core::AuxRowSink &sink) const {
  sink.Begin({{{"name", core::AuxType::String, false},
               {"cik", core::AuxType::Int64, false},
               {"total_value", core::AuxType::Double, false}}});
  for (const auto &row : BuildSecEdgarQ3Output(state)) {
    sink.WriteRow({{core::AuxValue::Of(core::AuxType::String, row.name),
                    core::AuxValue::Of(core::AuxType::Int64,
                                       static_cast<int64_t>(row.cik)),
                    core::AuxValue::Of(core::AuxType::Double, row.total_value)}});
  }
  sink.End();
}

} // namespace accelerator_codegen::aux_state::query
