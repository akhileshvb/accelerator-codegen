#include "sec_edgar/q6/common/module.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <unordered_map>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>

namespace accelerator_codegen::aux_state::query {
namespace {

constexpr int32_t kTargetFy = 2023;
constexpr std::string_view kUsd = "USD";
constexpr std::string_view kIsStmt = "IS";
constexpr core::PartitionId kStatePartition = core::SecEdgarQ6Layout::kState;

using Cols = SecEdgarQ6Columns;

enum class Cell { Ok, Null, Bad };

Cell GetString(const core::AuxRow &row, std::size_t index,
               std::string_view &out) {
  if (index >= row.values.size()) {
    return Cell::Bad;
  }
  const auto &cell = row.values[index];
  if (cell.is_null) {
    return Cell::Null;
  }
  const auto *text = std::get_if<std::string>(&cell.payload);
  if (!text) {
    return Cell::Bad;
  }
  out = *text;
  return Cell::Ok;
}

Cell GetDouble(const core::AuxRow &row, std::size_t index, double &out) {
  if (index >= row.values.size()) {
    return Cell::Bad;
  }
  const auto &cell = row.values[index];
  if (cell.is_null) {
    return Cell::Null;
  }
  if (const auto *d = std::get_if<double>(&cell.payload)) {
    out = *d;
    return Cell::Ok;
  }
  if (const auto *i = std::get_if<int64_t>(&cell.payload)) {
    out = static_cast<double>(*i);
    return Cell::Ok;
  }
  if (const auto *u = std::get_if<uint64_t>(&cell.payload)) {
    out = static_cast<double>(*u);
    return Cell::Ok;
  }
  if (const auto *dec = std::get_if<core::ScaledDecimal>(&cell.payload)) {
    out = static_cast<double>(dec->value);
    for (int32_t scale = 0; scale < dec->scale; ++scale) {
      out /= 10.0;
    }
    return Cell::Ok;
  }
  return Cell::Bad;
}

Cell GetInt(const core::AuxRow &row, std::size_t index, int64_t &out) {
  if (index >= row.values.size()) {
    return Cell::Bad;
  }
  const auto &cell = row.values[index];
  if (cell.is_null) {
    return Cell::Null;
  }
  if (const auto *i = std::get_if<int64_t>(&cell.payload)) {
    out = *i;
    return Cell::Ok;
  }
  if (const auto *u = std::get_if<uint64_t>(&cell.payload)) {
    out = static_cast<int64_t>(*u);
    return Cell::Ok;
  }
  return Cell::Bad;
}

bool ColumnTouched(const core::MutationBatch &mutation,
                   std::initializer_list<core::ColumnId> relevant) {
  if (mutation.kind != core::MutationKind::Update ||
      mutation.updated_columns.empty()) {
    return true;
  }
  for (const auto column : mutation.updated_columns) {
    for (const auto r : relevant) {
      if (column == r) {
        return true;
      }
    }
  }
  return false;
}

std::shared_ptr<const SecEdgarQ6StateImage>
MakeImage(std::shared_ptr<const SecEdgarQ6State> state) {
  auto image = std::make_shared<SecEdgarQ6StateImage>();
  image->state = std::move(state);
  return image;
}

core::InvalidationPlan AllPartitions() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

core::InvalidationPlan StatePartition() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
  plan.partitions.insert(
      static_cast<::accelerator_codegen::aux_state::PartitionKey>(
          kStatePartition));
  return plan;
}

core::StagedMaintenanceOutcome RebuildOutcome() {
  core::StagedMaintenanceOutcome outcome;
  outcome.action = core::MaintenanceAction::RebuildAll;
  outcome.invalidations = AllPartitions();
  outcome.pending_rebuild = outcome.invalidations;
  return outcome;
}

} // namespace

// ---------------------------------------------------------------------------
// Txn buffer
// ---------------------------------------------------------------------------
void SecEdgarQ6TxnBuffer::Clear() {
  pending_ = false;
  requires_rebuild_ = false;
  arena_.clear();
  num_deltas_.clear();
  sub_changes_.clear();
  pre_changes_.clear();
}

