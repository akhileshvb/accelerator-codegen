#include "tpch/q17/mysql/aux/module.hpp"
#include "tpch/q17/calibration_writes.hpp"

#include <algorithm>
#include "accelerator_codegen/stateful/core/mysql_collation.hpp"
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {

using Int128 = __int128;

// ── Row decoding ──────────────────────────────────────────────────────────

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

// decimal(15,2) cell as an exact x100 integer.
bool Scale2Cell(const core::AuxRow &row, std::size_t index, int64_t &out) {
  if (index >= row.values.size() || row.values[index].is_null ||
      !std::holds_alternative<core::ScaledDecimal>(row.values[index].payload))
    return false;
  const auto decimal = std::get<core::ScaledDecimal>(row.values[index].payload);
  Int128 raw = decimal.value;
  for (int32_t s = decimal.scale; s < 2; ++s)
    raw *= 10;
  for (int32_t s = decimal.scale; s > 2; --s) {
    if (raw % 10 != 0)
      return false; // not representable at scale 2: rebuild instead
    raw /= 10;
  }
  if (raw > std::numeric_limits<int64_t>::max() ||
      raw < std::numeric_limits<int64_t>::min())
    return false;
  out = static_cast<int64_t>(raw);
  return true;
}

// A lineitem with a NULL partkey (never joins) or NULL quantity (ignored by
// AVG and never < anything) is skipped. Anything unreadable forces a rebuild.
enum class RowStatus { Ok, Skip, Bad };

RowStatus ReadLineitem(const core::AuxRow &row, TpchQ17LineitemInput &out) {
  if (row.values.size() < 3)
    return RowStatus::Bad;
  if (row.values[0].is_null || row.values[1].is_null)
    return RowStatus::Skip;
  if (!IntCell(row, 0, out.partkey) || !Scale2Cell(row, 1, out.quantity_raw))
    return RowStatus::Bad;
  out.extendedprice_raw = 0;
  if (!row.values[2].is_null && !Scale2Cell(row, 2, out.extendedprice_raw))
    return RowStatus::Bad;
  return RowStatus::Ok;
}

// ── Histograms and contributions ─────────────────────────────────────────

struct Contribution {
  Int128 price_sum_raw = 0;
  int64_t row_count = 0;
};

// The rows of one part with l_quantity < 0.2 * avg(l_quantity), exactly:
// with C rows and quantity sum S (both x100), q < 0.2 * S / C  <=>  5*C*q < S.
// Quantities are sorted, so the qualifying rows are a prefix (binary search).
Contribution ContributionOf(const TpchQ17PartHistogram *hist) {
  Contribution out;
  if (!hist || hist->empty())
    return out;
  Int128 count = 0, sum_qty = 0;
  for (const auto &e : *hist) {
    count += e.row_count;
    sum_qty += static_cast<Int128>(e.quantity_raw) * e.row_count;
  }
  std::size_t lo = 0, hi = hist->size();
  while (lo < hi) {
    const std::size_t mid = (lo + hi) / 2;
    if (static_cast<Int128>((*hist)[mid].quantity_raw) * 5 * count < sum_qty)
      lo = mid + 1;
    else
      hi = mid;
  }
  for (std::size_t i = 0; i < lo; ++i) {
    out.price_sum_raw += (*hist)[i].price_sum_raw;
    out.row_count += (*hist)[i].row_count;
  }
  return out;
}

void AddEntry(TpchQ17PartHistogram &hist, const TpchQ17QtyEntry &add) {
  auto pos = std::lower_bound(
      hist.begin(), hist.end(), add.quantity_raw,
      [](const TpchQ17QtyEntry &e, int64_t q) { return e.quantity_raw < q; });
  if (pos != hist.end() && pos->quantity_raw == add.quantity_raw) {
    int64_t price = 0;
    const uint64_t rows = static_cast<uint64_t>(pos->row_count) + add.row_count;
    if (__builtin_add_overflow(pos->price_sum_raw, add.price_sum_raw, &price) ||
        rows > UINT32_MAX)
      throw std::overflow_error("TPC-H Q17 histogram overflow");
    pos->price_sum_raw = price;
    pos->row_count = static_cast<uint32_t>(rows);
  } else {
    hist.insert(pos, add);
  }
}

void MergeInto(TpchQ17PartHistogram &into, const TpchQ17PartHistogram &from) {
  for (const auto &e : from)
    AddEntry(into, e);
}

// ── State editing ─────────────────────────────────────────────────────────

void AdjustTotal(TpchQ17State &state, uint32_t group, const Contribution &c,
                 int sign) {
  if (group == kTpchQ17NoGroup || (c.row_count == 0 && c.price_sum_raw == 0))
    return;
  auto &total = state.totals.Mutable(group);
  total.price_sum_raw += sign * c.price_sum_raw;
  total.row_count += sign * c.row_count;
}

// Edits the group dictionary of a state copy; the dictionary object is cloned
// only when a new (brand, container) appears.
class DictionaryEditor {
public:
  explicit DictionaryEditor(TpchQ17State &state) : state_(state) {}
  uint32_t Id(const std::string &brand, const std::string &container) {
    const auto key = TpchQ17GroupDictionary::MakeKey(brand, container);
    const auto &ids = own_ ? own_->ids : state_.dictionary->ids;
    const auto it = ids.find(key);
    if (it != ids.end())
      return it->second;
    if (!own_)
      own_ = std::make_shared<TpchQ17GroupDictionary>(*state_.dictionary);
    const auto id = static_cast<uint32_t>(own_->ids.size());
    own_->ids.emplace(key, id);
    return id;
  }
  void Finish() {
    if (own_)
      state_.dictionary = std::move(own_);
  }

private:
  TpchQ17State &state_;
  std::shared_ptr<TpchQ17GroupDictionary> own_;
};

// Moves a part to another group, carrying its current contribution.
void SetPartGroup(TpchQ17State &state, int64_t partkey, uint32_t group) {
  const uint32_t old_group = state.part_group.Get(partkey);
  if (old_group == group)
    return;
  const auto contribution = ContributionOf(state.parts.Get(partkey).get());
  AdjustTotal(state, old_group, contribution, -1);
  AdjustTotal(state, group, contribution, +1);
  state.part_group.Set(partkey, group);
}

// Replaces a part's histogram and updates its group's total by the difference.
void SetPartHistogram(TpchQ17State &state, int64_t partkey,
                      std::shared_ptr<const TpchQ17PartHistogram> hist) {
  if (hist && hist->empty())
    hist = nullptr;
  const uint32_t group = state.part_group.Get(partkey);
  if (group != kTpchQ17NoGroup) {
    AdjustTotal(state, group, ContributionOf(state.parts.Get(partkey).get()), -1);
    AdjustTotal(state, group, ContributionOf(hist.get()), +1);
  }
  if (hist || state.parts.Find(partkey))
    state.parts.Set(partkey, std::move(hist));
}

core::InvalidationPlan All() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

core::InvalidationPlan StatePlan() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
  plan.partitions.insert(static_cast<::accelerator_codegen::aux_state::PartitionKey>(
      TpchQ17Layout::kStatePartition));
  return plan;
}

