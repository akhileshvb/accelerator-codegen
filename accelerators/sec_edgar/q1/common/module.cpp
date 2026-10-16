#include "sec_edgar/q1/common/module.hpp"

#include "sec_edgar/q1/common/adsh_key.hpp"
#include "substrate/aux/core/parallel_slices.hpp"

#include <algorithm>
#include <atomic>
#include <numeric>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>
#include <variant>

namespace accelerator_codegen::aux_state::query {
namespace {

using q1::Id;
using q1::kNoId;
using q1::kOtherKeyBit;

// ---------------------------------------------------------------------------
// Mutation row parsing (compact pre row: stmt, rfile, adsh, line)
// ---------------------------------------------------------------------------

struct RowView {
  bool relevant = false; // stmt IS NOT NULL
  const std::string *stmt = nullptr;
  const std::string *rfile = nullptr;
  const std::string *adsh = nullptr;
  bool rfile_null = false;
  bool adsh_null = false;
  bool line_null = false;
  int32_t line = 0;
};

bool ReadString(const core::AuxValue &cell, const std::string *&value,
                bool &is_null) {
  if (cell.is_null) {
    value = nullptr;
    is_null = true;
    return true;
  }
  const auto *s = std::get_if<std::string>(&cell.payload);
  if (!s) {
    return false;
  }
  value = s;
  is_null = false;
  return true;
}

bool ReadRow(const core::AuxRow &row, RowView &view) {
  if (row.values.size() < 4) {
    return false;
  }
  bool stmt_null = false;
  if (!ReadString(row.values[0], view.stmt, stmt_null) ||
      !ReadString(row.values[1], view.rfile, view.rfile_null) ||
      !ReadString(row.values[2], view.adsh, view.adsh_null)) {
    return false;
  }
  const auto &line = row.values[3];
  if (line.is_null) {
    view.line_null = true;
    view.line = 0;
  } else {
    const auto *raw = std::get_if<int64_t>(&line.payload);
    if (!raw || *raw < std::numeric_limits<int32_t>::min() ||
        *raw > std::numeric_limits<int32_t>::max()) {
      return false;
    }
    view.line_null = false;
    view.line = static_cast<int32_t>(*raw);
  }
  view.relevant = !stmt_null;
  return true;
}

bool SameGroupAndAdsh(const RowView &a, const RowView &b) {
  if (a.rfile_null != b.rfile_null || a.adsh_null != b.adsh_null ||
      *a.stmt != *b.stmt) {
    return false;
  }
  if (!a.rfile_null && *a.rfile != *b.rfile) {
    return false;
  }
  return a.adsh_null || *a.adsh == *b.adsh;
}

void AddRow(SecEdgarQ1TxnBuffer &buffer, const RowView &row, int32_t sign) {
  if (!row.relevant) {
    return;
  }
  SecEdgarQ1TxnBuffer::Delta delta;
  delta.group = buffer.InternGroup(*row.stmt, row.rfile_null ? std::string() : *row.rfile,
                                   row.rfile_null);
  delta.d_cnt = sign;
  if (!row.adsh_null) {
    delta.key = buffer.InternAdsh(*row.adsh);
    delta.d_refc = sign;
  }
  if (!row.line_null) {
    delta.d_line_cnt = sign;
    delta.d_line_sum = static_cast<int64_t>(sign) * row.line;
  }
  buffer.AddDelta(delta);
}

// Update whose group/adsh did not change: only the line aggregate moves.
void AddLineOnly(SecEdgarQ1TxnBuffer &buffer, const RowView &old_row,
                 const RowView &new_row) {
  const int32_t d_cnt = (new_row.line_null ? 0 : 1) - (old_row.line_null ? 0 : 1);
  const int64_t d_sum = (new_row.line_null ? 0 : int64_t(new_row.line)) -
                        (old_row.line_null ? 0 : int64_t(old_row.line));
  if (d_cnt == 0 && d_sum == 0) {
    return;
  }
  SecEdgarQ1TxnBuffer::Delta delta;
  delta.group = buffer.InternGroup(*new_row.stmt,
                                   new_row.rfile_null ? std::string() : *new_row.rfile,
                                   new_row.rfile_null);
  delta.d_line_cnt = d_cnt;
  delta.d_line_sum = d_sum;
  buffer.AddDelta(delta);
}

// Rows per slice of a mutation's row loop (~100 ns a row).
constexpr std::size_t kAccumulateGrain = 8192;

// Buffers the deltas of rows [begin, end) of `mutation` into `buffer`.
void AccumulateRows(const core::MutationBatch &mutation, std::size_t begin,
                    std::size_t end, SecEdgarQ1TxnBuffer &buffer) {
  RowView view;
  switch (mutation.kind) {
  case core::MutationKind::Insert:
    for (std::size_t i = begin; i < end; ++i) {
      if (!ReadRow(mutation.new_rows[i], view)) {
        buffer.MarkRebuild();
        return;
      }
      AddRow(buffer, view, 1);
    }
    break;
  case core::MutationKind::Delete:
    for (std::size_t i = begin; i < end; ++i) {
      if (!ReadRow(mutation.old_rows[i], view)) {
        buffer.MarkRebuild();
        return;
      }
      AddRow(buffer, view, -1);
    }
    break;
  case core::MutationKind::Update: {
    RowView old_view;
    for (std::size_t i = begin; i < end; ++i) {
      if (!ReadRow(mutation.old_rows[i], old_view) ||
          !ReadRow(mutation.new_rows[i], view)) {
        buffer.MarkRebuild();
        return;
      }
      if (old_view.relevant && view.relevant && SameGroupAndAdsh(old_view, view)) {
        AddLineOnly(buffer, old_view, view);
      } else {
        AddRow(buffer, old_view, -1);
        AddRow(buffer, view, 1);
      }
    }
    break;
  }
  }
}

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------

std::shared_ptr<const SecEdgarQ1StateImage>
MakeImage(std::shared_ptr<const SecEdgarQ1State> state) {
  auto image = std::make_shared<SecEdgarQ1StateImage>();
  image->state = std::move(state);
  return image;
}

core::InvalidationPlan AllPartitions() {
  core::InvalidationPlan plan;
  plan.scope = ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

core::InvalidationPlan NoInvalidation() {
  core::InvalidationPlan plan;
  plan.scope = ::accelerator_codegen::aux_state::InvalidationScope::None;
  return plan;
}

core::StagedMaintenanceOutcome RebuildOutcome(core::MaintenanceAction action) {
  core::StagedMaintenanceOutcome outcome;
  outcome.action = action;
  outcome.invalidations = AllPartitions();
  outcome.pending_rebuild = outcome.invalidations;
  return outcome;
}

} // namespace

// ---------------------------------------------------------------------------
// Txn buffer
// ---------------------------------------------------------------------------

bool SecEdgarQ1TxnBuffer::HasPendingChanges() const { return pending; }

void SecEdgarQ1TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  deltas.clear();
  groups.clear();
  group_index.clear();
  others.clear();
  other_index.clear();
}

bool SecEdgarQ1TxnBuffer::RequiresRebuild() const { return requires_rebuild; }

void SecEdgarQ1TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  deltas.clear();
}