void SecEdgarQ6TxnBuffer::AddNumDelta(std::string_view adsh, std::string_view tag,
                                      std::string_view version, double value,
                                      int8_t sign) {
  pending_ = true;
  NumDelta d;
  d.offset = static_cast<uint32_t>(arena_.size());
  d.adsh_len = static_cast<uint32_t>(adsh.size());
  d.tag_len = static_cast<uint32_t>(tag.size());
  d.version_len = static_cast<uint32_t>(version.size());
  d.value = value;
  d.sign = sign;
  arena_.insert(arena_.end(), adsh.begin(), adsh.end());
  arena_.insert(arena_.end(), tag.begin(), tag.end());
  arena_.insert(arena_.end(), version.begin(), version.end());
  num_deltas_.push_back(d);
}

// ---------------------------------------------------------------------------
// Module
// ---------------------------------------------------------------------------
core::AcceleratorId SecEdgarQ6Module::Id() const { return kId; }

std::vector<core::TableDependency> SecEdgarQ6Module::Dependencies() const {
  const auto attnums = [](const auto &a) {
    return std::vector<int32_t>(std::begin(a), std::end(a));
  };
  return {{config.num_table,
           {Cols::kNumAdsh, Cols::kNumTag, Cols::kNumVersion, Cols::kNumUom,
            Cols::kNumValue},
           attnums(Cols::kNumAttnums)},
          {config.sub_table,
           {Cols::kSubAdsh, Cols::kSubName, Cols::kSubFy},
           attnums(Cols::kSubAttnums)},
          {config.pre_table,
           {Cols::kPreAdsh, Cols::kPreStmt, Cols::kPreTag, Cols::kPreVersion,
            Cols::kPrePlabel},
           attnums(Cols::kPreAttnums)}};
}

std::unique_ptr<core::AuxTxnBuffer> SecEdgarQ6Module::CreateTxnBuffer() const {
  return std::make_unique<SecEdgarQ6TxnBuffer>();
}

std::shared_ptr<const SecEdgarQ6State> SecEdgarQ6Module::BuildState() const {
  if (!initial_scan) {
    throw std::logic_error("SEC EDGAR Q6 requires a typed initial scan source");
  }
  SecEdgarQ6StructureBuilder builder;
  initial_scan->ScanSub([&](const SecEdgarQ6SubInput &in) {
    if (!in.fy_is_null && in.fy == kTargetFy) {
      builder.AddCohort(in.adsh, in.name, in.name_is_null);
    }
  });
  initial_scan->ScanPre([&](const SecEdgarQ6PreInput &in) {
    if (in.stmt == kIsStmt) {
      builder.AddPre(in.adsh, in.tag, in.version, in.plabel, in.plabel_is_null);
    }
  });
  auto state = std::make_shared<SecEdgarQ6State>(
      SecEdgarQ6MakeEmptyState(builder.Finish()));
  const SecEdgarQ6Structure &structure = state->structure;

  // Fast path: the engine aggregates num x pre x sub itself (its own sums are
  // the base values, bit for bit).
  bool failed = false;
  const bool supported = initial_scan->ScanGroupAggregates(
      [&](const SecEdgarQ6GroupAggregateInput &g) {
        if (failed) {
          return;
        }
        uint32_t name_id = 0;
        if (!g.name_is_null) {
          name_id = structure.names.Find(g.name);
          if (name_id == 0) {
            failed = true;
            return;
          }
        }
        const uint32_t tag_id = structure.tags.Find(g.tag);
        uint32_t plabel_id = 0;
        if (!g.plabel_is_null) {
          plabel_id = structure.plabels.Find(g.plabel);
          if (plabel_id == 0) {
            failed = true;
            return;
          }
        }
        const uint32_t gid = structure.FindGroup(name_id, tag_id, plabel_id);
        if (tag_id == 0 || gid == SecEdgarQ6Structure::kNoPayload) {
          failed = true;
          return;
        }
        SecEdgarQ6SetGroup(*state, gid, g.sum, g.count);
      });
  if (!supported || failed) {
    // Generic path: stream num and resolve every contribution through the
    // bridge. Sums are accumulated as double-doubles (order independent).
    *state = SecEdgarQ6MakeEmptyState(std::move(state->structure));
    const SecEdgarQ6Structure &st = state->structure;
    std::vector<SecEdgarQ6Sum> sums(st.GroupCount());
    std::vector<int64_t> counts(st.GroupCount(), 0);
    std::vector<SecEdgarQ6GroupDelta> scratch;
    initial_scan->ScanNum([&](const SecEdgarQ6NumInput &in) {
      if (in.value_is_null || in.uom != kUsd) {
        return;
      }
      scratch.clear();
      SecEdgarQ6ExpandPayload(SecEdgarQ6Resolve(st, in.adsh, in.tag, in.version),
                              in.value, 1, scratch);
      for (const auto &d : scratch) {
        sums[d.gid] = SecEdgarQ6SumAdd(sums[d.gid], d.sum);
        counts[d.gid] += d.cnt;
      }
    });
    for (uint32_t g = 0; g < st.GroupCount(); ++g) {
      if (counts[g] != 0) {
        SecEdgarQ6SetGroup(*state, g, sums[g].hi, counts[g]);
      }
    }
  }
  SecEdgarQ6Rescan(*state);
  return state;
}