const TpchQ17State *ReadState(core::AuxEngineServices &services) {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {TpchQ17Layout::kStatePartition});
  for (const auto &image : snapshot.partitions) {
    if (image.partition != TpchQ17Layout::kStatePartition)
      continue;
    const auto *typed = image.As<TpchQ17StateImage>();
    return typed && typed->state ? typed->state.get() : nullptr;
  }
  return nullptr;
}

core::StateImage MakeImage(std::shared_ptr<const TpchQ17State> state) {
  auto image = std::make_shared<TpchQ17StateImage>();
  image->state = std::move(state);
  return core::StateImage::Make(
      TpchQ17Layout::kAccelerator, TpchQ17Layout::kStatePartition, 0,
      std::shared_ptr<const TpchQ17StateImage>(std::move(image)));
}

// ── Query arguments / output ──────────────────────────────────────────────

// False when either argument is SQL NULL (p_brand = NULL is never true).
bool ReadArguments(const core::QueryArguments &arguments, std::string &brand,
                   std::string &container) {
  if (arguments.values.size() < 2)
    return false;
  for (std::size_t i = 0; i < 2; ++i)
    if (arguments.values[i].is_null ||
        !std::holds_alternative<std::string>(arguments.values[i].payload))
      return false;
  brand = std::get<std::string>(arguments.values[0].payload);
  container = std::get<std::string>(arguments.values[1].payload);
  return true;
}

void EmitAverageYearly(core::AuxRowSink &sink, const Contribution *total) {
  sink.Begin({{{"avg_yearly", core::AuxType::Double, true}}});
  if (!total || total->row_count == 0) {
    sink.WriteRow({{core::AuxValue::Null(core::AuxType::Double)}});
  } else {
    // SUM(l_extendedprice) / 7.0 with the sum held exactly (x100).
    const double result = static_cast<double>(total->price_sum_raw) / 700.0;
    sink.WriteRow({{core::AuxValue::Of(core::AuxType::Double, result)}});
  }
  sink.End();
}

} // namespace

