#include "tpch/q11/postgres/aux/module.hpp"
#include "tpch/q11/calibration_writes.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>

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

// A decimal cell as an integer count of 1/100 units (ps_supplycost's scale);
// false when it is not exactly representable.
bool CentsCell(const core::AuxRow &row, std::size_t index, int64_t &raw) {
  if (index >= row.values.size() || row.values[index].is_null)
    return false;
  const auto &cell = row.values[index];
  if (!std::holds_alternative<core::ScaledDecimal>(cell.payload))
    return false;
  const auto decimal = std::get<core::ScaledDecimal>(cell.payload);
  int64_t value = decimal.value;
  for (int32_t scale = decimal.scale; scale < 2; ++scale)
    if (__builtin_mul_overflow(value, int64_t{10}, &value))
      return false;
  for (int32_t scale = decimal.scale; scale > 2; --scale) {
    if (value % 10 != 0)
      return false;
    value /= 10;
  }
  raw = value;
  return true;
}

core::InvalidationPlan All() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

// ── Row readers (projected as in Dependencies()) ────────────────────────────
// false: undecodable (forces a rebuild); `skip`: the row can never contribute
// (a NULL key / cost / quantity), so it is ignored.
bool ReadPartsupp(const core::AuxRow &row, int64_t &partkey, TpchQ11Offer &offer,
                  bool &skip) {
  skip = NullCell(row, 0) || NullCell(row, 1) || NullCell(row, 2) ||
         NullCell(row, 3);
  if (skip)
    return true;
  int64_t qty = 0;
  if (!IntCell(row, 0, partkey) || !IntCell(row, 1, offer.suppkey) ||
      !IntCell(row, 2, qty) || !CentsCell(row, 3, offer.cost_raw) ||
      qty < std::numeric_limits<int32_t>::min() ||
      qty > std::numeric_limits<int32_t>::max())
    return false;
  offer.availqty = static_cast<int32_t>(qty);
  return true;
}

bool ReadSupplier(const core::AuxRow &row, int64_t &suppkey,
                  int32_t &nationkey) {
  nationkey = kTpchQ11NoNation; // NULL s_nationkey joins no nation
  if (!IntCell(row, 0, suppkey))
    return false;
  if (NullCell(row, 1))
    return true;
  int64_t nk = 0;
  if (!IntCell(row, 1, nk) || nk <= std::numeric_limits<int32_t>::min() ||
      nk > std::numeric_limits<int32_t>::max())
    return false;
  nationkey = static_cast<int32_t>(nk);
  return true;
}

bool ReadNation(const core::AuxRow &row, TpchQ11NationInput &out, bool &skip) {
  int64_t nk = 0;
  skip = NullCell(row, 1); // n_name = <arg> is never true for NULL
  if (!IntCell(row, 0, nk) || nk < std::numeric_limits<int32_t>::min() ||
      nk > std::numeric_limits<int32_t>::max())
    return false;
  out.nationkey = static_cast<int32_t>(nk);
  return skip || StringCell(row, 1, out.name);
}

Q11AccT Product(const TpchQ11Offer &offer) {
  return static_cast<Q11AccT>(offer.cost_raw) *
         static_cast<Q11AccT>(offer.availqty);
}

} // namespace

// ── TpchQ11Group ────────────────────────────────────────────────────────────
void TpchQ11Group::Insert(int64_t partkey, Q11AccT value) {
  total += value;
  ++parts;
  if (value > kTpchQ11NarrowLimit)
    wide_positive.insert({value, partkey});
  else if (value < -kTpchQ11NarrowLimit)
    wide_negative.insert({value, partkey});
  else
    narrow.Insert({static_cast<int64_t>(value), partkey});
}

void TpchQ11Group::Erase(int64_t partkey, Q11AccT value) {
  total -= value;
  --parts;
  if (value > kTpchQ11NarrowLimit)
    wide_positive.erase({value, partkey});
  else if (value < -kTpchQ11NarrowLimit)
    wide_negative.erase({value, partkey});
  else
    narrow.Erase({static_cast<int64_t>(value), partkey});
}

