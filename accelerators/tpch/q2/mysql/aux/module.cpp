#include "tpch/q2/mysql/aux/module.hpp"
#include "tpch/q2/calibration_writes.hpp"
#include "substrate/aux/core/aux_cells.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/partitioned_build.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>
#include "tpch/q2/layout.hpp"

namespace accelerator_codegen::aux_state::query {
namespace {

bool StringValue(const core::AuxValue &value, std::string &out) {
  if (value.is_null || !std::holds_alternative<std::string>(value.payload))
    return false;
  out = std::get<std::string>(value.payload);
  return true;
}

// A decimal cell as an integer count of 1/100 units (the scale of s_acctbal
// and ps_supplycost); false when it is not exactly representable.
bool NullCell(const core::AuxRow &row, std::size_t index) {
  return index >= row.values.size() || row.values[index].is_null;
}

// Row decoders for captured mutation rows (projected as in Dependencies()).
// Return false when the row cannot be decoded; `skip` is set when the row can
// never take part in the query (a NULL join/filter column), so it is ignored.
bool ReadPart(const core::AuxRow &row, TpchQ2PartInput &out, bool &skip) {
  skip = false;
  int64_t key = 0, size = 0;
  if (!core::IntCell(row, 0, key))
    return false;
  out.key = key;
  if (NullCell(row, 2) || NullCell(row, 3)) {
    skip = true; // NULL p_type / p_size never satisfies the predicates
    return true;
  }
  if (!core::IntCell(row, 3, size) || !StringValue(row.values[2], out.type) ||
      size < std::numeric_limits<int32_t>::min() ||
      size > std::numeric_limits<int32_t>::max())
    return false;
  out.size = static_cast<int32_t>(size);
  out.mfgr.clear();
  if (!NullCell(row, 1) && !StringValue(row.values[1], out.mfgr))
    return false;
  return true;
}

bool ReadSupplier(const core::AuxRow &row, TpchQ2SupplierInput &out,
                  bool &skip) {
  skip = false;
  int64_t key = 0, nationkey = 0;
  if (!core::IntCell(row, 0, key))
    return false;
  out.key = key;
  if (NullCell(row, 3)) {
    skip = true; // no nation: the supplier joins nothing
    return true;
  }
  if (!core::IntCell(row, 3, nationkey))
    return false;
  out.nationkey = nationkey;
  for (const std::size_t col : {1, 2, 4, 6}) {
    std::string &dst = col == 1 ? out.name
                       : col == 2 ? out.address
                       : col == 4 ? out.phone
                                  : out.comment;
    dst.clear();
    if (!NullCell(row, col) && !StringValue(row.values[col], dst))
      return false;
  }
  out.acctbal_raw = 0;
  if (!NullCell(row, 5) && !core::ExactDecimalCell(row, 5, 2, out.acctbal_raw))
    return false;
  return true;
}

bool ReadPartsupp(const core::AuxRow &row, TpchQ2PartsuppInput &out,
                  bool &skip) {
  skip = NullCell(row, 0) || NullCell(row, 1) || NullCell(row, 2);
  if (skip)
    return true; // a NULL key or cost never matches the join / the minimum
  return core::IntCell(row, 0, out.partkey) && core::IntCell(row, 1, out.suppkey) &&
         core::ExactDecimalCell(row, 2, 2, out.supplycost_raw);
}

bool ReadNation(const core::AuxRow &row, TpchQ2NationInput &out, bool &skip) {
  skip = false;
  int64_t key = 0, region = 0;
  if (!core::IntCell(row, 0, key))
    return false;
  out.key = key;
  out.name.clear();
  if (!NullCell(row, 1) && !StringValue(row.values[1], out.name))
    return false;
  if (NullCell(row, 2)) {
    out.regionkey = -1; // joins no region
    out.regionkey_null = true;
    return true;
  }
  if (!core::IntCell(row, 2, region))
    return false;
  out.regionkey = region;
  return true;
}

bool ReadRegion(const core::AuxRow &row, TpchQ2RegionInput &out, bool &skip) {
  skip = false;
  int64_t key = 0;
  if (!core::IntCell(row, 0, key))
    return false;
  out.key = key;
  if (NullCell(row, 1)) {
    skip = true; // r_name = <arg> is never true
    return true;
  }
  return StringValue(row.values[1], out.name);
}

struct PublishedState {
  std::shared_ptr<const TpchQ2State> state;
  core::Version version = 0;
};

// The published full image, or an empty state when it is absent or foreign.
PublishedState LoadPublished(core::AuxEngineServices &services) {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {core::TpchQ2Layout::kFull});
  for (const auto &image : snapshot.partitions) {
    if (image.partition != core::TpchQ2Layout::kFull)
      continue;
    const auto *typed = image.As<TpchQ2StateImage>();
    if (!typed || !typed->state)
      return {};
    return {typed->state, image.version};
  }
  return {};
}

// Applies the buffered events to `state`; false when the state disagrees with
// the table (a deleted offer that was never seen).
bool ApplyEvents(TpchQ2State &state, const TpchQ2TxnBuffer &buffer) {
  for (const auto &ev : buffer.regions)
    ev.sign < 0 ? state.EraseRegion(ev.row) : state.UpsertRegion(ev.row);
  for (const auto &ev : buffer.nations)
    ev.sign < 0 ? state.EraseNation(ev.row) : state.UpsertNation(ev.row);
  for (const auto &ev : buffer.suppliers)
    ev.sign < 0 ? state.EraseSupplier(ev.row.key) : state.SetSupplier(ev.row.key, ev.row);
  for (const auto &ev : buffer.parts)
    ev.sign < 0 ? state.ErasePart(ev.row.key) : state.SetPart(ev.row.key, ev.row);
  for (const auto &ev : buffer.partsupps) {
    if (ev.sign > 0)
      state.AddOffer(ev.row.partkey, ev.row.suppkey, ev.row.supplycost_raw);
    else if (!state.RemoveOffer(ev.row.partkey, ev.row.suppkey, ev.row.supplycost_raw))
      return false;
  }
  return true;
}

// Applies the events to a copy-on-write copy of `published` (it shares every
// chunk and each point edit clones only the chunk it touches). Fills `outcome`
// and returns true when the cost model keeps the delta; false when the copy is
// inconsistent or a rebuild is chosen.
bool TryApplyEvents(core::AuxEngineServices &services, const TpchQ2TxnBuffer &buffer,
                    const PublishedState &published, core::StagedMaintenanceOutcome &outcome) {
  auto next = std::make_shared<TpchQ2State>(*published.state);
  if (!ApplyEvents(*next, buffer))
    return false;
  const auto decision = services.ChooseMaintenance(
      core::MaintenanceRequest::IncrementalSingleState(
          buffer.EventCount(), services.ExpectedQueriesBeforeNextWrite()));
  if (decision.action != core::MaintenanceAction::ApplyDelta)
    return false;
  auto image = std::make_shared<TpchQ2StateImage>();
  image->state = std::move(next);
  outcome.action = core::MaintenanceAction::ApplyDelta;
  outcome.updates.push_back(core::StateImage::Make(
      core::TpchQ2Layout::kAccelerator, core::TpchQ2Layout::kFull, published.version,
      std::shared_ptr<const TpchQ2StateImage>(std::move(image))));
  return true;
}

} // namespace