std::string TpchQ17GroupDictionary::MakeKey(const std::string &brand,
                                            const std::string &container) {
  std::string key;
  key.reserve(4 + brand.size() + container.size());
  const uint32_t n = static_cast<uint32_t>(brand.size());
  key.append(reinterpret_cast<const char *>(&n), sizeof(n));
  // p_brand = ? AND p_container = ? are case-insensitive (MySQL default
  // collation): the key is the ASCII-folded pair.
  key.append(mysql_collation::FoldAscii(brand));
  key.append(mysql_collation::FoldAscii(container));
  return key;
}

// ── TxnBuffer ─────────────────────────────────────────────────────────────

bool TpchQ17TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ17TxnBuffer::Clear() {
  pending = false;
  rebuild = false;
  lineitem_deltas.clear();
  part_deltas.clear();
}
bool TpchQ17TxnBuffer::RequiresRebuild() const { return rebuild; }
const std::vector<TpchQ17LineitemDelta> &
TpchQ17TxnBuffer::LineitemDeltas() const {
  return lineitem_deltas;
}
const std::vector<TpchQ17PartDelta> &TpchQ17TxnBuffer::PartDeltas() const {
  return part_deltas;
}
void TpchQ17TxnBuffer::MarkRebuild() {
  pending = true;
  rebuild = true;
  lineitem_deltas.clear();
  part_deltas.clear();
}
void TpchQ17TxnBuffer::AddLineitemDelta(TpchQ17LineitemDelta delta) {
  pending = true;
  lineitem_deltas.push_back(delta);
}
void TpchQ17TxnBuffer::AddPartDelta(TpchQ17PartDelta delta) {
  pending = true;
  part_deltas.push_back(std::move(delta));
}

// ── Module ────────────────────────────────────────────────────────────────

core::AcceleratorId TpchQ17Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ17Module::Dependencies() const {
  return {{config.part_table, {0, 1, 2}, {0, 3, 6}},
          {config.lineitem_table, {0, 1, 2}, {1, 4, 5}}};
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ17Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ17TxnBuffer>();
}

void TpchQ17Module::ScanHistograms(
    const std::function<void(int64_t, TpchQ17PartHistogram &&)> &consumer)
    const {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q17 requires a typed initial scan source");

  // Preferred: SQL GROUP BY (partkey, quantity) ordered by partkey; one part's
  // histogram is assembled at a time.
  int64_t current = 0;
  TpchQ17PartHistogram hist;
  bool have = false;
  const bool used_sql = initial_scan->ScanPartQuantityAggregates(
      [&](const TpchQ17PartQuantityAggregate &agg) {
        if (have && agg.partkey != current) {
          consumer(current, std::move(hist));
          hist = TpchQ17PartHistogram();
        }
        current = agg.partkey;
        have = true;
        if (agg.row_count < 0 || agg.row_count > UINT32_MAX)
          throw std::overflow_error("TPC-H Q17 row count out of range");
        AddEntry(hist, {agg.quantity_raw, agg.extendedprice_sum_raw,
                        static_cast<uint32_t>(agg.row_count)});
      });
  if (used_sql) {
    if (have)
      consumer(current, std::move(hist));
    return;
  }

  // Generic: aggregate the lineitem scan per part.
  std::unordered_map<int64_t, TpchQ17PartHistogram> by_part;
  initial_scan->ScanLineitems([&](const TpchQ17LineitemInput &input) {
    AddEntry(by_part[input.partkey],
             {input.quantity_raw, input.extendedprice_raw, 1});
  });
  for (auto &[partkey, h] : by_part)
    consumer(partkey, std::move(h));
}

void TpchQ17Module::Build(core::AuxEngineServices &,
                          core::StateBuilder &builder) {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q17 requires a typed initial scan source");
  auto state = std::make_shared<TpchQ17State>();

  DictionaryEditor dictionary(*state);
  initial_scan->ScanParts([&](const TpchQ17PartInput &input) {
    state->part_group.Set(input.key, dictionary.Id(input.brand, input.container));
  });
  dictionary.Finish();

  ScanHistograms([&](int64_t partkey, TpchQ17PartHistogram &&hist) {
    // A part reported twice (non-contiguous scan) is merged.
    if (const auto *existing = state->parts.Find(partkey); existing && *existing)
      MergeInto(hist, **existing);
    SetPartHistogram(*state, partkey,
                     std::make_shared<const TpchQ17PartHistogram>(
                         std::move(hist)));
  });

  builder.Write(MakeImage(std::move(state)));
  builder.Finish();
}