// ── TpchQ11State ────────────────────────────────────────────────────────────
TpchQ11Offer TpchQ11State::OfferAt(const TpchQ11OfferSlots &slots,
                                   int64_t partkey, uint32_t i) const {
  if (i < kTpchQ11InlineOffers)
    return {slots.suppkey[i], slots.cost_raw[i], slots.availqty[i]};
  const auto *extra = offer_overflow.Find(partkey);
  return (*extra)[i - kTpchQ11InlineOffers];
}

TpchQ11PartValue TpchQ11State::PartValueIn(
    int64_t partkey, int32_t nationkey,
    const core::ChunkedCowArray<int32_t> &supplier_map) const {
  TpchQ11PartValue out;
  const auto *slots = offers.Find(partkey);
  if (!slots || nationkey == kTpchQ11NoNation)
    return out;
  for (uint32_t i = 0; i < slots->count; ++i) {
    const auto offer = OfferAt(*slots, partkey, i);
    if (supplier_map.Get(offer.suppkey) != nationkey)
      continue;
    out.present = true;
    out.value += Product(offer);
  }
  return out;
}

void TpchQ11State::AddOffer(int64_t partkey, const TpchQ11Offer &offer) {
  if (partkey < 0)
    return; // negative keys are never stored
  auto &slots = offers.Mutable(partkey);
  if (slots.count < kTpchQ11InlineOffers) {
    slots.suppkey[slots.count] = offer.suppkey;
    slots.cost_raw[slots.count] = offer.cost_raw;
    slots.availqty[slots.count] = offer.availqty;
  } else {
    offer_overflow[partkey].push_back(offer);
  }
  ++slots.count;
}

bool TpchQ11State::RemoveOffer(int64_t partkey, const TpchQ11Offer &offer) {
  if (partkey < 0)
    return true;
  const auto *found = offers.Find(partkey);
  if (!found || found->count == 0)
    return false;
  const uint32_t count = found->count;
  uint32_t hit = count;
  for (uint32_t i = 0; i < count; ++i) {
    const auto at = OfferAt(*found, partkey, i);
    if (at.suppkey == offer.suppkey && at.cost_raw == offer.cost_raw &&
        at.availqty == offer.availqty) {
      hit = i;
      break;
    }
  }
  if (hit == count)
    return false;
  const auto last = OfferAt(*found, partkey, count - 1);
  auto &slots = offers.Mutable(partkey); // `found` may dangle after this
  if (hit != count - 1) {
    if (hit < kTpchQ11InlineOffers) {
      slots.suppkey[hit] = last.suppkey;
      slots.cost_raw[hit] = last.cost_raw;
      slots.availqty[hit] = last.availqty;
    } else {
      offer_overflow[partkey][hit - kTpchQ11InlineOffers] = last;
    }
  }
  if (count > kTpchQ11InlineOffers) {
    auto &extra = offer_overflow[partkey];
    extra.pop_back();
    if (extra.empty())
      offer_overflow.Erase(partkey);
  }
  --slots.count;
  return true;
}

TpchQ11Group &TpchQ11State::MutableGroup(int32_t nationkey) {
  auto &group = groups[nationkey];
  if (!group)
    group = std::make_shared<TpchQ11Group>();
  else if (group.use_count() > 1) // shared with a published state
    group = std::make_shared<TpchQ11Group>(*group);
  return *group;
}

void TpchQ11State::UpdateEntry(int32_t nationkey, int64_t partkey,
                               const TpchQ11PartValue &before,
                               const TpchQ11PartValue &after) {
  if (before == after)
    return;
  auto &group = MutableGroup(nationkey);
  if (before.present)
    group.Erase(partkey, before.value);
  if (after.present)
    group.Insert(partkey, after.value);
}

