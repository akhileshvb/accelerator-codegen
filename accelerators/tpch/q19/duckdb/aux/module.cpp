#include "tpch/q19/duckdb/aux/module.hpp"
#include "tpch/q19/calibration_writes.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/partitioned_build.hpp"
#include "tpch/common/typed_row_batch.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {

using Vec = TpchQ19State::Vec;

// The single state image's partition id.
constexpr core::PartitionId kStatePartition = 0;
// Lineitem deltas per maintenance slice (~70 ns each: >= 250 us a slice).
constexpr std::size_t kDeltaGrain = 4096;

// ── Cell extraction helpers ─────────────────────────────────────────────────
bool StringCell(const core::AuxRow &row, std::size_t index, std::string &out) {
  if (index >= row.values.size() || row.values[index].is_null ||
      !std::holds_alternative<std::string>(row.values[index].payload))
    return false;
  out = std::get<std::string>(row.values[index].payload);
  return true;
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

// A scale-2 decimal cell's unscaled value.
bool Decimal2Cell(const core::AuxRow &row, std::size_t index, int64_t &raw) {
  if (index >= row.values.size() || row.values[index].is_null)
    return false;
  const auto *decimal =
      std::get_if<core::ScaledDecimal>(&row.values[index].payload);
  if (!decimal || decimal->scale != 2)
    return false;
  raw = decimal->value;
  return true;
}

bool ReadPartRow(const core::AuxRow &row, TpchQ19PartInput &out) {
  int64_t partkey = 0, size = 0;
  std::string brand, container;
  if (!IntCell(row, 0, partkey) || !StringCell(row, 1, brand) ||
      !StringCell(row, 2, container) || !IntCell(row, 3, size))
    return false;
  // A size outside int32 can never be BETWEEN the family bounds.
  out.key = partkey;
  out.brand = std::move(brand);
  out.container = std::move(container);
  out.size = static_cast<int32_t>(
      std::clamp<int64_t>(size, std::numeric_limits<int32_t>::min(),
                          std::numeric_limits<int32_t>::max()));
  return true;
}

// Reads a lineitem row; `qualifies` is the shipping predicate (rows that
// cannot match contribute nothing and are dropped by the caller).
bool ReadLineitemRow(const core::AuxRow &row, TpchQ19LineitemDelta &out,
                     bool &qualifies) {
  int64_t partkey = 0, quantity_raw = 0, extendedprice_raw = 0, discount_raw = 0;
  std::string shipmode, shipinstruct;
  if (!IntCell(row, 0, partkey) || !Decimal2Cell(row, 1, quantity_raw) ||
      !Decimal2Cell(row, 2, extendedprice_raw) ||
      !Decimal2Cell(row, 3, discount_raw) || !StringCell(row, 4, shipmode) ||
      !StringCell(row, 5, shipinstruct))
    return false;
  qualifies = (shipmode == "AIR" || shipmode == "AIR REG") &&
              shipinstruct == "DELIVER IN PERSON";
  out.partkey = partkey;
  out.quantity_raw = quantity_raw;
  // l_extendedprice * (1 - l_discount) at scale 4.
  out.revenue_raw = static_cast<__int128>(extendedprice_raw) * (100 - discount_raw);
  return true;
}

// ── Q19 family classification ───────────────────────────────────────────────
TpchQ19Family ClassifyContainerFamily(std::string_view container) {
  for (const char *c : {"SM CASE", "SM BOX", "SM PACK", "SM PKG"})
    if (container == c)
      return TpchQ19Family::SM;
  for (const char *c : {"MED BAG", "MED BOX", "MED PKG", "MED PACK"})
    if (container == c)
      return TpchQ19Family::MED;
  for (const char *c : {"LG CASE", "LG BOX", "LG PACK", "LG PKG"})
    if (container == c)
      return TpchQ19Family::LG;
  return TpchQ19Family::None;
}

int32_t BranchSizeLimit(TpchQ19Family family) {
  switch (family) {
  case TpchQ19Family::SM: return 5;
  case TpchQ19Family::MED: return 10;
  case TpchQ19Family::LG: return 15;
  default: return 0;
  }
}

// The branch a part can qualify for (None: it never does).
TpchQ19Family PartFamily(const std::string &container, int32_t size) {
  const auto family = ClassifyContainerFamily(container);
  return size >= 1 && size <= BranchSizeLimit(family) ? family
                                                      : TpchQ19Family::None;
}

std::shared_ptr<const TpchQ19StateImage>
MakeStateImage(std::shared_ptr<const TpchQ19State> state) {
  auto image = std::make_shared<TpchQ19StateImage>();
  image->state = std::move(state);
  return image;
}

const TpchQ19StateImage *AsQ19Image(const core::StateImage &image) {
  return image.As<TpchQ19StateImage>();
}

core::InvalidationPlan All() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

// The part codes one build partition has seen. Brand ids are local until the
// partitions fold in table order, so the dictionary ends up in order of first
// appearance, as in a serial scan.
struct PartCodes {
  core::StringDict brands;
  core::AnyKeyCowArray<uint32_t, 14> codes{0};

  void Add(const TpchQ19PartInput &part) {
    const auto family = PartFamily(part.container, part.size);
    if (family != TpchQ19Family::None)
      codes.Set(part.key, TpchQ19State::Code(brands.Intern(part.brand), family));
  }
  void Merge(PartCodes &&from) {
    std::vector<uint32_t> brand_map;
    for (uint32_t id = 0; id < from.brands.size(); ++id)
      brand_map.push_back(brands.Intern(from.brands.Get(id)));
    from.codes.ForEach([&](int64_t key, uint32_t code) {
      if (code != 0)
        codes.Set(key, ((brand_map[(code >> 2) - 1] + 1) << 2) | (code & 3u));
    });
  }
};

} // namespace