void SecEdgarQ6Module::Build(core::AuxEngineServices &,
                             core::StateBuilder &builder) {
  builder.Write(core::StateImage::Make(Id(), kStatePartition, 0,
                                       MakeImage(BuildState())));
  builder.Finish();
}

core::InvalidationPlan
SecEdgarQ6Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.num_table) {
    return ColumnTouched(mutation, {Cols::kNumAdsh, Cols::kNumTag,
                                    Cols::kNumVersion, Cols::kNumUom,
                                    Cols::kNumValue})
               ? StatePartition()
               : core::InvalidationPlan{};
  }
  if (mutation.table == config.sub_table) {
    return ColumnTouched(mutation,
                         {Cols::kSubAdsh, Cols::kSubName, Cols::kSubFy})
               ? AllPartitions()
               : core::InvalidationPlan{};
  }
  if (mutation.table == config.pre_table) {
    return ColumnTouched(mutation,
                         {Cols::kPreAdsh, Cols::kPreStmt, Cols::kPreTag,
                          Cols::kPreVersion, Cols::kPrePlabel})
               ? AllPartitions()
               : core::InvalidationPlan{};
  }
  return {};
}

void SecEdgarQ6Module::AccumulateMutation(const core::MutationBatch &mutation,
                                          core::AuxEngineServices &,
                                          core::AuxTxnBuffer &base_buffer) {
  auto &buffer = static_cast<SecEdgarQ6TxnBuffer &>(base_buffer);
  const bool is_num = mutation.table == config.num_table;
  const bool is_sub = mutation.table == config.sub_table;
  const bool is_pre = mutation.table == config.pre_table;
  if (!is_num && !is_sub && !is_pre) {
    return;
  }

  // Row images to process: (row, sign).
  std::vector<std::pair<const core::AuxRow *, int8_t>> rows;
  switch (mutation.kind) {
  case core::MutationKind::Insert:
    for (const auto &row : mutation.new_rows) {
      rows.emplace_back(&row, 1);
    }
    break;
  case core::MutationKind::Delete:
    for (const auto &row : mutation.old_rows) {
      rows.emplace_back(&row, -1);
    }
    break;
  case core::MutationKind::Update:
    if (mutation.old_rows.size() != mutation.new_rows.size() ||
        mutation.old_rows.empty()) {
      buffer.MarkRebuild();
      return;
    }
    if (is_num &&
        !ColumnTouched(mutation, {Cols::kNumAdsh, Cols::kNumTag, Cols::kNumVersion,
                                  Cols::kNumUom, Cols::kNumValue})) {
      return;
    }
    if (is_sub && !ColumnTouched(mutation, {Cols::kSubAdsh, Cols::kSubName,
                                            Cols::kSubFy})) {
      return;
    }
    if (is_pre && !ColumnTouched(mutation, {Cols::kPreAdsh, Cols::kPreStmt,
                                            Cols::kPreTag, Cols::kPreVersion,
                                            Cols::kPrePlabel})) {
      return;
    }
    for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
      rows.emplace_back(&mutation.old_rows[i], -1);
      rows.emplace_back(&mutation.new_rows[i], 1);
    }
    break;
  }

  if (is_num) {
    for (const auto &[row, sign] : rows) {
      std::string_view adsh, tag, version, uom;
      double value = 0.0;
      const Cell c_adsh = GetString(*row, Cols::kNumAdsh, adsh);
      const Cell c_tag = GetString(*row, Cols::kNumTag, tag);
      const Cell c_version = GetString(*row, Cols::kNumVersion, version);
      const Cell c_uom = GetString(*row, Cols::kNumUom, uom);
      const Cell c_value = GetDouble(*row, Cols::kNumValue, value);
      if (c_adsh == Cell::Bad || c_tag == Cell::Bad || c_version == Cell::Bad ||
          c_uom == Cell::Bad || c_value == Cell::Bad) {
        buffer.MarkRebuild();
        return;
      }
      if (c_adsh == Cell::Null || c_tag == Cell::Null ||
          c_version == Cell::Null || c_uom == Cell::Null ||
          c_value == Cell::Null || uom != kUsd) {
        continue; // cannot join / filtered out
      }
      if (!std::isfinite(value)) {
        // inf / NaN cannot be removed from a sum again: rebuild.
        buffer.MarkRebuild();
        return;
      }
      buffer.AddNumDelta(adsh, tag, version, value, sign);
    }
    return;
  }

  if (is_sub) {
    // For updates, rows alternate old/new; drop pairs whose cohort-relevant
    // image is identical.
    const bool is_update = mutation.kind == core::MutationKind::Update;
    struct Parsed {
      bool ok = false;
      bool in_cohort = false;
      std::string adsh;
      std::string name;
      bool name_is_null = false;
    };
    auto parse = [&](const core::AuxRow &row, Parsed &p) {
      std::string_view adsh, name;
      int64_t fy = 0;
      const Cell c_adsh = GetString(row, Cols::kSubAdsh, adsh);
      const Cell c_name = GetString(row, Cols::kSubName, name);
      const Cell c_fy = GetInt(row, Cols::kSubFy, fy);
      if (c_adsh == Cell::Bad || c_name == Cell::Bad || c_fy == Cell::Bad) {
        return false;
      }
      p.in_cohort = c_fy == Cell::Ok && fy == kTargetFy && c_adsh == Cell::Ok;
      if (p.in_cohort) {
        p.adsh = std::string(adsh);
        p.name_is_null = c_name == Cell::Null;
        if (!p.name_is_null) {
          p.name = std::string(name);
        }
      }
      return true;
    };
    for (std::size_t i = 0; i < rows.size(); i += is_update ? 2 : 1) {
      Parsed first, second;
      if (!parse(*rows[i].first, first) ||
          (is_update && !parse(*rows[i + 1].first, second))) {
        buffer.MarkRebuild();
        return;
      }
      if (is_update && first.in_cohort && second.in_cohort &&
          first.adsh == second.adsh && first.name_is_null == second.name_is_null &&
          first.name == second.name) {
        continue;
      }
      if (first.in_cohort) {
        buffer.AddSubChange({first.adsh, first.name, first.name_is_null,
                             rows[i].second});
      }
      if (is_update && second.in_cohort) {
        buffer.AddSubChange({second.adsh, second.name, second.name_is_null,
                             rows[i + 1].second});
      }
    }
    return;
  }

  // pre
  const bool is_update = mutation.kind == core::MutationKind::Update;
  struct ParsedPre {
    bool relevant = false;
    std::string adsh, tag, version, plabel;
    bool plabel_is_null = false;
  };
  auto parse_pre = [&](const core::AuxRow &row, ParsedPre &p) {
    std::string_view adsh, stmt, tag, version, plabel;
    const Cell c_adsh = GetString(row, Cols::kPreAdsh, adsh);
    const Cell c_stmt = GetString(row, Cols::kPreStmt, stmt);
    const Cell c_tag = GetString(row, Cols::kPreTag, tag);
    const Cell c_version = GetString(row, Cols::kPreVersion, version);
    const Cell c_plabel = GetString(row, Cols::kPrePlabel, plabel);
    if (c_adsh == Cell::Bad || c_stmt == Cell::Bad || c_tag == Cell::Bad ||
        c_version == Cell::Bad || c_plabel == Cell::Bad) {
      return false;
    }
    p.relevant = c_stmt == Cell::Ok && stmt == kIsStmt && c_adsh == Cell::Ok &&
                 c_tag == Cell::Ok && c_version == Cell::Ok;
    if (p.relevant) {
      p.adsh = std::string(adsh);
      p.tag = std::string(tag);
      p.version = std::string(version);
      p.plabel_is_null = c_plabel == Cell::Null;
      if (!p.plabel_is_null) {
        p.plabel = std::string(plabel);
      }
    }
    return true;
  };
  for (std::size_t i = 0; i < rows.size(); i += is_update ? 2 : 1) {
    ParsedPre first, second;
    if (!parse_pre(*rows[i].first, first) ||
        (is_update && !parse_pre(*rows[i + 1].first, second))) {
      buffer.MarkRebuild();
      return;
    }
    if (is_update && first.relevant && second.relevant &&
        first.adsh == second.adsh && first.tag == second.tag &&
        first.version == second.version &&
        first.plabel_is_null == second.plabel_is_null &&
        first.plabel == second.plabel) {
      continue;
    }
    if (first.relevant) {
      buffer.AddPreChange({first.adsh, first.tag, first.version, first.plabel,
                           first.plabel_is_null, rows[i].second});
    }
    if (is_update && second.relevant) {
      buffer.AddPreChange({second.adsh, second.tag, second.version,
                           second.plabel, second.plabel_is_null,
                           rows[i + 1].second});
    }
  }
}