// ── supplier text packing ────────────────────────────────────────────────────

std::string TpchQ2PackSupplierText(const std::string &name,
                                   const std::string &address,
                                   const std::string &phone,
                                   const std::string &comment) {
  const uint32_t lens[3] = {static_cast<uint32_t>(name.size()),
                            static_cast<uint32_t>(address.size()),
                            static_cast<uint32_t>(phone.size())};
  std::string packed(reinterpret_cast<const char *>(lens), sizeof(lens));
  packed.reserve(sizeof(lens) + name.size() + address.size() + phone.size() +
                 comment.size());
  packed += name;
  packed += address;
  packed += phone;
  packed += comment;
  return packed;
}

TpchQ2SupplierText TpchQ2UnpackSupplierText(std::string_view packed) {
  uint32_t lens[3];
  std::memcpy(lens, packed.data(), sizeof(lens));
  const char *at = packed.data() + sizeof(lens);
  TpchQ2SupplierText text;
  text.name = std::string_view(at, lens[0]);
  at += lens[0];
  text.address = std::string_view(at, lens[1]);
  at += lens[1];
  text.phone = std::string_view(at, lens[2]);
  at += lens[2];
  text.comment = std::string_view(
      at, static_cast<std::size_t>(packed.data() + packed.size() - at));
  return text;
}

// ── TpchQ2State point edits ──────────────────────────────────────────────────

void TpchQ2State::InternInto(std::shared_ptr<core::StringDict> &dict,
                             const std::string &value, uint32_t &id) {
  id = dict->Find(value);
  if (id != core::StringDict::kNone)
    return;
  if (dict.use_count() > 1) // shared with a published state: clone first
    dict = std::make_shared<core::StringDict>(*dict);
  id = dict->Intern(value);
}