// ── State accessors ─────────────────────────────────────────────────────────
uint32_t TpchQ19State::InternBrand(const std::string &brand) {
  const auto found = brands->Find(brand);
  if (found != core::StringDict::kNone)
    return found;
  auto copy = std::make_shared<core::StringDict>(*brands);
  const auto id = copy->Intern(brand);
  brands = std::move(copy);
  return id;
}

uint32_t TpchQ19State::PartCode(const std::string &brand,
                                const std::string &container, int32_t size) {
  const auto family = PartFamily(container, size);
  return family == TpchQ19Family::None ? 0 : Code(InternBrand(brand), family);
}

void TpchQ19State::Add(uint32_t code, int64_t qty, __int128 revenue,
                       int64_t count) {
  if (code == 0)
    return;
  auto &slot = aggregates[code];
  if (!slot)
    slot = std::make_shared<Vec>();
  else if (slot.use_count() > 1)
    slot = std::make_shared<Vec>(*slot); // copy-on-write
  auto &vec = *slot;
  auto it = std::lower_bound(vec.begin(), vec.end(), qty,
                             [](const QtyRev &qr, int64_t q) { return qr.qty < q; });
  if (it == vec.end() || it->qty != qty)
    it = vec.insert(it, QtyRev{qty, 0, 0});
  it->rev += revenue;
  it->count += count;
  if (it->count == 0 && it->rev == 0)
    vec.erase(it);
  if (vec.empty())
    aggregates.erase(code);
}

void TpchQ19State::RangeSum(const std::string &brand, TpchQ19Family family,
                            __int128 qty_lo, __int128 qty_hi,
                            __int128 &revenue, int64_t &count) const {
  const auto id = brands->Find(brand);
  if (id == core::StringDict::kNone)
    return;
  const auto it = aggregates.find(Code(id, family));
  if (it == aggregates.end() || qty_lo > qty_hi)
    return;
  const auto &vec = *it->second;
  const auto begin = std::lower_bound(
      vec.begin(), vec.end(), qty_lo,
      [](const QtyRev &qr, __int128 q) { return qr.qty < q; });
  const auto end = std::upper_bound(
      vec.begin(), vec.end(), qty_hi,
      [](__int128 q, const QtyRev &qr) { return q < qr.qty; });
  for (auto cell = begin; cell != end; ++cell) {
    revenue += cell->rev;
    count += cell->count;
  }
}

// ── TxnBuffer ───────────────────────────────────────────────────────────────
bool TpchQ19TxnBuffer::HasPendingChanges() const { return pending; }

void TpchQ19TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  deltas.clear();
  part_events.clear();
}

bool TpchQ19TxnBuffer::RequiresRebuild() const { return requires_rebuild; }

const std::vector<TpchQ19LineitemDelta> &TpchQ19TxnBuffer::Deltas() const {
  return deltas;
}

const std::vector<TpchQ19PartEvent> &TpchQ19TxnBuffer::PartEvents() const {
  return part_events;
}

void TpchQ19TxnBuffer::MarkRebuild() {
  pending = requires_rebuild = true;
  deltas.clear();
  part_events.clear();
}

void TpchQ19TxnBuffer::AddDelta(TpchQ19LineitemDelta delta) {
  pending = true;
  deltas.push_back(delta);
}