std::shared_ptr<const SecEdgarQ6State>
SecEdgarQ6Module::LoadPublishedState(core::AuxEngineServices &services,
                                     core::Version *version) const {
  // Validity and the state's handle in one linearized observation.
  thread_local std::vector<core::PartitionId> wanted(1);
  wanted[0] = kStatePartition;
  const auto snapshot = services.StateStore().ReadHitPartitions(
      services.CurrentVisibility(), {Id(), {}}, wanted);
  if (!snapshot.available) {
    return nullptr;
  }
  for (const auto &image : snapshot.partitions) {
    if (image.partition != kStatePartition) {
      continue;
    }
    const auto *typed = image.As<SecEdgarQ6StateImage>();
    if (typed && typed->state) {
      if (version) {
        *version = image.version;
      }
      return typed->state;
    }
  }
  return nullptr;
}

core::StagedMaintenanceOutcome
SecEdgarQ6Module::StageMaintenance(core::AuxEngineServices &services,
                                   const core::InvalidationPlan &,
                                   core::AuxTxnBuffer &base_buffer) {
  auto &buffer = static_cast<SecEdgarQ6TxnBuffer &>(base_buffer);
  core::StagedMaintenanceOutcome outcome;
  if (!buffer.HasPendingChanges()) {
    return outcome;
  }

  // Never built: nothing to maintain. Invalidated/racing: self-heal by rebuild.
  const auto validity = services.StateStore().CheckValidity(
      {Id(), {}}, services.CurrentVisibility());
  if (validity.code == core::ValidityCode::Missing) {
    return outcome;
  }
  core::Version version = 0;
  const auto state = LoadPublishedState(services, &version);
  if (!state || !validity.IsValid()) {
    return RebuildOutcome();
  }

  // Structural changes (sub cohort/name, pre IS bridge): move the affected
  // adsh's contributions using targeted base lookups, falling back to a full
  // rebuild for malformed input, unsupported scans or oversized batches.
  constexpr std::size_t kMaxAffectedAdsh = 64;
  if (buffer.RequiresRebuild()) {
    return RebuildOutcome();
  }
  std::set<std::string> affected;
  for (const auto &change : buffer.SubChanges()) {
    affected.insert(change.adsh);
  }
  for (const auto &change : buffer.PreChanges()) {
    if (affected.count(change.adsh) || state->structure.IsCohort(change.adsh)) {
      affected.insert(change.adsh);
    }
  }
  if (!affected.empty()) {
    if (!initial_scan || affected.size() > kMaxAffectedAdsh) {
      return RebuildOutcome();
    }
    const auto decision = services.ChooseMaintenance(
        {static_cast<uint64_t>(buffer.NumDeltas().size() + affected.size()), 1,
         true, false, true, false, false, core::MaintenanceAction::NoOp,
         services.ExpectedQueriesBeforeNextWrite()});
    if (decision.action != core::MaintenanceAction::ApplyDelta) {
      return RebuildOutcome();
    }
    std::vector<std::string> adshs(affected.begin(), affected.end());
    std::vector<SecEdgarQ6AdshSnapshot> snapshots;
    if (!initial_scan->LoadAdshSnapshots(adshs, snapshots) ||
        snapshots.size() != adshs.size()) {
      return RebuildOutcome();
    }
    // Transaction num deltas per (adsh, tag, version) to reconstruct the
    // pre-transaction sums, and the deltas that bypass the edit (other adsh).
    std::unordered_map<std::string, SecEdgarQ6NumSum> txn;
    std::vector<SecEdgarQ6NumRef> refs;
    for (const auto &d : buffer.NumDeltas()) {
      const std::string adsh(buffer.Adsh(d));
      if (!affected.count(adsh)) {
        refs.push_back({buffer.Adsh(d), buffer.Tag(d), buffer.Version(d), d.value, d.sign});
        continue;
      }
      std::string key = adsh;
      key.push_back('\0');
      key.append(buffer.Tag(d));
      key.push_back('\0');
      key.append(buffer.Version(d));
      auto &entry = txn[key];
      entry.tag = std::string(buffer.Tag(d));
      entry.version = std::string(buffer.Version(d));
      const auto added = SecEdgarQ6SumAdd(
          {entry.sum, entry.sum_lo}, {static_cast<double>(d.sign) * d.value, 0.0});
      entry.sum = added.hi;
      entry.sum_lo = added.lo;
      entry.cnt += d.sign;
    }
    std::vector<SecEdgarQ6AdshEdit> edits(adshs.size());
    for (std::size_t i = 0; i < adshs.size(); ++i) {
      auto &edit = edits[i];
      auto &snap = snapshots[i];
      edit.adsh = adshs[i];
      edit.new_cohort = snap.in_cohort;
      edit.name = std::move(snap.name);
      edit.name_is_null = snap.name_is_null;
      edit.pre = std::move(snap.pre);
      edit.new_nums = snap.nums;
      // old = now - txn deltas
      std::unordered_map<std::string, SecEdgarQ6NumSum> old_map;
      for (const auto &n : snap.nums) {
        std::string key = adshs[i];
        key.push_back('\0');
        key.append(n.tag);
        key.push_back('\0');
        key.append(n.version);
        old_map[key] = n;
      }
      for (const auto &[key, delta] : txn) {
        if (key.compare(0, adshs[i].size() + 1, adshs[i] + '\0') != 0) continue;
        auto &entry = old_map[key];
        entry.tag = delta.tag;
        entry.version = delta.version;
        const auto reduced = SecEdgarQ6SumAdd({entry.sum, entry.sum_lo},
                                              {-delta.sum, -delta.sum_lo});
        entry.sum = reduced.hi;
        entry.sum_lo = reduced.lo;
        entry.cnt -= delta.cnt;
      }
      for (auto &[key, n] : old_map) {
        if (n.cnt != 0) edit.old_nums.push_back(std::move(n));
      }
    }
    std::vector<SecEdgarQ6GroupDelta> group_deltas;
    SecEdgarQ6ResolveBatch(state->structure, refs.data(), refs.size(), group_deltas);
    bool overflow = false;
    std::shared_ptr<const SecEdgarQ6State> next;
    try {
      next = SecEdgarQ6ApplyEdits(*state, edits, group_deltas, &overflow);
    } catch (const std::length_error &) {
      return RebuildOutcome(); // id space exhausted: a fresh build compacts
    }
    if (!next || overflow) {
      return RebuildOutcome();
    }
    outcome.action = core::MaintenanceAction::ApplyDelta;
    outcome.updates.push_back(core::StateImage::Make(
        Id(), kStatePartition, version, MakeImage(std::move(next))));
    outcome.expected_version = 0;
    return outcome;
  }
  if (buffer.NumDeltas().empty()) {
    return outcome; // nothing relevant
  }

  const auto decision = services.ChooseMaintenance(
      {static_cast<uint64_t>(buffer.NumDeltas().size()), 1, true, false, true,
       false, false, core::MaintenanceAction::NoOp,
       services.ExpectedQueriesBeforeNextWrite()});
  if (decision.action != core::MaintenanceAction::ApplyDelta) {
    return RebuildOutcome(); // one partition: a partition rebuild is a full one
  }

  const auto &deltas = buffer.NumDeltas();
  std::vector<SecEdgarQ6NumRef> refs;
  refs.reserve(deltas.size());
  for (const auto &d : deltas) {
    refs.push_back({buffer.Adsh(d), buffer.Tag(d), buffer.Version(d), d.value,
                    d.sign});
  }
  std::vector<SecEdgarQ6GroupDelta> group_deltas;
  group_deltas.reserve(refs.size());
  if (refs.size() == 1) {
    const auto &r = refs[0];
    SecEdgarQ6ExpandPayload(
        SecEdgarQ6Resolve(state->structure, r.adsh, r.tag, r.version), r.value,
        r.sign, group_deltas);
  } else {
    SecEdgarQ6ResolveBatch(state->structure, refs.data(), refs.size(),
                           group_deltas);
  }
  bool overflow = false;
  auto next = SecEdgarQ6ApplyDeltas(*state, group_deltas, &overflow);
  if (overflow) {
    return RebuildOutcome(); // a sum left the double range: not removable exactly
  }
  if (!next) {
    return outcome; // none of the rows reach the result
  }
  outcome.action = core::MaintenanceAction::ApplyDelta;
  outcome.updates.push_back(core::StateImage::Make(
      Id(), kStatePartition, version, MakeImage(std::move(next))));
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
  // The query has no parameters; arguments are ignored.
  const auto state = LoadPublishedState(services, nullptr);
  if (!state) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  EmitState(*state, sink);
}