uint32_t SecEdgarQ1TxnBuffer::InternGroup(const std::string &stmt,
                                          const std::string &rfile,
                                          bool rfile_is_null) {
  std::string name = std::to_string(stmt.size());
  name.push_back(':');
  name += stmt;
  name.push_back(rfile_is_null ? '\1' : '\2');
  if (!rfile_is_null) {
    name += rfile;
  }
  const auto it = group_index.find(name);
  if (it != group_index.end()) {
    return it->second;
  }
  groups.push_back({stmt, rfile_is_null ? std::string() : rfile, rfile_is_null});
  const auto index = static_cast<uint32_t>(groups.size() - 1);
  group_index.emplace(std::move(name), index);
  return index;
}

uint64_t SecEdgarQ1TxnBuffer::InternAdsh(const std::string &adsh) {
  uint64_t key = 0;
  if (q1::ParseAdshKey(adsh.data(), adsh.size(), key)) {
    return key;
  }
  const auto it = other_index.find(adsh);
  if (it != other_index.end()) {
    return kOtherKeyBit | it->second;
  }
  others.push_back(adsh);
  other_index.emplace(adsh, static_cast<uint32_t>(others.size() - 1));
  return kOtherKeyBit | (others.size() - 1);
}

void SecEdgarQ1TxnBuffer::AddDelta(const Delta &delta) {
  pending = true;
  deltas.push_back(delta);
}