void TpchQ19TxnBuffer::AddPartEvent(TpchQ19PartEvent event) {
  pending = true;
  part_events.push_back(std::move(event));
}

// ── Module methods ──────────────────────────────────────────────────────────
core::AcceleratorId TpchQ19Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ19Module::Dependencies() const {
  std::vector<core::TableDependency> deps;
  deps.push_back({config.lineitem_table,
                   {0, 1, 2, 3, 4, 5},
                   {1, 4, 5, 6, 14, 13}});
  deps.push_back({config.part_table, {0, 1, 2, 3}, {0, 3, 6, 5}});
  return deps;
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ19Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ19TxnBuffer>();
}

void TpchQ19Module::Build(core::AuxEngineServices &services,
                          core::StateBuilder &builder) {
  builder.Write(core::StateImage::Make(
      Id(), kStatePartition, 0,
      MakeStateImage(BuildState(services, /*with_parts=*/true))));
  builder.Finish();
}

core::InvalidationPlan
TpchQ19Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.lineitem_table || mutation.table == config.part_table)
    return All();
  return {};
}

void TpchQ19Module::AccumulateMutation(const core::MutationBatch &mutation,
                                       core::AuxEngineServices &,
                                       core::AuxTxnBuffer &txn_buf) {
  auto &buf = static_cast<TpchQ19TxnBuffer &>(txn_buf);

  if (mutation.table == config.lineitem_table) {
    auto add = [&](const core::AuxRow &row, int8_t sign) {
      TpchQ19LineitemDelta delta;
      bool qualifies = false;
      if (!ReadLineitemRow(row, delta, qualifies)) {
        buf.MarkRebuild();
        return;
      }
      if (!qualifies)
        return;
      delta.sign = sign;
      buf.AddDelta(delta);
    };
    switch (mutation.kind) {
    case core::MutationKind::Insert:
      for (const auto &row : mutation.new_rows)
        add(row, 1);
      break;
    case core::MutationKind::Delete:
      for (const auto &row : mutation.old_rows)
        add(row, -1);
      break;
    case core::MutationKind::Update:
      if (mutation.old_rows.size() != mutation.new_rows.size()) {
        buf.MarkRebuild();
        return;
      }
      for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
        add(mutation.old_rows[i], -1);
        add(mutation.new_rows[i], 1);
      }
      break;
    }
    return;
  }

  if (mutation.table == config.part_table) {
    // A part appearing, disappearing or changing brand / container / size
    // moves its lineitems' contributions (re-aggregated at maintenance).
    auto event = [&](const core::AuxRow &row, bool present) {
      TpchQ19PartEvent e;
      if (!ReadPartRow(row, e.part)) {
        buf.MarkRebuild();
        return false;
      }
      e.present = present;
      e.partkey = e.part.key;
      buf.AddPartEvent(std::move(e));
      return true;
    };
    switch (mutation.kind) {
    case core::MutationKind::Insert:
      for (const auto &row : mutation.new_rows)
        if (!event(row, true))
          return;
      break;
    case core::MutationKind::Delete:
      for (const auto &row : mutation.old_rows)
        if (!event(row, false))
          return;
      break;
    case core::MutationKind::Update:
      if (mutation.old_rows.size() != mutation.new_rows.size()) {
        buf.MarkRebuild();
        return;
      }
      for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
        if (!event(mutation.old_rows[i], false) ||
            !event(mutation.new_rows[i], true))
          return;
      }
      break;
    }
  }
}

std::shared_ptr<TpchQ19State>
TpchQ19Module::BuildState(core::AuxEngineServices &services,
                          bool with_parts) const {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q19 aux requires a typed initial scan source");
  auto state = std::make_shared<TpchQ19State>();
  if (with_parts) {
    auto parts = core::PartitionedBuild<PartCodes>(
        services, [] { return PartCodes{}; },
        [&](std::size_t partitions, auto &&sink) {
          initial_scan->ScanPartsPartitioned(
              services, partitions,
              [&](std::size_t p, const TpchQ19PartInput &input) {
                sink(p, TypedRowBatch<TpchQ19PartInput>{input});
              });
        },
        [](PartCodes &partial, const TpchQ19PartInput &part) { partial.Add(part); },
        [](PartCodes &into, PartCodes &&from) { into.Merge(std::move(from)); });
    state->brands = std::make_shared<core::StringDict>(std::move(parts.brands));
    state->part_code = std::move(parts.codes);
  }
  initial_scan->ScanGroupAggregates([&](const TpchQ19GroupAggregate &agg) {
    state->Add(TpchQ19State::Code(state->InternBrand(agg.brand),
                                  static_cast<TpchQ19Family>(agg.family)),
               agg.quantity_raw, agg.revenue_raw, agg.count);
  });
  return state;
}