core::InvalidationPlan
TpchQ17Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.lineitem_table ||
      mutation.table == config.part_table)
    return StatePlan();
  return {};
}

void TpchQ17Module::AccumulateMutation(const core::MutationBatch &mutation,
                                       core::AuxEngineServices &,
                                       core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ17TxnBuffer &>(base_buffer);

  if (mutation.table == config.part_table) {
    // Rows are [partkey, brand, container].
    auto make = [&](const core::AuxRow *old_row, const core::AuxRow *new_row) {
      TpchQ17PartDelta delta;
      if (old_row) {
        if (!IntCell(*old_row, 0, delta.old_partkey)) {
          buffer.MarkRebuild();
          return;
        }
        delta.has_old = true;
      }
      if (new_row) {
        if (!IntCell(*new_row, 0, delta.new_partkey)) {
          buffer.MarkRebuild();
          return;
        }
        delta.has_new = true;
        delta.new_has_group = StringCell(*new_row, 1, delta.new_brand) &&
                              StringCell(*new_row, 2, delta.new_container);
      }
      buffer.AddPartDelta(std::move(delta));
    };
    switch (mutation.kind) {
    case core::MutationKind::Insert:
      for (const auto &row : mutation.new_rows)
        make(nullptr, &row);
      break;
    case core::MutationKind::Delete:
      for (const auto &row : mutation.old_rows)
        make(&row, nullptr);
      break;
    case core::MutationKind::Update:
      if (mutation.old_rows.size() != mutation.new_rows.size()) {
        buffer.MarkRebuild();
        return;
      }
      for (std::size_t i = 0; i < mutation.old_rows.size(); ++i)
        make(&mutation.old_rows[i], &mutation.new_rows[i]);
      break;
    }
    return;
  }

  if (mutation.table != config.lineitem_table)
    return;

  auto add_lineitem = [&](const core::AuxRow &row, int8_t sign) {
    TpchQ17LineitemInput input;
    switch (ReadLineitem(row, input)) {
    case RowStatus::Skip:
      return;
    case RowStatus::Bad:
      buffer.MarkRebuild();
      return;
    case RowStatus::Ok:
      break;
    }
    buffer.AddLineitemDelta(
        {input.partkey, input.quantity_raw, input.extendedprice_raw, sign});
  };

  switch (mutation.kind) {
  case core::MutationKind::Insert:
    for (const auto &row : mutation.new_rows)
      add_lineitem(row, 1);
    break;
  case core::MutationKind::Delete:
    for (const auto &row : mutation.old_rows)
      add_lineitem(row, -1);
    break;
  case core::MutationKind::Update:
    if (mutation.old_rows.size() != mutation.new_rows.size()) {
      buffer.MarkRebuild();
      return;
    }
    for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
      add_lineitem(mutation.old_rows[i], -1);
      add_lineitem(mutation.new_rows[i], 1);
    }
    break;
  }
}