bool TpchQ11State::ApplyPartsupp(int8_t sign, int64_t partkey,
                                 const TpchQ11Offer &offer) {
  const int32_t nationkey = supplier_nation.Get(offer.suppkey);
  const auto before = PartValueIn(partkey, nationkey, supplier_nation);
  if (sign > 0) {
    AddOffer(partkey, offer);
  } else if (!RemoveOffer(partkey, offer)) {
    return false;
  }
  if (nationkey != kTpchQ11NoNation && partkey >= 0)
    UpdateEntry(nationkey, partkey, before,
                PartValueIn(partkey, nationkey, supplier_nation));
  return true;
}

void TpchQ11State::SetSupplierNation(int64_t suppkey, int32_t nationkey) {
  if (suppkey < 0)
    return;
  if (nationkey == kTpchQ11NoNation && !supplier_nation.Has(suppkey, kTpchQ11NoNation))
    return; // already absent: do not allocate a chunk
  supplier_nation.Set(suppkey, nationkey);
}

void TpchQ11State::RegroupSuppliers(
    const core::FlatHashMap<uint8_t> &changed_suppliers,
    const core::ChunkedCowArray<int32_t> &old_map) {
  using Offers = core::ChunkedCowArray<TpchQ11OfferSlots, 10>;
  std::vector<int32_t> nations_of_part;
  for (std::size_t ci = 0; ci < offers.NumChunks(); ++ci) {
    const auto *chunk = offers.ChunkAt(ci);
    if (!chunk)
      continue;
    for (std::size_t slot = 0; slot < chunk->size(); ++slot) {
      const auto &slots = (*chunk)[slot];
      bool touched = false;
      for (uint32_t i = 0; i < slots.count && !touched; ++i)
        touched = changed_suppliers.Contains(
            OfferAt(slots, Offers::ChunkBase(ci) + static_cast<int64_t>(slot), i)
                .suppkey);
      if (!touched)
        continue;
      const int64_t partkey = Offers::ChunkBase(ci) + static_cast<int64_t>(slot);
      // Every nation this part had or has offers in may change.
      nations_of_part.clear();
      for (uint32_t i = 0; i < slots.count; ++i) {
        const auto offer = OfferAt(slots, partkey, i);
        for (const int32_t nk :
             {old_map.Get(offer.suppkey), supplier_nation.Get(offer.suppkey)})
          if (nk != kTpchQ11NoNation &&
              std::find(nations_of_part.begin(), nations_of_part.end(), nk) ==
                  nations_of_part.end())
            nations_of_part.push_back(nk);
      }
      for (const int32_t nk : nations_of_part)
        UpdateEntry(nk, partkey, PartValueIn(partkey, nk, old_map),
                    PartValueIn(partkey, nk, supplier_nation));
    }
  }
}

void TpchQ11State::UpsertNation(const TpchQ11NationInput &nation) {
  auto next = std::make_shared<std::vector<TpchQ11NationRec>>(*nations);
  for (auto &rec : *next)
    if (rec.key == nation.nationkey) {
      rec.name = nation.name;
      nations = std::move(next);
      return;
    }
  next->push_back({nation.nationkey, nation.name});
  nations = std::move(next);
}

void TpchQ11State::EraseNation(const TpchQ11NationInput &nation) {
  auto next = std::make_shared<std::vector<TpchQ11NationRec>>(*nations);
  for (auto it = next->begin(); it != next->end(); ++it)
    if (it->key == nation.nationkey) {
      next->erase(it);
      break;
    }
  nations = std::move(next);
}

