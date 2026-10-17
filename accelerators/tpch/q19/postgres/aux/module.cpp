#include "tpch/q19/postgres/aux/module.hpp"
#include "tpch/q19/calibration_writes.hpp"
#include "substrate/aux/core/aux_cells.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/partitioned_build.hpp"
#include "prims/pg_numeric.hpp"

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
// l_quantity BETWEEN qty AND qty + kQuantityWindow.
constexpr int64_t kQuantityWindow = 10;
// One unit of a scale-2 decimal (quantity, price, discount).
constexpr int64_t kScale2Unit = 100;
constexpr int32_t kScale2 = 2;

using core::DecimalCell;
using core::IntCell;
using core::ScaleToRaw;
using core::StringCell;

// String comparison as the engine's column collation does it. PostgreSQL's
// columns are COLLATE "C": byte equality.
std::string CollateKey(const std::string &text) { return text; }

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
  int32_t qty_scale = 0, ext_scale = 0, disc_scale = 0;
  std::string shipmode, shipinstruct;
  if (!IntCell(row, 0, partkey) ||
      !DecimalCell(row, 1, quantity_raw, qty_scale) ||
      !DecimalCell(row, 2, extendedprice_raw, ext_scale) ||
      !DecimalCell(row, 3, discount_raw, disc_scale) ||
      !StringCell(row, 4, shipmode) || !StringCell(row, 5, shipinstruct))
    return false;
  shipmode = CollateKey(shipmode);
  qualifies = (shipmode == CollateKey("AIR") || shipmode == CollateKey("AIR REG")) &&
              CollateKey(shipinstruct) == CollateKey("DELIVER IN PERSON");
  out.partkey = partkey;
  out.quantity_raw = ScaleToRaw(quantity_raw, qty_scale, kScale2);
  // l_extendedprice * (1 - l_discount) at scale 4.
  out.revenue_raw =
      static_cast<__int128>(ScaleToRaw(extendedprice_raw, ext_scale, kScale2)) *
      (kScale2Unit - ScaleToRaw(discount_raw, disc_scale, kScale2));
  return true;
}