core::StagedMaintenanceOutcome
TpchQ17Module::StageMaintenance(core::AuxEngineServices &services,
                                const core::InvalidationPlan &,
                                core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ17TxnBuffer &>(base_buffer);
  core::StagedMaintenanceOutcome outcome;

  if (buffer.RequiresRebuild()) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }
  if (buffer.LineitemDeltas().empty() && buffer.PartDeltas().empty())
    return outcome;

  // Honor the cost model's (or a forced) choice: this state has no finer
  // partitions, so anything but ApplyDelta is a whole-state rebuild.
  {
    const auto decision = services.ChooseMaintenance(
        {static_cast<uint64_t>(buffer.LineitemDeltas().size() + buffer.PartDeltas().size()), 1, true, false, true, false, false,
         core::MaintenanceAction::NoOp,
         services.ExpectedQueriesBeforeNextWrite()});
    if (decision.action == core::MaintenanceAction::RebuildPartition) {
      core::FinishRebuildAll(outcome);
      return outcome;
    }
    if (decision.action != core::MaintenanceAction::ApplyDelta) {
      outcome.action = decision.action;
      outcome.invalidations = All();
      outcome.pending_rebuild = outcome.invalidations;
      return outcome;
    }
  }

  const TpchQ17State *current = ReadState(services);
  if (!current) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }
  // A copy shares every chunk; only the chunks written below are cloned.
  auto state = std::make_shared<TpchQ17State>(*current);

  // Part rows first (group membership), in order; each move carries the part's
  // current contribution to the new group.
  DictionaryEditor dictionary(*state);
  for (const auto &delta : buffer.PartDeltas()) {
    if (delta.has_old)
      SetPartGroup(*state, delta.old_partkey, kTpchQ17NoGroup);
    if (delta.has_new)
      SetPartGroup(*state, delta.new_partkey,
                   delta.new_has_group
                       ? dictionary.Id(delta.new_brand, delta.new_container)
                       : kTpchQ17NoGroup);
  }
  dictionary.Finish();

  // Lineitem rows: edit each touched part's histogram once; the totals of its
  // group change by the difference of the part's contribution.
  std::map<int64_t, std::vector<const TpchQ17LineitemDelta *>> by_part;
  for (const auto &delta : buffer.LineitemDeltas())
    by_part[delta.partkey].push_back(&delta);
  for (const auto &[partkey, deltas] : by_part) {
    TpchQ17PartHistogram hist;
    if (const auto *existing = state->parts.Find(partkey); existing && *existing)
      hist = **existing;
    for (const auto *delta : deltas) {
      auto pos = std::lower_bound(
          hist.begin(), hist.end(), delta->quantity_raw,
          [](const TpchQ17QtyEntry &e, int64_t q) { return e.quantity_raw < q; });
      const bool found = pos != hist.end() && pos->quantity_raw == delta->quantity_raw;
      if (delta->sign > 0) {
        AddEntry(hist, {delta->quantity_raw, delta->extendedprice_raw, 1});
        continue;
      }
      if (!found || pos->row_count == 0) {
        // The state does not contain the removed row: it is out of sync.
        core::FinishRebuildAll(outcome);
        return outcome;
      }
      int64_t price = 0;
      if (__builtin_sub_overflow(pos->price_sum_raw, delta->extendedprice_raw,
                                 &price))
        throw std::overflow_error("TPC-H Q17 histogram overflow");
      pos->price_sum_raw = price;
      if (--pos->row_count == 0)
        hist.erase(pos);
    }
    SetPartHistogram(*state, partkey,
                     std::make_shared<const TpchQ17PartHistogram>(
                         std::move(hist)));
  }

  outcome.action = core::MaintenanceAction::ApplyDelta;
  outcome.updates.push_back(
      MakeImage(std::shared_ptr<const TpchQ17State>(std::move(state))));
  return outcome;
}

bool TpchQ17Module::CheckValidity(core::AuxEngineServices &services,
                                  const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void TpchQ17Module::Execute(core::AuxEngineServices &services,
                            const core::QueryArguments &arguments,
                            core::AuxRowSink &sink) {
  std::string brand, container;
  if (!ReadArguments(arguments, brand, container)) {
    EmitAverageYearly(sink, nullptr); // p_brand = NULL matches nothing
    return;
  }
  if (!services.StateStore()
           .CheckValidity({Id(), {}}, services.CurrentVisibility())
           .IsValid()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  const TpchQ17State *state = ReadState(services);
  if (!state) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  const auto it =
      state->dictionary->ids.find(TpchQ17GroupDictionary::MakeKey(brand, container));
  if (it == state->dictionary->ids.end()) {
    EmitAverageYearly(sink, nullptr); // no such part
    return;
  }
  const auto total = state->totals.Get(it->second);
  const Contribution result{total.price_sum_raw, total.row_count};
  EmitAverageYearly(sink, &result);
}

// Exact answer straight from the tables (own uncommitted writes): only the
// matching parts' histograms are kept while streaming.
void TpchQ17Module::ExecuteFallback(core::AuxEngineServices &,
                                    const core::QueryArguments &arguments,
                                    core::AuxRowSink &sink) {
  std::string brand, container;
  if (!ReadArguments(arguments, brand, container)) {
    EmitAverageYearly(sink, nullptr);
    return;
  }
  if (!initial_scan)
    throw std::logic_error("TPC-H Q17 requires a typed initial scan source");
  std::unordered_set<int64_t> parts;
  initial_scan->ScanParts([&](const TpchQ17PartInput &input) {
    if (mysql_collation::EqualsCi(input.brand, brand) &&
        mysql_collation::EqualsCi(input.container, container))
      parts.insert(input.key);
  });
  std::unordered_map<int64_t, TpchQ17PartHistogram> histograms;
  ScanHistograms([&](int64_t partkey, TpchQ17PartHistogram &&hist) {
    if (parts.count(partkey))
      MergeInto(histograms[partkey], hist);
  });
  Contribution total;
  for (const auto &[partkey, hist] : histograms) {
    const auto c = ContributionOf(&hist);
    total.price_sum_raw += c.price_sum_raw;
    total.row_count += c.row_count;
  }
  EmitAverageYearly(sink, &total);
}

std::vector<core::CalibrationWrite> TpchQ17Module::CalibrationWrites() const {
  return TpchQ17CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