void TpchQ2State::SetPart(int64_t partkey, const TpchQ2PartInput &part) {
  uint32_t type_id = 0, mfgr_id = 0;
  InternInto(types, part.type, type_id);
  InternInto(mfgrs, part.mfgr, mfgr_id);
  part_size.Set(partkey, part.size);
  part_type.Set(partkey, type_id);
  part_mfgr.Set(partkey, mfgr_id);
}

void TpchQ2State::ErasePart(int64_t partkey) {
  if (!part_type.Has(partkey, kTpchQ2NoId))
    return;
  part_size.Set(partkey, kTpchQ2NoSize);
  part_type.Set(partkey, kTpchQ2NoId);
  part_mfgr.Set(partkey, kTpchQ2NoId);
}

void TpchQ2State::SetSupplier(int64_t suppkey,
                              const TpchQ2SupplierInput &supplier) {
  TpchQ2SupplierRec rec;
  rec.acctbal_raw = supplier.acctbal_raw;
  rec.nationkey = supplier.nationkey;
  rec.present = 1;
  suppliers.Set(suppkey, rec);
  supplier_text.Set(suppkey,
                    TpchQ2PackSupplierText(supplier.name, supplier.address,
                                           supplier.phone, supplier.comment));
}

void TpchQ2State::EraseSupplier(int64_t suppkey) {
  const auto *rec = suppliers.Find(suppkey);
  if (!rec || !rec->present)
    return;
  suppliers.Set(suppkey, TpchQ2SupplierRec{});
  supplier_text.Erase(suppkey);
}

TpchQ2Offer TpchQ2State::OfferAt(const TpchQ2OfferSlots &slots,
                                 int64_t partkey, uint32_t i) const {
  if (i < kTpchQ2InlineOffers)
    return {slots.suppkey[i], slots.cost_raw[i]};
  const auto *extra = offer_overflow.Find(partkey);
  return (*extra)[i - kTpchQ2InlineOffers];
}

void TpchQ2State::AddOffer(int64_t partkey, int64_t suppkey,
                           int64_t cost_raw) {
  auto &slots = offers.Mutable(partkey);
  if (slots.count < kTpchQ2InlineOffers) {
    slots.suppkey[slots.count] = suppkey;
    slots.cost_raw[slots.count] = cost_raw;
  } else {
    offer_overflow[partkey].push_back({suppkey, cost_raw});
  }
  ++slots.count;
}

bool TpchQ2State::RemoveOffer(int64_t partkey, int64_t suppkey,
                              int64_t cost_raw) {
  const auto *found = offers.Find(partkey);
  if (!found || found->count == 0)
    return false;
  const uint32_t count = found->count;
  uint32_t hit = count;
  for (uint32_t i = 0; i < count; ++i) {
    const auto offer = OfferAt(*found, partkey, i);
    if (offer.suppkey == suppkey && offer.cost_raw == cost_raw) {
      hit = i;
      break;
    }
  }
  if (hit == count)
    return false;
  const auto last = OfferAt(*found, partkey, count - 1);
  auto &slots = offers.Mutable(partkey); // `found` may dangle after this
  if (hit != count - 1) {
    if (hit < kTpchQ2InlineOffers) {
      slots.suppkey[hit] = last.suppkey;
      slots.cost_raw[hit] = last.cost_raw;
    } else {
      offer_overflow[partkey][hit - kTpchQ2InlineOffers] = last;
    }
  }
  if (count > kTpchQ2InlineOffers) {
    auto &extra = offer_overflow[partkey];
    extra.pop_back();
    if (extra.empty())
      offer_overflow.Erase(partkey);
  }
  --slots.count;
  return true;
}

void TpchQ2State::UpsertNation(const TpchQ2NationInput &nation) {
  auto next = std::make_shared<std::vector<TpchQ2NationRec>>(*nations);
  const int64_t regionkey =
      nation.regionkey_null ? std::numeric_limits<int64_t>::min()
                            : nation.regionkey;
  for (auto &rec : *next)
    if (rec.key == nation.key) {
      rec = {nation.key, nation.name, regionkey};
      nations = std::move(next);
      return;
    }
  next->push_back({nation.key, nation.name, regionkey});
  nations = std::move(next);
}

void TpchQ2State::EraseNation(const TpchQ2NationInput &nation) {
  auto next = std::make_shared<std::vector<TpchQ2NationRec>>(*nations);
  for (auto it = next->begin(); it != next->end(); ++it)
    if (it->key == nation.key) {
      next->erase(it);
      break;
    }
  nations = std::move(next);
}