void SecEdgarQ1TxnBuffer::Absorb(SecEdgarQ1TxnBuffer &&from) {
  if (requires_rebuild) {
    return;
  }
  if (from.requires_rebuild) {
    MarkRebuild();
    return;
  }
  std::vector<uint32_t> group_of;
  group_of.reserve(from.groups.size());
  for (const auto &g : from.groups) {
    group_of.push_back(InternGroup(g.stmt, g.rfile, g.rfile_is_null));
  }
  std::vector<uint64_t> other_of;
  other_of.reserve(from.others.size());
  for (const auto &adsh : from.others) {
    other_of.push_back(InternAdsh(adsh));
  }
  for (auto delta : from.deltas) {
    delta.group = group_of[delta.group];
    if (delta.d_refc != 0 && (delta.key & kOtherKeyBit)) {
      delta.key = other_of[delta.key & ~kOtherKeyBit];
    }
    AddDelta(delta);
  }
}

// ---------------------------------------------------------------------------
// Delta application (copy-on-write)
// ---------------------------------------------------------------------------

namespace {

struct RefEntry {
  Id group;
  Id id;
  int32_t delta;
};

// Entries per slice of the refcount application (a few hundred ns each).
constexpr std::size_t kApplyGrain = 8192;

// Applies one group's refcount deltas to a private copy of its chunk
// directories (every untouched chunk is shared). False: underflow.
bool ApplyGroupRefs(q1::GroupData &g, RefEntry *first, RefEntry *last) {
  std::sort(first, last, [](const RefEntry &l, const RefEntry &r) { return l.id < r.id; });
  auto refs = g.refs ? std::make_shared<q1::GroupRefs>(*g.refs) : std::make_shared<q1::GroupRefs>();
  int64_t distinct = g.distinct;
  while (first != last) {
    const Id id = first->id;
    int64_t delta = 0;
    for (; first != last && first->id == id; ++first) {
      delta += first->delta;
    }
    if (delta == 0) {
      continue;
    }
    const uint64_t old_count = refs->Get(id);
    if (delta < 0 && old_count < static_cast<uint64_t>(-delta)) {
      return false;
    }
    const uint64_t updated = static_cast<uint64_t>(static_cast<int64_t>(old_count) + delta);
    refs->Set(id, updated);
    distinct += (updated > 0) - (old_count > 0);
  }
  g.distinct = distinct;
  g.refs = std::move(refs);
  return true;
}

} // namespace