TpchQ19Family ClassifyContainerFamily(const std::string &container_text) {
  const std::string container = CollateKey(container_text);
  for (const char *c : {"SM CASE", "SM BOX", "SM PACK", "SM PKG"})
    if (container == CollateKey(c))
      return TpchQ19Family::SM;
  for (const char *c : {"MED BAG", "MED BOX", "MED PKG", "MED PACK"})
    if (container == CollateKey(c))
      return TpchQ19Family::MED;
  for (const char *c : {"LG CASE", "LG BOX", "LG PACK", "LG PKG"})
    if (container == CollateKey(c))
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

// Part codes of one scan partition, with the brands in order of first
// appearance. Partials merge in partition order, so the merged dictionary is
// the one a serial scan builds.
struct PartCodes {
  core::StringDict brands;
  core::AnyKeyCowArray<uint32_t, 14> codes{0};

  void Add(const TpchQ19PartInput &part) {
    const auto family = ClassifyContainerFamily(part.container);
    if (family == TpchQ19Family::None || part.size < 1 ||
        part.size > BranchSizeLimit(family))
      return;
    codes.Set(part.key,
              TpchQ19State::Code(brands.Intern(CollateKey(part.brand)), family));
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

} // namespace

uint32_t TpchQ19State::InternBrand(const std::string &brand) {
  const auto key = CollateKey(brand);
  const auto found = brands->Find(key);
  if (found != core::StringDict::kNone)
    return found;
  auto copy = std::make_shared<core::StringDict>(*brands);
  const auto id = copy->Intern(key);
  brands = std::move(copy);
  return id;
}

uint32_t TpchQ19State::PartCode(const std::string &brand,
                                const std::string &container, int32_t size) {
  const auto family = ClassifyContainerFamily(container);
  if (family == TpchQ19Family::None || size < 1 ||
      size > BranchSizeLimit(family))
    return 0;
  return Code(InternBrand(brand), family);
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
  const auto id = brands->Find(CollateKey(brand));
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
      MakeStateImage(BuildState(services))));
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
TpchQ19Module::BuildState(core::AuxEngineServices &services) const {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q19 aux requires a typed initial scan source");
  auto state = std::make_shared<TpchQ19State>();
  // Columnar per-part codes, decoded and folded in partitions.
  auto parts = core::PartitionedBuild<PartCodes>(
      services, [] { return PartCodes{}; },
      [&](std::size_t partitions, auto &&sink) {
        initial_scan->ScanParts(
            services, partitions,
            [&](std::size_t p, const TpchQ19PartInput &input) {
              sink(p, core::SingleRowBatch<TpchQ19PartInput>{input});
            });
      },
      [](PartCodes &partial, const TpchQ19PartInput &part) { partial.Add(part); },
      [](PartCodes &into, PartCodes &&from) { into.Merge(std::move(from)); });
  state->brands = std::make_shared<core::StringDict>(std::move(parts.brands));
  state->part_code = std::move(parts.codes);
  initial_scan->ScanGroupAggregatesSerial([&](const TpchQ19GroupAggregate &agg) {
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

  // The state is one copy-on-write image: applying the deltas is always
  // cheaper than a rebuild, which is chosen only when the deltas could not be
  // read, the state is missing, too many parts changed, or it is forced.
  std::shared_ptr<TpchQ19State> next;
  std::unordered_map<int64_t, uint32_t> final_code; // last part event wins
  std::vector<int64_t> affected;                    // parts whose code changed
  if (!buf.RequiresRebuild()) {
    const auto snapshot = services.StateStore().ReadPartitions(
        services.CurrentVisibility(), {kStatePartition});
    for (const auto &image : snapshot.partitions) {
      const auto *typed = AsQ19Image(image);
      if (image.partition == kStatePartition && typed && typed->state)
        next = std::make_shared<TpchQ19State>(*typed->state); // COW copy
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
  }
  // Nothing to apply (e.g. a part that never qualified deleted): the state
  // stays valid as it is.
  if (next && affected.empty() && buf.Deltas().empty())
    return outcome;
  auto request = core::MaintenanceRequest::IncrementalSingleState(
      buf.Deltas().size() + affected.size(),
      services.ExpectedQueriesBeforeNextWrite());
  request.incremental_supported = next != nullptr && initial_scan;
  const auto decision = services.ChooseMaintenance(request);
  if (!next || decision.action != core::MaintenanceAction::ApplyDelta) {
    core::FinishRebuildAll(outcome, decision);
    return outcome;
  }

  // Parts whose code changed: their qualifying lineitems move between
  // (brand, family) vectors. The table now holds this transaction's lineitem
  // writes too, so the post-write contribution per (part, quantity) comes from
  // one indexed point scan, the pre-write one is that minus this
  // transaction's lineitem deltas, and those deltas are not applied again.
  struct QtyCell {
    __int128 post = 0, pre = 0;
    int64_t post_count = 0, pre_count = 0;
  };
  std::map<std::pair<int64_t, int64_t>, QtyCell> moved; // (partkey, qty)
  const std::unordered_set<int64_t> is_affected(affected.begin(), affected.end());
  if (!affected.empty()) {
    initial_scan->ScanPartQuantityRevenueSerial(
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

bool TpchQ19Module::CheckValidity(core::AuxEngineServices &services,
                                  const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void TpchQ19Module::Execute(core::AuxEngineServices &services,
                            const core::QueryArguments &arguments,
                            core::AuxRowSink &sink) {
  const auto snapshot = services.StateStore().ReadHitPartitions(
      services.CurrentVisibility(), {Id(), {}}, {kStatePartition});
  const auto *typed = snapshot.available && snapshot.partitions.size() == 1
                          ? AsQ19Image(snapshot.partitions[0])
                          : nullptr;
  if (!typed || !typed->state) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  Answer(*typed->state, arguments, sink);
}

void TpchQ19Module::Answer(const TpchQ19State &state,
                           const core::QueryArguments &arguments,
                           core::AuxRowSink &sink) const {
  // Branch i: p_brand = brand_i, family_i containers, p_size 1..max_i (all
  // folded into the state), l_quantity BETWEEN qty_i AND qty_i + window, shipping
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
    // Quantities are hundredths, in 128 bits so any BIGINT argument stays exact.
    const __int128 qty = std::get<int64_t>(arguments.values[qty_index].payload);
    state.RangeSum(std::get<std::string>(arguments.values[brand_index].payload),
                   kFamilies[branch], qty * kScale2Unit,
                   (qty + kQuantityWindow) * kScale2Unit, revenue, matches);
  }
  EmitResult(revenue, matches, sink);
}

void TpchQ19Module::EmitResult(__int128 revenue_raw, int64_t matches,
                               core::AuxRowSink &sink) const {
  // COALESCE(SUM(l_extendedprice * (1 - l_discount)), 0): the sum as exact
  // NUMERIC text at scale 4, and the integer literal 0 when nothing matched.
  sink.Begin({{{"revenue", core::AuxType::String, false}}});
  sink.WriteRow({{core::AuxValue::Of(
      core::AuxType::String,
      matches == 0 ? std::string("0")
                   : ::accelerator_codegen::pg_numeric::ToText(revenue_raw, 4))}});
  sink.End();
}

std::vector<core::CalibrationWrite> TpchQ19Module::CalibrationWrites() const {
  return TpchQ19CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