core::StagedMaintenanceOutcome
TpchQ19Module::StageMaintenance(core::AuxEngineServices &services,
                                const core::InvalidationPlan &,
                                core::AuxTxnBuffer &txn_buf) {
  auto &buf = static_cast<TpchQ19TxnBuffer &>(txn_buf);
  core::StagedMaintenanceOutcome outcome;
  if (!buf.RequiresRebuild() && buf.Deltas().empty() && buf.PartEvents().empty())
    return outcome;

  // The state is one copy-on-write image: applying the deltas is cheaper than
  // a rebuild unless the deltas could not be read, the state is missing, or
  // the cost model says otherwise.
  std::shared_ptr<TpchQ19State> next;
  std::unordered_map<int64_t, uint32_t> final_code; // last part event wins
  std::vector<int64_t> affected;                    // parts whose code changed
  if (!buf.RequiresRebuild()) {
    if (const auto current = LoadState(services))
      next = std::make_shared<TpchQ19State>(*current); // COW copy
  }
  if (next) {
    for (const auto &event : buf.PartEvents())
      final_code[event.partkey] =
          event.present ? next->PartCode(event.part.brand, event.part.container,
                                         event.part.size)
                        : 0;
    for (const auto &[key, code] : final_code)
      if (next->part_code.Get(key) != code)
        affected.push_back(key);
  }
  // Nothing to apply (e.g. a part that never qualified deleted): the state
  // stays valid as it is.
  if (next && affected.empty() && buf.Deltas().empty())
    return outcome;
  const auto decision = services.ChooseMaintenance(
      {static_cast<uint64_t>(buf.Deltas().size() + affected.size()), 1,
       next != nullptr && initial_scan, false, true, false, false,
       core::MaintenanceAction::NoOp, services.ExpectedQueriesBeforeNextWrite()});
  if (!next || decision.action != core::MaintenanceAction::ApplyDelta) {
    outcome.action = decision.action == core::MaintenanceAction::ApplyDelta ||
                             decision.action == core::MaintenanceAction::RebuildPartition
                         ? core::MaintenanceAction::RebuildAll
                         : decision.action;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  // Parts whose code changed: their qualifying lineitems move between
  // (brand, family) vectors. The table now holds this transaction's lineitem
  // writes too, so the post-write contribution per (part, quantity) comes from
  // one point scan, the pre-write one is that minus this transaction's
  // lineitem deltas, and those deltas are not applied again.
  struct QtyCell {
    __int128 post = 0, pre = 0;
    int64_t post_count = 0, pre_count = 0;
  };
  std::map<std::pair<int64_t, int64_t>, QtyCell> moved; // (partkey, qty)
  const std::unordered_set<int64_t> is_affected(affected.begin(), affected.end());
  if (!affected.empty()) {
    initial_scan->ScanPartQuantityRevenue(
        affected, [&](const TpchQ19PartQuantityRevenue &row) {
          auto &cell = moved[{row.partkey, row.quantity_raw}];
          cell.post += row.revenue_raw;
          cell.post_count += row.count;
          cell.pre += row.revenue_raw;
          cell.pre_count += row.count;
        });
  }
  for (const auto &delta : buf.Deltas()) {
    if (!is_affected.count(delta.partkey))
      continue;
    auto &cell = moved[{delta.partkey, delta.quantity_raw}];
    cell.pre -= delta.sign * delta.revenue_raw;
    cell.pre_count -= delta.sign;
  }
  for (const auto &[part_qty, cell] : moved) {
    const auto [key, qty] = part_qty;
    next->Add(final_code.at(key), qty, cell.post, cell.post_count);
    next->Add(next->part_code.Get(key), qty, -cell.pre, -cell.pre_count);
  }
  for (const auto key : affected)
    next->part_code.Set(key, final_code.at(key));

  // Lineitems of unaffected parts: -old +new under the part's (unchanged)
  // code, netted per (code, quantity) cell by slices of the delta list.
  struct Cell {
    __int128 revenue = 0;
    int64_t count = 0;
  };
  using Cells = std::map<std::pair<uint32_t, int64_t>, Cell>;
  Cells net;
  core::ParallelReduce(
      services, buf.Deltas().size(), kDeltaGrain, net,
      [&](Cells &cells, std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
          const auto &delta = buf.Deltas()[i];
          if (is_affected.count(delta.partkey))
            continue;
          auto &cell = cells[{next->part_code.Get(delta.partkey), delta.quantity_raw}];
          cell.revenue += delta.sign * delta.revenue_raw;
          cell.count += delta.sign;
        }
      },
      [](Cells &into, Cells &&from) {
        for (const auto &[key, cell] : from) {
          into[key].revenue += cell.revenue;
          into[key].count += cell.count;
        }
      });
  for (const auto &[code_qty, cell] : net)
    next->Add(code_qty.first, code_qty.second, cell.revenue, cell.count);

  outcome.action = core::MaintenanceAction::ApplyDelta;
  outcome.updates.push_back(core::StateImage::Make(
      Id(), kStatePartition, 0, MakeStateImage(std::move(next))));
  return outcome;
}