void SecEdgarQ6Module::ExecuteFallback(core::AuxEngineServices &,
                                       const core::QueryArguments &,
                                       core::AuxRowSink &sink) {
  EmitState(*BuildState(), sink);
}

void SecEdgarQ6Module::EmitState(const SecEdgarQ6State &state,
                                 core::AuxRowSink &sink) const {
  sink.Begin({{{"name", core::AuxType::String, true},
               {"stmt", core::AuxType::String, false},
               {"tag", core::AuxType::String, false},
               {"plabel", core::AuxType::String, true},
               {"total_value", core::AuxType::Double, false},
               {"cnt", core::AuxType::Int64, false}}});
  for (auto &row : SecEdgarQ6TopRows(state)) {
    core::AuxRow out;
    out.values.reserve(6);
    out.values.push_back(
        row.name_is_null
            ? core::AuxValue::Null(core::AuxType::String)
            : core::AuxValue::Of(core::AuxType::String, std::move(row.name)));
    out.values.push_back(
        core::AuxValue::Of(core::AuxType::String, std::move(row.stmt)));
    out.values.push_back(
        core::AuxValue::Of(core::AuxType::String, std::move(row.tag)));
    out.values.push_back(
        row.plabel_is_null
            ? core::AuxValue::Null(core::AuxType::String)
            : core::AuxValue::Of(core::AuxType::String, std::move(row.plabel)));
    out.values.push_back(
        core::AuxValue::Of(core::AuxType::Double, row.total_value));
    out.values.push_back(core::AuxValue::Of(core::AuxType::Int64, row.cnt));
    sink.WriteRow(out);
  }
  sink.End();
}

} // namespace accelerator_codegen::aux_state::query
