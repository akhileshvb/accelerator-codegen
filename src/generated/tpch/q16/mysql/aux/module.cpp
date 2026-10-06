#include "tpch/q16/mysql/aux/module.hpp"
#include "tpch/q16/calibration_writes.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

#include "accelerator_codegen/stateful/core/sql_like.hpp"

namespace accelerator_codegen::aux_state::query {
namespace {

// ── Cell helpers ────────────────────────────────────────────────────────────
bool StringCell(const core::AuxRow &row, std::size_t index, std::string &out) {
  if (index >= row.values.size() || row.values[index].is_null ||
      !std::holds_alternative<std::string>(row.values[index].payload))
    return false;
  out = std::get<std::string>(row.values[index].payload);
  return true;
}

bool NullCell(const core::AuxRow &row, std::size_t index) {
  return index >= row.values.size() || row.values[index].is_null;
}

bool IntCell(const core::AuxRow &row, std::size_t index, int64_t &out) {
  if (index >= row.values.size() || row.values[index].is_null)
    return false;
  const auto &cell = row.values[index];
  if (std::holds_alternative<int64_t>(cell.payload)) {
    out = std::get<int64_t>(cell.payload);
    return true;
  }
  if (std::holds_alternative<uint64_t>(cell.payload)) {
    const auto value = std::get<uint64_t>(cell.payload);
    if (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
      return false;
    out = static_cast<int64_t>(value);
    return true;
  }
  return false;
}

core::InvalidationPlan All() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

// s_comment LIKE '%Customer%Complaints%' (a NULL comment is not a complaint).
bool IsComplaintComment(std::string_view comment) {
  static const auto matcher =
      core::sql_like::LikeMatcher::Compile("%Customer%Complaints%");
  return matcher.Match(comment);
}

// ── Row readers (projected as in Dependencies()) ────────────────────────────
// false: undecodable (forces a rebuild); `skip`: the row can never qualify
// (a NULL brand / type / size / key), so a new image of it is ignored.
bool ReadPart(const core::AuxRow &row, TpchQ16PartInput &out, bool &skip) {
  int64_t size = 0;
  if (!IntCell(row, 0, out.partkey))
    return false;
  skip = NullCell(row, 1) || NullCell(row, 2) || NullCell(row, 3);
  if (skip)
    return true;
  if (!StringCell(row, 1, out.brand) || !StringCell(row, 2, out.type) ||
      !IntCell(row, 3, size) || size < std::numeric_limits<int32_t>::min() ||
      size > std::numeric_limits<int32_t>::max())
    return false;
  out.size = static_cast<int32_t>(size);
  return true;
}

bool ReadPartsupp(const core::AuxRow &row, TpchQ16PartsuppInput &out,
                  bool &skip) {
  skip = NullCell(row, 0) || NullCell(row, 1);
  return skip || (IntCell(row, 0, out.partkey) && IntCell(row, 1, out.suppkey));
}

uint32_t Intern(std::shared_ptr<core::StringDict> &dict,
                const std::string &value) {
  const uint32_t id = dict->Find(value);
  if (id != core::StringDict::kNone)
    return id;
  if (dict.use_count() > 1) // shared with a published state: clone first
    dict = std::make_shared<core::StringDict>(*dict);
  return dict->Intern(value);
}

} // namespace

// ── TpchQ16State ────────────────────────────────────────────────────────────
int64_t TpchQ16State::SupplierAt(const TpchQ16Suppliers &slots,
                                 int64_t partkey, uint32_t i) const {
  if (i < kTpchQ16InlineSuppliers)
    return slots.suppkey[i];
  return (*supplier_overflow.Find(partkey))[i - kTpchQ16InlineSuppliers];
}

uint32_t TpchQ16State::GroupId(const TpchQ16PartInput &part) {
  const TpchQ16GroupKey key{Intern(brands, part.brand), Intern(types, part.type),
                            part.size};
  const auto it = group_table->ids.find(key);
  if (it != group_table->ids.end())
    return it->second;
  if (group_table.use_count() > 1) // shared with a published state
    group_table = std::make_shared<TpchQ16GroupTable>(*group_table);
  const auto gid = static_cast<uint32_t>(group_table->keys.size());
  group_table->keys.push_back(key);
  group_table->ids.emplace(key, gid);
  group_table->by_size[key.size].push_back(gid);
  return gid;
}

void TpchQ16State::AddToGroup(int64_t partkey, uint32_t gid) {
  auto &slot = members.Mutable(gid);
  if (!slot)
    slot = std::make_shared<TpchQ16Members>();
  else if (slot.use_count() > 1) // shared with a published state
    slot = std::make_shared<TpchQ16Members>(*slot);
  slot->Insert(partkey);
  MarkDirty(gid);
}

void TpchQ16State::RemoveFromGroup(int64_t partkey, uint32_t gid) {
  const auto *found = members.Find(gid);
  if (!found || !*found)
    return;
  auto &slot = members.Mutable(gid);
  if (slot.use_count() > 1)
    slot = std::make_shared<TpchQ16Members>(*slot);
  slot->Erase(partkey);
  MarkDirty(gid);
}

void TpchQ16State::SetPart(int64_t partkey, const TpchQ16PartInput &part) {
  if (partkey < 0)
    return; // negative keys are never stored
  const uint32_t gid = GroupId(part);
  const uint32_t old = part_group.Get(partkey);
  if (old == gid)
    return;
  if (old != kTpchQ16NoGroup)
    RemoveFromGroup(partkey, old);
  AddToGroup(partkey, gid);
  part_group.Set(partkey, gid);
}

void TpchQ16State::ErasePart(int64_t partkey) {
  const uint32_t old = part_group.Get(partkey);
  if (old == kTpchQ16NoGroup)
    return;
  RemoveFromGroup(partkey, old);
  part_group.Set(partkey, kTpchQ16NoGroup);
}

void TpchQ16State::AddSupplier(int64_t partkey, int64_t suppkey) {
  if (partkey < 0)
    return;
  auto &slots = suppliers_of.Mutable(partkey);
  if (slots.count < kTpchQ16InlineSuppliers)
    slots.suppkey[slots.count] = suppkey;
  else
    supplier_overflow[partkey].push_back(suppkey);
  ++slots.count;
  const uint32_t gid = part_group.Get(partkey);
  if (gid != kTpchQ16NoGroup)
    MarkDirty(gid);
}

bool TpchQ16State::RemoveSupplier(int64_t partkey, int64_t suppkey) {
  if (partkey < 0)
    return true;
  const auto *found = suppliers_of.Find(partkey);
  if (!found || found->count == 0)
    return false;
  const uint32_t count = found->count;
  uint32_t hit = count;
  for (uint32_t i = 0; i < count; ++i)
    if (SupplierAt(*found, partkey, i) == suppkey) {
      hit = i;
      break;
    }
  if (hit == count)
    return false;
  const int64_t last = SupplierAt(*found, partkey, count - 1);
  auto &slots = suppliers_of.Mutable(partkey); // `found` may dangle after this
  if (hit != count - 1) {
    if (hit < kTpchQ16InlineSuppliers)
      slots.suppkey[hit] = last;
    else
      supplier_overflow[partkey][hit - kTpchQ16InlineSuppliers] = last;
  }
  if (count > kTpchQ16InlineSuppliers) {
    auto &extra = supplier_overflow[partkey];
    extra.pop_back();
    if (extra.empty())
      supplier_overflow.Erase(partkey);
  }
  --slots.count;
  const uint32_t gid = part_group.Get(partkey);
  if (gid != kTpchQ16NoGroup)
    MarkDirty(gid);
  return true;
}

bool TpchQ16State::SetComplaint(int64_t suppkey, bool flag) {
  if (suppkey < 0)
    return false;
  const uint8_t value = flag ? 1 : 0;
  if (complaint.Get(suppkey) == value)
    return false;
  complaint.Set(suppkey, value);
  return true;
}

void TpchQ16State::MarkGroupsOfSuppliers(
    const core::FlatHashMap<uint8_t> &changed) {
  using Lists = core::ChunkedCowArray<TpchQ16Suppliers, 10>;
  for (std::size_t ci = 0; ci < suppliers_of.NumChunks(); ++ci) {
    const auto *chunk = suppliers_of.ChunkAt(ci);
    if (!chunk)
      continue;
    for (std::size_t slot = 0; slot < chunk->size(); ++slot) {
      const auto &slots = (*chunk)[slot];
      const int64_t partkey = Lists::ChunkBase(ci) + static_cast<int64_t>(slot);
      for (uint32_t i = 0; i < slots.count; ++i)
        if (changed.Contains(SupplierAt(slots, partkey, i))) {
          const uint32_t gid = part_group.Get(partkey);
          if (gid != kTpchQ16NoGroup)
            dirty.push_back(gid);
          break;
        }
    }
  }
}

// Distinct non-complaint suppliers over the parts of one group.
int64_t TpchQ16State::CountDistinct(uint32_t gid) const {
  const auto *found = members.Find(gid);
  if (!found || !*found)
    return 0;
  std::vector<int64_t> seen;
  (*found)->ForEach([&](int64_t partkey) {
    const auto *slots = suppliers_of.Find(partkey);
    if (!slots)
      return true;
    for (uint32_t i = 0; i < slots->count; ++i) {
      const int64_t suppkey = SupplierAt(*slots, partkey, i);
      if (!complaint.Get(suppkey))
        seen.push_back(suppkey);
    }
    return true;
  });
  std::sort(seen.begin(), seen.end());
  return static_cast<int64_t>(std::unique(seen.begin(), seen.end()) -
                              seen.begin());
}

void TpchQ16State::RecomputeDirty() {
  std::sort(dirty.begin(), dirty.end());
  dirty.erase(std::unique(dirty.begin(), dirty.end()), dirty.end());
  for (const uint32_t gid : dirty)
    distinct.Set(gid, CountDistinct(gid));
  dirty.clear();
}

void TpchQ16State::RecomputeAll() {
  const std::size_t groups = group_table->keys.size();
  std::vector<int64_t> counts(groups, 0);
  std::atomic<std::size_t> next{0};
  const auto worker = [&] {
    for (std::size_t g = next.fetch_add(64); g < groups; g = next.fetch_add(64))
      for (std::size_t i = g; i < std::min(groups, g + 64); ++i)
        counts[i] = CountDistinct(static_cast<uint32_t>(i));
  };
  const std::size_t threads = std::min<std::size_t>(
      std::max(1u, std::thread::hardware_concurrency()), 8);
  if (groups < 256 || threads <= 1) {
    worker();
  } else {
    std::vector<std::thread> pool;
    for (std::size_t t = 0; t < threads; ++t)
      pool.emplace_back(worker);
    for (auto &th : pool)
      th.join();
  }
  for (std::size_t g = 0; g < groups; ++g)
    if (counts[g] != 0)
      distinct.Set(static_cast<int64_t>(g), counts[g]);
  dirty.clear();
}

std::size_t TpchQ16State::MemoryBytes() const {
  std::size_t n = part_group.MemoryBytes() + suppliers_of.MemoryBytes() +
                  supplier_overflow.memory_bytes() + complaint.MemoryBytes() +
                  members.MemoryBytes() + distinct.MemoryBytes() +
                  brands->MemoryBytes() + types->MemoryBytes();
  for (std::size_t ci = 0; ci < members.NumChunks(); ++ci)
    if (const auto *chunk = members.ChunkAt(ci))
      for (const auto &m : *chunk)
        if (m)
          n += m->MemoryBytes();
  return n;
}

// ── TxnBuffer ───────────────────────────────────────────────────────────────
bool TpchQ16TxnBuffer::HasPendingChanges() const {
  return requires_rebuild || EventCount() != 0;
}
void TpchQ16TxnBuffer::Clear() {
  requires_rebuild = false;
  parts.clear();
  partsupps.clear();
  suppliers.clear();
}

// ── Module ──────────────────────────────────────────────────────────────────
core::AcceleratorId TpchQ16Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ16Module::Dependencies() const {
  std::vector<core::TableDependency> deps;
  deps.push_back({config.part_table, {0, 1, 2, 3}, {0, 3, 4, 5}});
  deps.push_back({config.partsupp_table, {0, 1}, {0, 1}});
  deps.push_back({config.supplier_table, {0, 1}, {0, 6}});
  return deps;
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ16Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ16TxnBuffer>();
}

void TpchQ16Module::Build(core::AuxEngineServices &services,
                          core::StateBuilder &builder) {
  auto image = std::make_shared<TpchQ16StateImage>();
  image->state = std::make_shared<const TpchQ16State>(BuildState(services));
  const std::shared_ptr<const TpchQ16StateImage> const_image = std::move(image);
  builder.Write(core::StateImage::Make(Id(), TpchQ16Layout::kState, 0,
                                       const_image));
  builder.Finish();
}

core::InvalidationPlan
TpchQ16Module::InterpretMutation(const core::MutationBatch &) const {
  // All three tables are maintained incrementally.
  return {};
}

void TpchQ16Module::AccumulateMutation(const core::MutationBatch &mutation,
                                       core::AuxEngineServices &,
                                       core::AuxTxnBuffer &txn_buffer) {
  auto &buffer = dynamic_cast<TpchQ16TxnBuffer &>(txn_buffer);
  // An UPDATE that touches none of the columns the query reads changes nothing.
  const auto touches = [&](std::initializer_list<core::ColumnId> watched) {
    if (mutation.kind != core::MutationKind::Update ||
        mutation.updated_columns.empty())
      return true;
    for (const auto col : mutation.updated_columns)
      for (const auto w : watched)
        if (col == w)
          return true;
    return false;
  };
  // Removals of a batch precede its insertions so an UPDATE that moves keys
  // around composes correctly.
  const auto each_image = [&](auto &&fn) {
    if (mutation.kind != core::MutationKind::Insert)
      for (const auto &row : mutation.old_rows)
        fn(row, int8_t{-1});
    if (mutation.kind != core::MutationKind::Delete)
      for (const auto &row : mutation.new_rows)
        fn(row, int8_t{+1});
  };

  if (mutation.table == config.part_table) {
    each_image([&](const core::AuxRow &row, int8_t sign) {
      TpchQ16PartEvent event;
      bool skip = false;
      if (!ReadPart(row, event.row, skip)) {
        buffer.MarkRebuild();
      } else if (!skip || sign < 0) {
        event.sign = sign;
        buffer.parts.push_back(std::move(event));
      }
    });
  } else if (mutation.table == config.partsupp_table) {
    if (!touches({0, 1}))
      return;
    each_image([&](const core::AuxRow &row, int8_t sign) {
      TpchQ16PartsuppEvent event;
      bool skip = false;
      if (!ReadPartsupp(row, event.row, skip)) {
        buffer.MarkRebuild();
      } else if (!skip) {
        event.sign = sign;
        buffer.partsupps.push_back(event);
      }
    });
  } else if (mutation.table == config.supplier_table) {
    if (!touches({0, 1}))
      return;
    each_image([&](const core::AuxRow &row, int8_t sign) {
      TpchQ16SupplierEvent event;
      if (!IntCell(row, 0, event.suppkey)) {
        buffer.MarkRebuild();
        return;
      }
      std::string comment;
      event.sign = sign;
      event.complaint = sign > 0 && StringCell(row, 1, comment) &&
                        IsComplaintComment(comment);
      buffer.suppliers.push_back(event);
    });
  }
}

core::StagedMaintenanceOutcome
TpchQ16Module::StageMaintenance(core::AuxEngineServices &services,
                                const core::InvalidationPlan &,
                                core::AuxTxnBuffer &txn_buffer) {
  auto &buffer = dynamic_cast<TpchQ16TxnBuffer &>(txn_buffer);
  core::StagedMaintenanceOutcome outcome;
  if (!buffer.HasPendingChanges())
    return outcome;

  bool need_rebuild = buffer.RequiresRebuild();
  if (!need_rebuild) {
    const auto old_state = LoadState(services);
    core::Version old_version = 0;
    if (!old_state) {
      buffer.Clear();
      return outcome;
    }
    const auto snapshot = services.StateStore().ReadPartitions(
        services.CurrentVisibility(), {TpchQ16Layout::kState});
    for (const auto &image : snapshot.partitions)
      if (image.partition == TpchQ16Layout::kState)
        old_version = image.version;

    // Copy-on-write: the copy shares every chunk / group with the published
    // state; edits clone only what they touch.
    auto new_state = std::make_shared<TpchQ16State>(*old_state);
    new_state->dirty.clear();
    for (const auto &ev : buffer.parts)
      ev.sign < 0 ? new_state->ErasePart(ev.row.partkey)
                  : new_state->SetPart(ev.row.partkey, ev.row);
    for (const auto &ev : buffer.partsupps) {
      if (ev.sign > 0) {
        new_state->AddSupplier(ev.row.partkey, ev.row.suppkey);
      } else if (!new_state->RemoveSupplier(ev.row.partkey, ev.row.suppkey)) {
        need_rebuild = true; // the state disagrees with the table
        break;
      }
    }
    if (!need_rebuild) {
      // Net complaint flips (an UPDATE of an unrelated column removes and
      // re-adds the same flag): only a real flip pays the pass over the lists.
      std::unordered_map<int64_t, uint8_t> original;
      for (const auto &ev : buffer.suppliers) {
        original.emplace(ev.suppkey, new_state->complaint.Get(ev.suppkey));
        new_state->SetComplaint(ev.suppkey, ev.sign > 0 && ev.complaint);
      }
      core::FlatHashMap<uint8_t> flipped;
      for (const auto &[suppkey, before] : original)
        if (new_state->complaint.Get(suppkey) != before)
          flipped.Upsert(suppkey, 1);
      if (!flipped.empty())
        new_state->MarkGroupsOfSuppliers(flipped);
      new_state->RecomputeDirty();

      const auto decision = services.ChooseMaintenance(
          {buffer.EventCount(), 1, true, false, true, false, false,
           core::MaintenanceAction::NoOp,
           services.ExpectedQueriesBeforeNextWrite()});
      if (decision.action == core::MaintenanceAction::ApplyDelta) {
        auto image = std::make_shared<TpchQ16StateImage>();
        image->state = std::move(new_state);
        const std::shared_ptr<const TpchQ16StateImage> const_image =
            std::move(image);
        outcome.action = core::MaintenanceAction::ApplyDelta;
        outcome.updates.push_back(core::StateImage::Make(
            Id(), TpchQ16Layout::kState, old_version, const_image));
        buffer.Clear();
        return outcome;
      }
      // Cost-model rebuild: fall through with empty updates.
    }
  }

  const auto decision = services.ChooseMaintenance(
      {0, 0, false, false, true, true, false, core::MaintenanceAction::NoOp,
       services.ExpectedQueriesBeforeNextWrite()});
  outcome.action = decision.action == core::MaintenanceAction::ApplyDelta
                       ? core::MaintenanceAction::RebuildAll
                       : decision.action;
  if (outcome.action == core::MaintenanceAction::RebuildPartition)
    outcome.action = core::MaintenanceAction::RebuildAll;
  outcome.invalidations = All();
  outcome.pending_rebuild = outcome.invalidations;
  buffer.Clear();
  return outcome;
}

bool TpchQ16Module::CheckValidity(core::AuxEngineServices &services,
                                  const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void TpchQ16Module::Execute(core::AuxEngineServices &services,
                            const core::QueryArguments &arguments,
                            core::AuxRowSink &sink) {
  if (!CheckValidity(services, arguments)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  const auto state = LoadState(services);
  if (!state) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  EmitResult(*state, arguments, sink);
}

void TpchQ16Module::ExecuteFallback(core::AuxEngineServices &services,
                                    const core::QueryArguments &arguments,
                                    core::AuxRowSink &sink) {
  const TpchQ16State state = BuildState(services);
  EmitResult(state, arguments, sink);
}

TpchQ16State TpchQ16Module::BuildState(core::AuxEngineServices &services) const {
  (void)services;
  if (!initial_scan)
    throw std::logic_error("TPC-H Q16 requires a typed initial scan source");
  TpchQ16State state;
  state.track_dirty = false;
  initial_scan->ScanSuppliers([&](const TpchQ16SupplierInput &input) {
    state.SetComplaint(input.suppkey, IsComplaintComment(input.comment));
  });
  initial_scan->ScanParts([&](const TpchQ16PartInput &input) {
    state.SetPart(input.partkey, input);
  });
  initial_scan->ScanPartsupp([&](const TpchQ16PartsuppInput &input) {
    state.AddSupplier(input.partkey, input.suppkey);
  });
  state.RecomputeAll();
  state.track_dirty = true;
  return state;
}

std::shared_ptr<const TpchQ16State>
TpchQ16Module::LoadState(core::AuxEngineServices &services) const {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {TpchQ16Layout::kState});
  for (const auto &image : snapshot.partitions) {
    if (image.partition != TpchQ16Layout::kState)
      continue;
    const auto *typed = image.As<TpchQ16StateImage>();
    return typed ? typed->state : nullptr;
  }
  return nullptr;
}

void TpchQ16Module::EmitResult(const TpchQ16State &state,
                               const core::QueryArguments &arguments,
                               core::AuxRowSink &sink) const {
  // arguments: brand, type pattern, then the p_size IN-list (up to 8 items).
  // SQL NULL (or a missing / mistyped value) is nullopt / a dropped item.
  TpchQ16Args args;
  if (arguments.values.size() >= 2) {
    const auto text = [&](std::size_t i) -> std::optional<std::string> {
      const auto &v = arguments.values[i];
      if (!v.is_null && std::holds_alternative<std::string>(v.payload))
        return std::get<std::string>(v.payload);
      return std::nullopt;
    };
    args.brand = text(0);
    args.type_pattern = text(1);
    for (std::size_t i = 2; i < arguments.values.size(); ++i) {
      const auto &v = arguments.values[i];
      if (v.is_null)
        continue;
      if (std::holds_alternative<int64_t>(v.payload)) {
        args.sizes.push_back(std::get<int64_t>(v.payload));
      } else if (std::holds_alternative<uint64_t>(v.payload)) {
        const auto u = std::get<uint64_t>(v.payload);
        if (u <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
          args.sizes.push_back(static_cast<int64_t>(u));
      } else if (std::holds_alternative<std::string>(v.payload)) {
        // Engines that pass every argument as text: "" is NULL.
        const auto &s = std::get<std::string>(v.payload);
        char *end = nullptr;
        const long long parsed = std::strtoll(s.c_str(), &end, 10);
        if (!s.empty() && *end == '\0')
          args.sizes.push_back(parsed);
      }
    }
  }
  TpchQ16FinalizeAndEmit(state, args, sink);
}

std::vector<core::CalibrationWrite> TpchQ16Module::CalibrationWrites() const {
  return TpchQ16CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