void TpchQ11State::BuildGroups() {
  using Offers = core::ChunkedCowArray<TpchQ11OfferSlots, 10>;
  groups.clear();
  std::unordered_map<int32_t, std::vector<TpchQ11Key>> pending; // per nation
  std::vector<std::pair<int32_t, Q11AccT>> sums; // this part's per-nation values
  for (std::size_t ci = 0; ci < offers.NumChunks(); ++ci) {
    const auto *chunk = offers.ChunkAt(ci);
    if (!chunk)
      continue;
    for (std::size_t slot = 0; slot < chunk->size(); ++slot) {
      const auto &slots = (*chunk)[slot];
      if (slots.count == 0)
        continue;
      const int64_t partkey = Offers::ChunkBase(ci) + static_cast<int64_t>(slot);
      sums.clear();
      for (uint32_t i = 0; i < slots.count; ++i) {
        const auto offer = OfferAt(slots, partkey, i);
        const int32_t nk = supplier_nation.Get(offer.suppkey);
        if (nk == kTpchQ11NoNation)
          continue;
        auto it = std::find_if(sums.begin(), sums.end(),
                               [&](const auto &s) { return s.first == nk; });
        if (it == sums.end())
          sums.push_back({nk, Product(offer)});
        else
          it->second += Product(offer);
      }
      for (const auto &[nk, value] : sums) {
        auto &group = groups[nk];
        if (!group)
          group = std::make_shared<TpchQ11Group>();
        group->total += value;
        ++group->parts;
        if (value > kTpchQ11NarrowLimit)
          group->wide_positive.insert({value, partkey});
        else if (value < -kTpchQ11NarrowLimit)
          group->wide_negative.insert({value, partkey});
        else
          pending[nk].push_back({static_cast<int64_t>(value), partkey});
      }
    }
  }
  // Sort each nation's entries (nations in parallel) and cut them into blocks;
  // each vector is released as soon as its blocks exist.
  std::vector<int32_t> order;
  for (const auto &entry : pending)
    order.push_back(entry.first);
  std::atomic<std::size_t> next{0};
  const auto worker = [&] {
    for (std::size_t i = next.fetch_add(1); i < order.size();
         i = next.fetch_add(1)) {
      auto &keys = pending[order[i]];
      std::sort(keys.begin(), keys.end(), TpchQ11KeyLess());
      groups[order[i]]->narrow =
          core::CowSortedBlocks<TpchQ11Key, TpchQ11KeyLess>::FromSorted(
              std::move(keys));
    }
  };
  const std::size_t threads = std::min<std::size_t>(
      std::max(1u, std::thread::hardware_concurrency()),
      std::min<std::size_t>(order.size(), 8));
  if (threads <= 1) {
    worker();
  } else {
    std::vector<std::thread> pool;
    for (std::size_t t = 0; t < threads; ++t)
      pool.emplace_back(worker);
    for (auto &th : pool)
      th.join();
  }
}

std::size_t TpchQ11State::MemoryBytes() const {
  std::size_t n = supplier_nation.MemoryBytes() + offers.MemoryBytes() +
                  offer_overflow.memory_bytes();
  for (const auto &entry : groups)
    n += entry.second->narrow.MemoryBytes();
  return n;
}

// ── TxnBuffer ───────────────────────────────────────────────────────────────
bool TpchQ11TxnBuffer::HasPendingChanges() const {
  return requires_rebuild || EventCount() != 0;
}
void TpchQ11TxnBuffer::Clear() {
  requires_rebuild = false;
  partsupps.clear();
  suppliers.clear();
  nations.clear();
}

// ── Module ──────────────────────────────────────────────────────────────────
core::AcceleratorId TpchQ11Module::Id() const { return TpchQ11Layout::kAccelerator; }

std::vector<core::TableDependency> TpchQ11Module::Dependencies() const {
  return {{config.partsupp_table, {0, 1, 2, 3}, {0, 1, 2, 3}},
          {config.supplier_table, {0, 1}, {0, 3}},
          {config.nation_table, {0, 1}, {0, 1}}};
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ11Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ11TxnBuffer>();
}

void TpchQ11Module::Build(core::AuxEngineServices &services,
                          core::StateBuilder &builder) {
  auto state = std::make_shared<const TpchQ11State>(BuildState(services));
  auto image = std::make_shared<TpchQ11StateImage>();
  image->state = std::move(state);
  const std::shared_ptr<const TpchQ11StateImage> const_image = std::move(image);
  builder.Write(core::StateImage::Make(TpchQ11Layout::kAccelerator,
                                       TpchQ11Layout::kState, 0, const_image));
  builder.Finish();
}