void TpchQ2State::UpsertRegion(const TpchQ2RegionInput &region) {
  auto next = std::make_shared<std::vector<TpchQ2RegionRec>>(*regions);
  for (auto &rec : *next)
    if (rec.key == region.key) {
      rec = {region.key, region.name};
      regions = std::move(next);
      return;
    }
  next->push_back({region.key, region.name});
  regions = std::move(next);
}

void TpchQ2State::EraseRegion(const TpchQ2RegionInput &region) {
  auto next = std::make_shared<std::vector<TpchQ2RegionRec>>(*regions);
  for (auto it = next->begin(); it != next->end(); ++it)
    if (it->key == region.key) {
      next->erase(it);
      break;
    }
  regions = std::move(next);
}

std::size_t TpchQ2State::MemoryBytes() const {
  return part_size.MemoryBytes() + part_type.MemoryBytes() +
         part_mfgr.MemoryBytes() + offers.MemoryBytes() +
         offer_overflow.memory_bytes() + suppliers.MemoryBytes() +
         supplier_text.MemoryBytes() + types->MemoryBytes() +
         mfgrs->MemoryBytes();
}

// ── transaction buffer ───────────────────────────────────────────────────────

bool TpchQ2TxnBuffer::HasPendingChanges() const {
  return requires_rebuild || EventCount() != 0;
}
void TpchQ2TxnBuffer::Clear() {
  requires_rebuild = false;
  parts.clear();
  suppliers.clear();
  partsupps.clear();
  nations.clear();
  regions.clear();
}
void TpchQ2TxnBuffer::MarkRebuild() { requires_rebuild = true; }

// ── module ───────────────────────────────────────────────────────────────────

core::AcceleratorId TpchQ2Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ2Module::Dependencies() const {
  return {{config.part_table, {0, 1, 2, 3}, {0, 2, 4, 5}},
          {config.supplier_table, {0, 1, 2, 3, 4, 5, 6}, {0, 1, 2, 3, 4, 5, 6}},
          {config.partsupp_table, {0, 1, 2}, {0, 1, 3}},
          {config.nation_table, {0, 1, 2}, {0, 1, 2}},
          {config.region_table, {0, 1}, {0, 1}}};
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ2Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ2TxnBuffer>();
}

void TpchQ2Module::Build(core::AuxEngineServices &services,
                         core::StateBuilder &builder) {
  auto state = BuildState(services);
  const auto typed_state =
      std::make_shared<const TpchQ2State>(std::move(state));
  builder.Write(core::StateImage::Make(
      core::TpchQ2Layout::kAccelerator, core::TpchQ2Layout::kFull, 0,
      std::make_shared<const TpchQ2StateImage>(TpchQ2StateImage{typed_state})));
  builder.Finish();
}

core::InvalidationPlan
TpchQ2Module::InterpretMutation(const core::MutationBatch &) const {
  // Every table is maintained incrementally (point edits of the columnar
  // state); nothing needs invalidation up front.
  return {};
}

void TpchQ2Module::AccumulateMutation(const core::MutationBatch &mutation,
                                      core::AuxEngineServices &,
                                      core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ2TxnBuffer &>(base_buffer);

  // Removals of a batch are applied before its insertions, so an UPDATE that
  // moves keys around (swaps, shifts) composes correctly.
  const auto add = [&](auto &events, auto read, const core::AuxRow &row,
                       int8_t sign) {
    typename std::decay_t<decltype(events)>::value_type event;
    bool skip = false;
    if (!read(row, event.row, skip)) {
      buffer.MarkRebuild();
      return;
    }
    if (skip && sign > 0)
      return; // a new image that can never qualify: nothing to add
    event.sign = sign;
    events.push_back(std::move(event));
  };
  const auto apply = [&](auto &events, auto read) {
    if (mutation.kind != core::MutationKind::Insert)
      for (const auto &row : mutation.old_rows)
        add(events, read, row, -1);
    if (mutation.kind != core::MutationKind::Delete)
      for (const auto &row : mutation.new_rows)
        add(events, read, row, +1);
  };

  if (mutation.table == config.part_table) {
    apply(buffer.parts, ReadPart);
  } else if (mutation.table == config.supplier_table) {
    apply(buffer.suppliers, ReadSupplier);
  } else if (mutation.table == config.partsupp_table) {
    // A NULL-cost / NULL-key partsupp row was never stored: skip both images.
    const auto add_offer = [&](const core::AuxRow &row, int8_t sign) {
      TpchQ2PartsuppEvent event;
      bool skip = false;
      if (!ReadPartsupp(row, event.row, skip)) {
        buffer.MarkRebuild();
        return;
      }
      if (skip)
        return;
      event.sign = sign;
      buffer.partsupps.push_back(std::move(event));
    };
    if (mutation.kind != core::MutationKind::Insert)
      for (const auto &row : mutation.old_rows)
        add_offer(row, -1);
    if (mutation.kind != core::MutationKind::Delete)
      for (const auto &row : mutation.new_rows)
        add_offer(row, +1);
  } else if (mutation.table == config.nation_table) {
    apply(buffer.nations, ReadNation);
  } else if (mutation.table == config.region_table) {
    apply(buffer.regions, ReadRegion);
  }
}

