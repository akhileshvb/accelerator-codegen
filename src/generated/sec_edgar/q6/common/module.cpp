#include "sec_edgar/q6/common/module.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
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

constexpr int64_t kDefaultFy = 2023;
constexpr std::string_view kDefaultStmt = "IS";
constexpr std::string_view kDefaultUom = "USD";
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
                                      std::string_view version, std::string_view uom,
                                      double value, int8_t sign) {
  pending_ = true;
  NumDelta d;
  d.offset = static_cast<uint32_t>(arena_.size());
  d.adsh_len = static_cast<uint32_t>(adsh.size());
  d.tag_len = static_cast<uint32_t>(tag.size());
  d.version_len = static_cast<uint32_t>(version.size());
  d.uom_len = static_cast<uint32_t>(uom.size());
  d.value = value;
  d.sign = sign;
  arena_.insert(arena_.end(), adsh.begin(), adsh.end());
  arena_.insert(arena_.end(), tag.begin(), tag.end());
  arena_.insert(arena_.end(), version.begin(), version.end());
  arena_.insert(arena_.end(), uom.begin(), uom.end());
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
    if (!in.fy_is_null) {
      builder.AddCohort(in.adsh, in.fy, in.name, in.name_is_null);
    }
  });
  initial_scan->ScanPre([&](const SecEdgarQ6PreInput &in) {
    builder.AddPre(in.adsh, in.tag, in.version, in.stmt, in.plabel,
                   in.plabel_is_null);
  });
  auto state = std::make_shared<SecEdgarQ6State>(
      SecEdgarQ6MakeEmptyState(builder.Finish()));

  // Fast path: the engine aggregates num x pre x sub itself (its own sums are
  // the base values, bit for bit).
  bool failed = false;
  const bool supported = initial_scan->ScanGroupAggregates(
      [&](const SecEdgarQ6GroupAggregateInput &g) {
        if (failed) {
          return;
        }
        auto &structure = state->structure;
        uint32_t name_id = 0;
        if (!g.name_is_null) {
          name_id = structure.names.Find(g.name);
        }
        const uint32_t tag_id = structure.tags.Find(g.tag);
        uint32_t plabel_id = 0;
        if (!g.plabel_is_null) {
          plabel_id = structure.plabels.Find(g.plabel);
        }
        const uint32_t stmt_id = structure.stmts.Find(g.stmt);
        if ((name_id == 0 && !g.name_is_null) || tag_id == 0 ||
            (plabel_id == 0 && !g.plabel_is_null) || stmt_id == 0) {
          failed = true;
          return;
        }
        const uint32_t combo =
            structure.FindOrAddCombo(g.fy, stmt_id, structure.uoms.Intern(g.uom));
        SecEdgarQ6SetGroup(
            *state, structure.FindOrAddGroup(name_id, tag_id, plabel_id, combo),
            g.sum, g.count);
      });
  if (!supported || failed) {
    // Generic path: stream num and resolve every contribution through the
    // bridge. Sums are accumulated as double-doubles (order independent).
    // drop the groups of the failed fast path: restart from the bare structure
    builder = SecEdgarQ6StructureBuilder();
    initial_scan->ScanSub([&](const SecEdgarQ6SubInput &in) {
      if (!in.fy_is_null) {
        builder.AddCohort(in.adsh, in.fy, in.name, in.name_is_null);
      }
    });
    initial_scan->ScanPre([&](const SecEdgarQ6PreInput &in) {
      builder.AddPre(in.adsh, in.tag, in.version, in.stmt, in.plabel,
                     in.plabel_is_null);
    });
    *state = SecEdgarQ6MakeEmptyState(builder.Finish());
    auto &st = state->structure;
    std::vector<SecEdgarQ6Sum> sums;
    std::vector<int64_t> counts;
    std::vector<SecEdgarQ6GroupDelta> scratch;
    initial_scan->ScanNum([&](const SecEdgarQ6NumInput &in) {
      if (in.value_is_null) {
        return;
      }
      const SecEdgarQ6Hit hit = SecEdgarQ6Resolve(st, in.adsh, in.tag, in.version);
      if (!hit.Found()) {
        return;
      }
      scratch.clear();
      SecEdgarQ6ExpandHit(st, hit, st.uoms.Intern(in.uom), {in.value, 0.0}, 1,
                          scratch);
      if (st.GroupCount() > sums.size()) {
        sums.resize(st.GroupCount());
        counts.resize(st.GroupCount(), 0);
      }
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
  state->structure.base_group_count = state->structure.group_count;
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
          c_value == Cell::Null) {
        continue; // cannot join / filtered out
      }
      if (!std::isfinite(value)) {
        // inf / NaN cannot be removed from a sum again: rebuild.
        buffer.MarkRebuild();
        return;
      }
      buffer.AddNumDelta(adsh, tag, version, uom, value, sign);
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
      int64_t fy = 0;
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
      p.in_cohort = c_fy == Cell::Ok && c_adsh == Cell::Ok &&
                    fy >= std::numeric_limits<int32_t>::min() &&
                    fy <= std::numeric_limits<int32_t>::max();
      if (p.in_cohort) {
        p.fy = fy;
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
          first.adsh == second.adsh && first.fy == second.fy &&
          first.name_is_null == second.name_is_null && first.name == second.name) {
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
    std::string adsh, tag, version, stmt, plabel;
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
    p.relevant = c_stmt == Cell::Ok && c_adsh == Cell::Ok &&
                 c_tag == Cell::Ok && c_version == Cell::Ok;
    if (p.relevant) {
      p.adsh = std::string(adsh);
      p.tag = std::string(tag);
      p.version = std::string(version);
      p.stmt = std::string(stmt);
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
        first.version == second.version && first.stmt == second.stmt &&
        first.plabel_is_null == second.plabel_is_null &&
        first.plabel == second.plabel) {
      continue;
    }
    if (first.relevant) {
      buffer.AddPreChange({first.adsh, first.tag, first.version, first.stmt, first.plabel,
                           first.plabel_is_null, rows[i].second});
    }
    if (is_update && second.relevant) {
      buffer.AddPreChange({second.adsh, second.tag, second.version, second.stmt,
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
    // Transaction num deltas per (adsh, tag, version, uom) to reconstruct the
    // pre-transaction sums, and the deltas that bypass the edit (other adsh).
    const auto num_key = [](std::string_view adsh, std::string_view tag,
                            std::string_view version, std::string_view uom) {
      std::string key(adsh);
      key.push_back('\0');
      key.append(tag);
      key.push_back('\0');
      key.append(version);
      key.push_back('\0');
      key.append(uom);
      return key;
    };
    std::unordered_map<std::string, SecEdgarQ6NumSum> txn;
    std::vector<SecEdgarQ6NumRef> refs;
    for (const auto &d : buffer.NumDeltas()) {
      const std::string adsh(buffer.Adsh(d));
      if (!affected.count(adsh)) {
        refs.push_back({buffer.Adsh(d), buffer.Tag(d), buffer.Version(d),
                        buffer.Uom(d), d.value, d.sign});
        continue;
      }
      auto &entry = txn[num_key(adsh, buffer.Tag(d), buffer.Version(d), buffer.Uom(d))];
      entry.tag = std::string(buffer.Tag(d));
      entry.version = std::string(buffer.Version(d));
      entry.uom = std::string(buffer.Uom(d));
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
      edit.fy = snap.fy;
      edit.name = std::move(snap.name);
      edit.name_is_null = snap.name_is_null;
      edit.pre = std::move(snap.pre);
      edit.new_nums = snap.nums;
      // old = now - txn deltas
      std::unordered_map<std::string, SecEdgarQ6NumSum> old_map;
      for (const auto &n : snap.nums) {
        old_map[num_key(adshs[i], n.tag, n.version, n.uom)] = n;
      }
      for (const auto &[key, delta] : txn) {
        if (key.compare(0, adshs[i].size() + 1, adshs[i] + '\0') != 0) continue;
        auto &entry = old_map[key];
        entry.tag = delta.tag;
        entry.version = delta.version;
        entry.uom = delta.uom;
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
    bool overflow = false;
    std::shared_ptr<const SecEdgarQ6State> next;
    try {
      next = SecEdgarQ6ApplyEdits(*state, edits, refs, &overflow);
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
    refs.push_back({buffer.Adsh(d), buffer.Tag(d), buffer.Version(d),
                    buffer.Uom(d), d.value, d.sign});
  }
  bool overflow = false;
  std::shared_ptr<const SecEdgarQ6State> next;
  try {
    next = SecEdgarQ6ApplyNums(*state, refs.data(), refs.size(), &overflow);
  } catch (const std::length_error &) {
    return RebuildOutcome();
  }
  if (overflow) {
    return RebuildOutcome(); // a sum left the double range / group garbage
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

namespace {

struct QueryParams {
  bool is_null = false;
  int64_t fy = kDefaultFy;
  std::string stmt{kDefaultStmt};
  std::string uom{kDefaultUom};
};

QueryParams ParseArguments(const core::QueryArguments &args) {
  QueryParams p;
  if (args.values.empty()) {
    return p;
  }
  if (args.values.size() != 3) {
    throw std::invalid_argument(
        "sec_edgar q6 aux expects () or (fy BIGINT, stmt VARCHAR, uom VARCHAR)");
  }
  const auto &fy = args.values[0];
  if (fy.is_null || args.values[1].is_null || args.values[2].is_null) {
    p.is_null = true;
    return p;
  }
  if (std::holds_alternative<int64_t>(fy.payload)) {
    p.fy = std::get<int64_t>(fy.payload);
  } else if (std::holds_alternative<uint64_t>(fy.payload)) {
    const uint64_t v = std::get<uint64_t>(fy.payload);
    if (v > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      p.is_null = true; // no fiscal year (INTEGER) can equal it: empty result
      return p;
    }
    p.fy = static_cast<int64_t>(v);
  } else if (std::holds_alternative<double>(fy.payload)) {
    const double v = std::get<double>(fy.payload);
    if (!(v == std::floor(v)) || !(std::fabs(v) < 9.0e18)) {
      p.is_null = true; // fractional / out of range: equals no integer fy
      return p;
    }
    p.fy = static_cast<int64_t>(v);
  } else if (std::holds_alternative<std::string>(fy.payload)) {
    const std::string &text = std::get<std::string>(fy.payload);
    char *end = nullptr;
    errno = 0;
    const long long v = std::strtoll(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || errno == ERANGE) {
      p.is_null = true; // not an int64: equals no fiscal year
      return p;
    }
    p.fy = v;
  } else {
    throw std::invalid_argument("sec_edgar q6 aux: unsupported fy argument");
  }
  for (int i = 1; i <= 2; ++i) {
    if (!std::holds_alternative<std::string>(args.values[i].payload)) {
      throw std::invalid_argument("sec_edgar q6 aux: stmt and uom must be strings");
    }
  }
  p.stmt = std::get<std::string>(args.values[1].payload);
  p.uom = std::get<std::string>(args.values[2].payload);
  return p;
}

} // namespace

void SecEdgarQ6Module::Execute(core::AuxEngineServices &services,
                               const core::QueryArguments &arguments,
                               core::AuxRowSink &sink) {
  const QueryParams params = ParseArguments(arguments);
  if (params.is_null) {
    EmitRows({}, sink);
    return;
  }
  const auto state = LoadPublishedState(services, nullptr);
  if (!state) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  EmitRows(SecEdgarQ6TopRows(*state, params.fy, params.stmt, params.uom), sink);
}

void SecEdgarQ6Module::ExecuteFallback(core::AuxEngineServices &,
                                       const core::QueryArguments &arguments,
                                       core::AuxRowSink &sink) {
  const QueryParams params = ParseArguments(arguments);
  if (params.is_null) {
    EmitRows({}, sink);
    return;
  }
  EmitRows(SecEdgarQ6TopRows(*BuildState(), params.fy, params.stmt, params.uom),
           sink);
}

void SecEdgarQ6Module::EmitRows(const std::vector<SecEdgarQ6OutputRow> &rows_in,
                                core::AuxRowSink &sink) const {
  sink.Begin({{{"name", core::AuxType::String, true},
               {"stmt", core::AuxType::String, false},
               {"tag", core::AuxType::String, false},
               {"plabel", core::AuxType::String, true},
               {"total_value", core::AuxType::Double, false},
               {"cnt", core::AuxType::Int64, false}}});
  for (auto row : rows_in) {
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