core::InvalidationPlan
TpchQ11Module::InterpretMutation(const core::MutationBatch &) const {
  // All three tables are maintained incrementally.
  return {};
}

void TpchQ11Module::AccumulateMutation(const core::MutationBatch &mutation,
                                       core::AuxEngineServices &,
                                       core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ11TxnBuffer &>(base_buffer);
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

  if (mutation.table == config.partsupp_table) {
    each_image([&](const core::AuxRow &row, int8_t sign) {
      TpchQ11PartsuppEvent event;
      bool skip = false;
      if (!ReadPartsupp(row, event.partkey, event.offer, skip)) {
        buffer.MarkRebuild();
      } else if (!skip) {
        event.sign = sign;
        buffer.partsupps.push_back(event);
      }
    });
  } else if (mutation.table == config.supplier_table) {
    each_image([&](const core::AuxRow &row, int8_t sign) {
      TpchQ11SupplierEvent event;
      if (!ReadSupplier(row, event.suppkey, event.nationkey)) {
        buffer.MarkRebuild();
        return;
      }
      event.sign = sign;
      if (sign < 0)
        event.nationkey = kTpchQ11NoNation;
      buffer.suppliers.push_back(event);
    });
  } else if (mutation.table == config.nation_table) {
    each_image([&](const core::AuxRow &row, int8_t sign) {
      TpchQ11NationEvent event;
      bool skip = false;
      if (!ReadNation(row, event.row, skip)) {
        buffer.MarkRebuild();
      } else if (!skip || sign < 0) {
        event.sign = sign;
        buffer.nations.push_back(std::move(event));
      }
    });
  }
}