std::shared_ptr<const TpchQ19State>
TpchQ19Module::LoadState(core::AuxEngineServices &services) const {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {kStatePartition});
  for (const auto &image : snapshot.partitions) {
    const auto *typed = AsQ19Image(image);
    if (image.partition == kStatePartition && typed)
      return typed->state;
  }
  return nullptr;
}

bool TpchQ19Module::CheckValidity(core::AuxEngineServices &services,
                                  const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void TpchQ19Module::Execute(core::AuxEngineServices &services,
                            const core::QueryArguments &arguments,
                            core::AuxRowSink &sink) {
  const auto validity = services.StateStore().CheckValidity(
      {Id(), {}}, services.CurrentVisibility());
  if (!validity.IsValid()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  // Immutable (state, version) pair swapped atomically: safe for concurrent
  // readers and the writer's commits.
  std::shared_ptr<const TpchQ19State> state;
  const std::shared_ptr<const CachedRead> cached = std::atomic_load(&cached_read_);
  if (cached && cached->version == validity.visible_version) {
    state = cached->state;
  } else {
    state = LoadState(services);
    if (!state) {
      ExecuteFallback(services, arguments, sink);
      return;
    }
    auto fresh = std::make_shared<CachedRead>();
    fresh->state = state;
    fresh->version = validity.visible_version;
    std::atomic_store(&cached_read_,
                      std::static_pointer_cast<const CachedRead>(fresh));
  }
  Answer(*state, arguments, sink);
}

void TpchQ19Module::ExecuteFallback(core::AuxEngineServices &services,
                                    const core::QueryArguments &arguments,
                                    core::AuxRowSink &sink) {
  Answer(*BuildState(services, /*with_parts=*/false), arguments, sink);
}

void TpchQ19Module::Answer(const TpchQ19State &state,
                           const core::QueryArguments &arguments,
                           core::AuxRowSink &sink) const {
  // Branch i: p_brand = brand_i, family_i containers, p_size 1..max_i (all
  // folded into the state), l_quantity BETWEEN qty_i AND qty_i + 10, shipping
  // predicate (folded in too). A NULL argument makes its branch NULL (false).
  static constexpr TpchQ19Family kFamilies[3] = {
      TpchQ19Family::SM, TpchQ19Family::MED, TpchQ19Family::LG};
  __int128 revenue = 0;
  int64_t matches = 0;
  for (std::size_t branch = 0; branch < 3; ++branch) {
    const auto brand_index = branch * 2, qty_index = branch * 2 + 1;
    if (arguments.values.size() <= qty_index ||
        arguments.values[brand_index].is_null ||
        arguments.values[qty_index].is_null ||
        !std::holds_alternative<std::string>(arguments.values[brand_index].payload) ||
        !std::holds_alternative<int64_t>(arguments.values[qty_index].payload))
      continue;
    // Quantities are hundredths: [qty * 100, (qty + 10) * 100], in 128 bits
    // so any BIGINT argument stays exact.
    const __int128 qty = std::get<int64_t>(arguments.values[qty_index].payload);
    state.RangeSum(std::get<std::string>(arguments.values[brand_index].payload),
                   kFamilies[branch], qty * 100, (qty + 10) * 100, revenue,
                   matches);
  }
  EmitResult(revenue, matches, sink);
}

void TpchQ19Module::EmitResult(__int128 revenue_raw, int64_t matches,
                               core::AuxRowSink &sink) const {
  // SUM over no qualifying rows is NULL.
  sink.Begin({{{"revenue", core::AuxType::ScaledDecimal128, true}}});
  sink.WriteRow({{matches == 0
                      ? core::AuxValue::Null(core::AuxType::ScaledDecimal128)
                      : core::AuxValue::Of(core::AuxType::ScaledDecimal128,
                                           core::ScaledDecimal128{revenue_raw, 4})}});
  sink.End();
}

std::vector<core::CalibrationWrite> TpchQ19Module::CalibrationWrites() const {
  return TpchQ19CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