bool ApplySecEdgarQ1Deltas(const core::AuxEngineServices &services,
                           SecEdgarQ1State &state,
                           const SecEdgarQ1TxnBuffer &buffer) {
  const auto &local_groups = buffer.Groups();
  std::vector<int> gmap(local_groups.size());
  for (std::size_t i = 0; i < local_groups.size(); ++i) {
    gmap[i] = state.GetOrCreateGroup(local_groups[i].stmt, local_groups[i].rfile,
                                     local_groups[i].rfile_is_null);
  }
  // 1) group aggregates
  for (const auto &d : buffer.Deltas()) {
    auto &g = state.groups[static_cast<std::size_t>(gmap[d.group])];
    g.cnt += d.d_cnt;
    g.line_sum += d.d_line_sum;
    g.line_cnt += d.d_line_cnt;
  }
  for (const int gi : gmap) {
    const auto &g = state.groups[static_cast<std::size_t>(gi)];
    if (g.cnt < 0 || g.line_cnt < 0 || g.line_cnt > g.cnt) {
      return false;
    }
  }

  // 2) accession refcounts
  std::vector<RefEntry> entries;
  std::vector<uint64_t> other_key_cache(buffer.OtherAdsh().size(), 0);
  std::vector<uint8_t> other_key_ready(buffer.OtherAdsh().size(), 0);
  bool have_last = false;
  bool last_created = false;
  uint64_t last_key = 0;
  Id last_id = kNoId;
  for (const auto &d : buffer.Deltas()) {
    if (d.d_refc == 0) {
      continue;
    }
    uint64_t key = d.key;
    if (key & kOtherKeyBit) {
      const auto local = static_cast<std::size_t>(key & ~kOtherKeyBit);
      if (!other_key_ready[local]) {
        // A delete of a non-canonical accession we never saw interns it here
        // (harmless: `state` is discarded when the delete then fails below).
        other_key_cache[local] =
            kOtherKeyBit | state.others.Intern(buffer.OtherAdsh()[local]);
        other_key_ready[local] = 1;
      }
      key = other_key_cache[local];
    }
    // Consecutive rows of one filing share the accession: skip the lookup.
    Id id;
    if (have_last && key == last_key && (d.d_refc < 0 || last_created)) {
      id = last_id;
    } else {
      id = d.d_refc > 0 ? state.GetOrCreateId(key) : state.FindId(key);
      last_key = key;
      last_id = id;
      last_created = d.d_refc > 0;
      have_last = true;
    }
    if (id == kNoId) {
      return false; // delete of an accession we never saw
    }
    entries.push_back({static_cast<Id>(gmap[d.group]), id, d.d_refc});
  }
  // Bucket by group (counting sort): each group's refcounts are then applied
  // independently, with disjoint writes to its own copy of the chunk directories.
  std::vector<std::size_t> run_begin(state.groups.size() + 1, 0);
  for (const auto &e : entries) {
    ++run_begin[e.group + 1];
  }
  std::partial_sum(run_begin.begin(), run_begin.end(), run_begin.begin());
  std::vector<RefEntry> by_group(entries.size());
  {
    auto next_slot = run_begin;
    for (const auto &e : entries) {
      by_group[next_slot[e.group]++] = e;
    }
  }
  std::vector<Id> touched;
  for (std::size_t g = 0; g + 1 < run_begin.size(); ++g) {
    if (run_begin[g] != run_begin[g + 1]) {
      touched.push_back(static_cast<Id>(g));
    }
  }
  std::atomic<bool> consistent{true};
  const std::size_t slices = std::min(
      touched.size(), core::SliceCount(services, entries.size(), kApplyGrain));
  core::RunSlices(services, std::max<std::size_t>(1, slices), touched.size(),
                  [&](std::size_t, std::size_t begin, std::size_t end) {
                    for (std::size_t k = begin; k < end && consistent.load(); ++k) {
                      const Id group = touched[k];
                      if (!ApplyGroupRefs(state.groups[group], by_group.data() + run_begin[group],
                                          by_group.data() + run_begin[group + 1])) {
                        consistent = false;
                      }
                    }
                  });
  if (!consistent) {
    return false; // underflow: the state is not what the writes assume
  }

  // Reclaim dictionary growth from churned accessions by rebuilding.
  const Id grown = state.next_id - state.ids_at_build;
  if (grown > std::max<Id>(65536u, state.ids_at_build)) {
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Module
// ---------------------------------------------------------------------------

core::AcceleratorId SecEdgarQ1Module::Id() const { return kId; }

std::vector<core::TableDependency> SecEdgarQ1Module::Dependencies() const {
  return {{config.pre_table, {0, 1, 2, 3}, config.pre_attnums}};
}

std::unique_ptr<core::AuxTxnBuffer> SecEdgarQ1Module::CreateTxnBuffer() const {
  return std::make_unique<SecEdgarQ1TxnBuffer>();
}

SecEdgarQ1State
SecEdgarQ1Module::BuildState(core::AuxEngineServices &services) const {
  if (!initial_scan) {
    throw std::logic_error("SEC EDGAR Q1 requires a typed initial scan source");
  }
  q1::Builder builder;
  if (!initial_scan->ScanPreInto(builder)) {
    initial_scan->ScanPre(
        [&](const SecEdgarQ1PreInput &input) { builder.ConsumeRow(input); });
  }
  return builder.Finish(&services);
}

void SecEdgarQ1Module::Build(core::AuxEngineServices &services,
                             core::StateBuilder &builder) {
  auto state = std::make_shared<const SecEdgarQ1State>(BuildState(services));
  builder.Write(core::StateImage::Make(kId, kStatePartition, 0,
                                       MakeImage(std::move(state))));
  builder.Finish();
}

core::InvalidationPlan
SecEdgarQ1Module::InterpretMutation(const core::MutationBatch &) const {
  // Every INSERT/DELETE/UPDATE on pre is maintained incrementally from the
  // transaction buffer (StageMaintenance falls back to a rebuild when it
  // cannot), so no lazy invalidation is requested here.
  return NoInvalidation();
}

void SecEdgarQ1Module::AccumulateMutation(const core::MutationBatch &mutation,
                                          core::AuxEngineServices &services,
                                          core::AuxTxnBuffer &base_buffer) {
  auto &buffer = static_cast<SecEdgarQ1TxnBuffer &>(base_buffer);
  if (mutation.table != config.pre_table || buffer.RequiresRebuild()) {
    return; // a pending rebuild supersedes any deltas
  }
  if (mutation.kind == core::MutationKind::Update &&
      mutation.old_rows.size() != mutation.new_rows.size()) {
    buffer.MarkRebuild();
    return;
  }
  const auto &rows =
      mutation.kind == core::MutationKind::Insert ? mutation.new_rows : mutation.old_rows;
  core::ParallelReduce(
      services, rows.size(), kAccumulateGrain, buffer,
      [&](SecEdgarQ1TxnBuffer &out, std::size_t begin, std::size_t end) {
        AccumulateRows(mutation, begin, end, out);
      },
      [](SecEdgarQ1TxnBuffer &into, SecEdgarQ1TxnBuffer &&from) {
        into.Absorb(std::move(from));
      });
}

std::shared_ptr<const SecEdgarQ1State>
SecEdgarQ1Module::LoadState(core::AuxEngineServices &services,
                            core::Version *version,
                            const char *accelerator_id) const {
  thread_local std::vector<core::PartitionId> wanted(1);
  wanted[0] = kStatePartition;
  // One observation: whole-state validity plus the immutable image handle.
  const auto snapshot = services.StateStore().ReadHitPartitions(
      services.CurrentVisibility(), core::QueryKey{core::AcceleratorId(accelerator_id ? accelerator_id : kId), {}}, wanted);
  if (!snapshot.available) {
    return nullptr;
  }
  for (const auto &image : snapshot.partitions) {
    if (image.partition != kStatePartition) {
      continue;
    }
    const auto *typed = image.As<SecEdgarQ1StateImage>();
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
SecEdgarQ1Module::StageForAccelerator(core::AuxEngineServices &services,
                                      core::AuxTxnBuffer &base_buffer,
                                      const char *accelerator_id) {
  auto &buffer = static_cast<SecEdgarQ1TxnBuffer &>(base_buffer);
  if (buffer.RequiresRebuild()) {
    const auto decision = services.ChooseMaintenance(
        {0, 0, false, false, true, true, false, core::MaintenanceAction::NoOp,
         services.ExpectedQueriesBeforeNextWrite()});
    return RebuildOutcome(decision.action == core::MaintenanceAction::ApplyDelta
                              ? core::MaintenanceAction::RebuildAll
                              : decision.action);
  }
  if (buffer.Deltas().empty()) {
    return {};
  }
  const auto decision = services.ChooseMaintenance(
      {static_cast<uint64_t>(buffer.Deltas().size()), 1, true, false, true,
       false, false, core::MaintenanceAction::NoOp,
       services.ExpectedQueriesBeforeNextWrite()});
  if (decision.action != core::MaintenanceAction::ApplyDelta) {
    return RebuildOutcome(decision.action);
  }
  core::Version version = 0;
  const auto current = LoadState(services, &version, accelerator_id);
  if (!current) {
    return RebuildOutcome(core::MaintenanceAction::RebuildAll);
  }
  SecEdgarQ1State next = *current; // O(groups + overlay); pages are shared
  if (!ApplySecEdgarQ1Deltas(services, next, buffer)) {
    return RebuildOutcome(core::MaintenanceAction::RebuildAll);
  }
  core::StagedMaintenanceOutcome outcome;
  outcome.action = core::MaintenanceAction::ApplyDelta;
  outcome.expected_version = 0;
  outcome.updates.push_back(core::StateImage::Make(
      accelerator_id, kStatePartition, version,
      MakeImage(std::make_shared<const SecEdgarQ1State>(std::move(next)))));
  return outcome;
}

core::StagedMaintenanceOutcome
SecEdgarQ1Module::StageMaintenance(core::AuxEngineServices &services,
                                   const core::InvalidationPlan &,
                                   core::AuxTxnBuffer &buffer) {
  return StageForAccelerator(services, buffer, kId);
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
  const auto state = LoadState(services);
  if (!state) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  Emit(*state, sink);
}

void SecEdgarQ1Module::ExecuteFallback(core::AuxEngineServices &services,
                                       const core::QueryArguments &,
                                       core::AuxRowSink &sink) {
  Emit(BuildState(services), sink);
}

} // namespace accelerator_codegen::aux_state::query