core::StagedMaintenanceOutcome
TpchQ2Module::StageMaintenance(core::AuxEngineServices &services,
                               const core::InvalidationPlan &,
                               core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ2TxnBuffer &>(base_buffer);
  core::StagedMaintenanceOutcome outcome;
  if (!buffer.HasPendingChanges())
    return outcome;
  if (!buffer.RequiresRebuild()) {
    const auto published = LoadPublished(services);
    if (!published.state) {
      buffer.Clear();
      return outcome;
    }
    if (TryApplyEvents(services, buffer, published, outcome)) {
      buffer.Clear();
      return outcome;
    }
    // A rebuild chosen by the cost model leaves the updates empty, so PreCommit
    // MaterializeSyncRebuild can run under an active snapshot.
  }
  core::FinishRebuildAll(
      outcome, services.ChooseMaintenance(core::MaintenanceRequest::RebuildOnly(
                   services.ExpectedQueriesBeforeNextWrite())));
  buffer.Clear();
  return outcome;
}

bool TpchQ2Module::CheckValidity(core::AuxEngineServices &services,
                                 const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void TpchQ2Module::Execute(core::AuxEngineServices &services,
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
  EmitResult(services, *state, arguments, sink);
}

TpchQ2State
TpchQ2Module::BuildState(core::AuxEngineServices &services) const {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q2 requires a typed initial scan source");
  TpchQ2State state;
  // The five tables are independent in the state, so scan order is free and
  // each row is one point edit: no intermediate copies of any table.
  initial_scan->ScanRegionsSerial(
      [&](const TpchQ2RegionInput &input) { state.UpsertRegion(input); });
  initial_scan->ScanNationsSerial(
      [&](const TpchQ2NationInput &input) { state.UpsertNation(input); });
  // The scans run in parallel; the rows are applied in partition order.
  for (const auto &input : core::CollectRows<TpchQ2PartInput>(
           services, [&](std::size_t parts, auto &&sink) {
             initial_scan->ScanParts(services, parts, sink);
           }))
    state.SetPart(input.key, input);
  for (const auto &input : core::CollectRows<TpchQ2SupplierInput>(
           services, [&](std::size_t parts, auto &&sink) {
             initial_scan->ScanSuppliers(services, parts, sink);
           }))
    state.SetSupplier(input.key, input);
  for (const auto &input : core::CollectRows<TpchQ2PartsuppInput>(
           services, [&](std::size_t parts, auto &&sink) {
             initial_scan->ScanPartsupps(services, parts, sink);
           }))
    state.AddOffer(input.partkey, input.suppkey, input.supplycost_raw);
  return state;
}

std::shared_ptr<const TpchQ2State>
TpchQ2Module::LoadState(core::AuxEngineServices &services) const {
  return LoadPublished(services).state;
}

void TpchQ2Module::EmitResult(const core::AuxEngineServices &services,
                              const TpchQ2State &state,
                              const core::QueryArguments &arguments,
                              core::AuxRowSink &sink) const {
  // SQL NULL (or a missing / mistyped argument) makes its predicate unknown,
  // hence an empty result.
  TpchQ2Args args;
  if (arguments.values.size() == 3) {
    const auto &size = arguments.values[0];
    if (!size.is_null) {
      if (std::holds_alternative<int64_t>(size.payload))
        args.size = std::get<int64_t>(size.payload);
      else if (std::holds_alternative<uint64_t>(size.payload) &&
               std::get<uint64_t>(size.payload) <=
                   static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        args.size = static_cast<int64_t>(std::get<uint64_t>(size.payload));
    }
    std::string text;
    if (StringValue(arguments.values[1], text))
      args.type_pattern = std::move(text);
    if (StringValue(arguments.values[2], text))
      args.region_name = std::move(text);
  }
  TpchQ2FinalizeAndEmit(services, state, args, sink);
}

std::vector<core::CalibrationWrite> TpchQ2Module::CalibrationWrites() const {
  return TpchQ2CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