core::StagedMaintenanceOutcome
TpchQ11Module::StageMaintenance(core::AuxEngineServices &services,
                                const core::InvalidationPlan &,
                                core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ11TxnBuffer &>(base_buffer);
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
        services.CurrentVisibility(), {TpchQ11Layout::kState});
    for (const auto &image : snapshot.partitions)
      if (image.partition == TpchQ11Layout::kState)
        old_version = image.version;

    // Copy-on-write: the copy shares every chunk / block / group with the
    // published state; edits clone only what they touch.
    auto new_state = std::make_shared<TpchQ11State>(*old_state);
    for (const auto &ev : buffer.nations)
      ev.sign < 0 ? new_state->EraseNation(ev.row)
                  : new_state->UpsertNation(ev.row);
    // Partsupp changes first, against the supplier map as it was ...
    for (const auto &ev : buffer.partsupps)
      if (!new_state->ApplyPartsupp(ev.sign, ev.partkey, ev.offer)) {
        need_rebuild = true; // the state disagrees with the table
        break;
      }
    // ... then suppliers moving between nations re-group their parts.
    if (!need_rebuild && !buffer.suppliers.empty()) {
      const core::ChunkedCowArray<int32_t> old_map = new_state->supplier_nation;
      for (const auto &ev : buffer.suppliers)
        new_state->SetSupplierNation(ev.suppkey, ev.nationkey);
      core::FlatHashMap<uint8_t> changed;
      for (const auto &ev : buffer.suppliers)
        if (old_map.Get(ev.suppkey) != new_state->supplier_nation.Get(ev.suppkey))
          changed.Upsert(ev.suppkey, 1);
      if (!changed.empty())
        new_state->RegroupSuppliers(changed, old_map);
    }

    if (!need_rebuild) {
      const auto decision = services.ChooseMaintenance(
          {buffer.EventCount(), 1, true, false, true, false, false,
           core::MaintenanceAction::NoOp,
           services.ExpectedQueriesBeforeNextWrite()});
      if (decision.action == core::MaintenanceAction::ApplyDelta) {
        auto image = std::make_shared<TpchQ11StateImage>();
        image->state = std::move(new_state);
        const std::shared_ptr<const TpchQ11StateImage> const_image =
            std::move(image);
        outcome.action = core::MaintenanceAction::ApplyDelta;
        outcome.updates.push_back(core::StateImage::Make(
            TpchQ11Layout::kAccelerator, TpchQ11Layout::kState, old_version,
            const_image));
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

bool TpchQ11Module::CheckValidity(core::AuxEngineServices &services,
                                  const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void TpchQ11Module::Execute(core::AuxEngineServices &services,
                            const core::QueryArguments &arguments,
                            core::AuxRowSink &sink) {
  if (!services.StateStore()
           .CheckValidity({Id(), {}}, services.CurrentVisibility())
           .IsValid()) {
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

void TpchQ11Module::ExecuteFallback(core::AuxEngineServices &services,
                                    const core::QueryArguments &arguments,
                                    core::AuxRowSink &sink) {
  const TpchQ11State state = BuildState(services);
  EmitResult(state, arguments, sink);
}

TpchQ11State TpchQ11Module::BuildState(core::AuxEngineServices &services) const {
  (void)services;
  if (!initial_scan)
    throw std::logic_error("TPC-H Q11 requires a typed initial scan source");
  TpchQ11State state;
  initial_scan->ScanNations(
      [&](const TpchQ11NationInput &input) { state.UpsertNation(input); });
  initial_scan->ScanSuppliers([&](const TpchQ11SupplierInput &input) {
    state.SetSupplierNation(input.suppkey, input.nationkey);
  });
  initial_scan->ScanPartsupp([&](const TpchQ11PartsuppInput &input) {
    state.AddOffer(input.partkey,
                   {input.suppkey, input.supplycost_raw,
                    static_cast<int32_t>(input.availqty)});
  });
  state.BuildGroups();
  return state;
}

std::shared_ptr<const TpchQ11State>
TpchQ11Module::LoadState(core::AuxEngineServices &services) const {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {TpchQ11Layout::kState});
  for (const auto &image : snapshot.partitions) {
    if (image.partition != TpchQ11Layout::kState)
      continue;
    const auto *typed = image.As<TpchQ11StateImage>();
    return typed ? typed->state : nullptr;
  }
  return nullptr;
}

void TpchQ11Module::EmitResult(const TpchQ11State &state,
                               const core::QueryArguments &arguments,
                               core::AuxRowSink &sink) const {
  // A NULL (or missing / mistyped) argument makes the predicate or HAVING
  // comparison unknown: an empty result.
  TpchQ11Args args;
  if (arguments.values.size() >= 2) {
    const auto &nation = arguments.values[0];
    if (!nation.is_null && std::holds_alternative<std::string>(nation.payload))
      args.nation = std::get<std::string>(nation.payload);
    const auto &frac = arguments.values[1];
    if (!frac.is_null) {
      TpchQ11Fraction parsed;
      bool ok = false;
      if (std::holds_alternative<std::string>(frac.payload)) {
        ok = TpchQ11ParseFraction(std::get<std::string>(frac.payload), parsed);
      } else if (std::holds_alternative<double>(frac.payload)) {
        // A float8 argument: its shortest round-trip decimal is the value the
        // caller typed (the exact binary expansion would not be).
        char buf[64];
        const auto r = std::to_chars(buf, buf + sizeof(buf),
                                     std::get<double>(frac.payload));
        ok = r.ec == std::errc() &&
             TpchQ11ParseFraction(std::string_view(buf, r.ptr - buf), parsed);
      } else if (std::holds_alternative<int64_t>(frac.payload)) {
        parsed.coeff = std::get<int64_t>(frac.payload);
        ok = true;
      }
      if (ok)
        args.fraction = parsed;
    }
  }
  TpchQ11FinalizeAndEmit(state, args, sink);
}

std::vector<core::CalibrationWrite> TpchQ11Module::CalibrationWrites() const {
  return TpchQ11CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
