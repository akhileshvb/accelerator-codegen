#include "tpch/q14/duckdb/aux/module.hpp"
#include "prims/sql_date.hpp"
#include "substrate/aux/core/aux_cells.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/partitioned_build.hpp"
#include "tpch/q14/calibration_writes.hpp"
#include "tpch/q14/layout.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {

// Deltas per slice (~120 us): classifying and summing one is ~30 ns.
constexpr std::size_t kDeltaGrain = 4096;
// Lineitem deltas below which a commit is applied as deltas whatever the policy chose.
constexpr std::size_t kLocalDeltaLimit = 50000;

struct PartRow {
  int64_t key;
  bool promo;
};

void AddTotals(TpchQ14MonthTotals &into, const TpchQ14MonthTotals &from) {
  into.total_revenue_raw += from.total_revenue_raw;
  into.promo_revenue_raw += from.promo_revenue_raw;
  into.row_count += from.row_count;
}

int32_t MonthKey(int32_t days) {
  const auto civil = core::sql_date::CivilFromDays(days);
  return civil.year * 100 + civil.month;
}

std::shared_ptr<const TpchQ14StateImage>
MakeStateImage(TpchQ14ImageKind kind, int32_t month,
               const std::shared_ptr<const TpchQ14State> &state) {
  auto image = std::make_shared<TpchQ14StateImage>();
  image->kind = kind;
  image->month = month;
  image->state = state;
  return image;
}

const TpchQ14StateImage *AsQ14Image(const core::StateImage &image,
                                    TpchQ14ImageKind kind) {
  const auto *payload = image.As<TpchQ14StateImage>();
  if (!payload || payload->kind != kind)
    return nullptr;
  return kind == TpchQ14ImageKind::PartPromo && !payload->state ? nullptr
                                                                : payload;
}

std::shared_ptr<const TpchQ14StateImage>
MakeMonthImage(int32_t month, const TpchQ14MonthTotals &totals) {
  auto image = std::make_shared<TpchQ14StateImage>();
  image->kind = TpchQ14ImageKind::Month;
  image->month = month;
  image->totals = totals;
  return image;
}

bool ReadLineitem(const core::AuxRow &row, TpchQ14LineitemInput &out) {
  int64_t partkey = 0;
  int64_t extended_raw = 0, discount_raw = 0;
  int32_t extended_scale = 0, discount_scale = 0;
  if (!core::IntCell(row, 0, partkey) ||
      !core::DecimalCell(row, 1, extended_raw, extended_scale) ||
      !core::DecimalCell(row, 2, discount_raw, discount_scale) ||
      !core::DateCell(row, 3, out.shipdate_days))
    return false;
  out.partkey = partkey;
  out.extended_price_raw = extended_raw;
  out.extended_price_scale = extended_scale;
  out.discount_raw = discount_raw;
  out.discount_scale = discount_scale;
  return true;
}

// Part classes, copied (chunk directory; chunks stay shared until SetRaw
// touches one) only when a write actually changes a class: lineitem-only
// batches read the published state and copy nothing.
struct WorkState {
  const TpchQ14State *published;
  TpchQ14State copy;
  bool copied = false;
  bool changed = false;

  uint8_t RawOf(int64_t key) const { return copied ? copy.RawOf(key) : published->RawOf(key); }
  TpchQ14PromoClass PromoOf(int64_t key) const {
    return static_cast<TpchQ14PromoClass>(RawOf(key) & TpchQ14State::kClassMask);
  }
  int64_t BuildMax() const {
    return copied ? copy.build_max_partkey : published->build_max_partkey;
  }
  void SetRaw(int64_t key, uint8_t raw) {
    if (!copied) {
      copy.build_max_partkey = published->build_max_partkey;
      copy.classes = published->classes;
      copy.promo_overflow = published->promo_overflow;
      copied = true;
    }
    copy.SetRaw(key, raw);
  }
  // Records a class write for the commit.
  void Write(int64_t key, uint8_t raw) {
    SetRaw(key, raw);
    changed = true;
  }
};

// Month-total adjustments (signed deltas; saw_contribution unused), flat and
// sorted by month.
struct Adjust {
  int32_t month;
  TpchQ14MonthTotals totals;
};

TpchQ14MonthTotals &AdjustOf(std::vector<Adjust> &sorted, int32_t month) {
  auto it = std::lower_bound(sorted.begin(), sorted.end(), month,
                             [](const Adjust &a, int32_t m) { return a.month < m; });
  if (it == sorted.end() || it->month != month)
    it = sorted.insert(it, Adjust{month, {}});
  return it->totals;
}

// A part whose lineitems may exist (not fresh): old/new class, resolved with
// one lineitem aggregate scan.
struct Transition {
  TpchQ14PromoClass from;
  TpchQ14PromoClass to;
};
using Transitions = std::map<int64_t, Transition>;

constexpr uint8_t kFresh = TpchQ14State::kFreshBit;
constexpr uint8_t kGone = TpchQ14State::kGoneBit;
constexpr uint8_t kDead = TpchQ14State::kDeadBit;

// Re-aggregates the touched months from the base tables (RebuildPartition).
// False when the part classes or the scan source are missing.
bool RebuildMonths(TpchQ14InitialScan *scan, const TpchQ14State *part_state,
                   const std::vector<int32_t> &touched,
                   core::StagedMaintenanceOutcome &outcome) {
  if (!part_state || !scan)
    return false;
  const std::set<int32_t> touched_months(touched.begin(), touched.end());
  TpchQ14State rebuilt;
  scan->ScanMonthAggregates(
      touched_months, [&](int32_t month, int64_t total, int64_t promo, int64_t count) {
        auto &totals = rebuilt.month_totals[month];
        totals.total_revenue_raw += total;
        totals.promo_revenue_raw += promo;
        totals.row_count += count;
        totals.saw_contribution = totals.saw_contribution || count > 0;
      });
  for (const auto month : touched_months) {
    const auto it = rebuilt.month_totals.find(month);
    outcome.updates.push_back(core::StateImage::Make(
        core::TpchQ14Layout::kAccelerator, static_cast<core::PartitionId>(month), 0,
        MakeMonthImage(month, it != rebuilt.month_totals.end() ? it->second
                                                                : TpchQ14MonthTotals{})));
  }
  core::FinishRebuildPartition(outcome);
  return true;
}

// Applies the part inserts/deletes/retypes to `work` and records the parts
// whose existing lineitems must move between months. False when an event
// cannot be applied exactly (full rebuild).
bool ApplyPartEvents(const std::vector<TpchQ14PartEvent> &events, WorkState &work,
                     Transitions &transitions) {
  for (const auto &event : events) {
    const auto cls = static_cast<uint8_t>(event.new_class);
    const auto raw = work.RawOf(event.key);
    const auto current = work.PromoOf(event.key);
    const auto tr = transitions.find(event.key);
    switch (event.kind) {
    case core::MutationKind::Insert:
      if (current != TpchQ14PromoClass::Absent) {
        // Another row of an already inserted key: exact only while the key
        // has no lineitems (fresh); the class stays last-wins like Build.
        if (!(raw & kFresh) || tr != transitions.end())
          return false;
        work.Write(event.key, static_cast<uint8_t>(cls | kFresh));
        break;
      }
      if (tr != transitions.end()) {
        tr->second.to = event.new_class;
        work.SetRaw(event.key, cls);
      } else if ((raw & kDead) || (!(raw & kGone) && event.key > work.BuildMax())) {
        // Known to have no lineitems: nothing to add.
        work.SetRaw(event.key, static_cast<uint8_t>(cls | kFresh));
      } else {
        // Deleted before with lineitems left behind (or a never-seen key
        // inside the built range): add its existing lineitems' revenue.
        transitions.emplace(event.key, Transition{TpchQ14PromoClass::Absent, event.new_class});
        work.SetRaw(event.key, cls);
      }
      work.changed = true;
      break;
    case core::MutationKind::Delete:
      if (current == TpchQ14PromoClass::Absent) {
        // Another row of a key deleted earlier (bag delete): nothing left.
        if (!(raw & (kDead | kGone)))
          return false;
        break;
      }
      if (tr != transitions.end()) {
        tr->second.to = TpchQ14PromoClass::Absent;
        work.SetRaw(event.key, kGone);
      } else if (raw & kFresh) {
        work.SetRaw(event.key, kDead);
      } else {
        transitions.emplace(event.key, Transition{current, TpchQ14PromoClass::Absent});
        work.SetRaw(event.key, kGone);
      }
      work.changed = true;
      break;
    case core::MutationKind::Update:
      if (current == TpchQ14PromoClass::Absent)
        return false;
      if (current == event.new_class)
        break;
      if (tr != transitions.end())
        tr->second.to = event.new_class;
      else if (!(raw & kFresh))
        transitions.emplace(event.key, Transition{current, event.new_class});
      work.Write(event.key, static_cast<uint8_t>(cls | (raw & kFresh)));
      break;
    }
  }
  return true;
}

// Moves the month totals of parts that changed class or presence. False when
// that needs a lineitem scan the batch cannot use (no source, or lineitem
// deltas mixed in).
bool AdjustForTransitions(TpchQ14InitialScan *scan, bool has_lineitem_deltas,
                          const Transitions &transitions, std::vector<Adjust> &adj) {
  std::vector<int64_t> scan_keys;
  for (const auto &[key, tr] : transitions) {
    if (tr.from != tr.to)
      scan_keys.push_back(key);
  }
  if (scan_keys.empty())
    return true;
  if (!scan || has_lineitem_deltas)
    return false;
  scan->ScanPartMonthRevenue(
      scan_keys, [&](int64_t key, int32_t month, int64_t total, int64_t count) {
        const auto it = transitions.find(key);
        if (it == transitions.end())
          return;
        const auto from = it->second.from, to = it->second.to;
        auto &a = AdjustOf(adj, month);
        if (to == TpchQ14PromoClass::Absent) {
          a.total_revenue_raw -= total;
          a.row_count -= count;
        } else if (from == TpchQ14PromoClass::Absent) {
          a.total_revenue_raw += total;
          a.row_count += count;
        }
        if (from == TpchQ14PromoClass::Promo)
          a.promo_revenue_raw -= total;
        if (to == TpchQ14PromoClass::Promo)
          a.promo_revenue_raw += total;
      });
  return true;
}

// A delta's class never changes here, so the totals are summed per slice
// against the unchanged part state; the key-state writes (a lineitem now
// references the key, or exists for a part not yet inserted) follow serially.
void FoldLineitemDeltas(const core::AuxEngineServices &services,
                        const std::vector<TpchQ14LineitemDelta> &deltas, WorkState &work,
                        std::vector<Adjust> &adj) {
  struct DeltaPass {
    std::vector<Adjust> adj;
    std::vector<uint32_t> writes;
  };
  DeltaPass pass;
  core::ParallelReduce(
      services, deltas.size(), kDeltaGrain, pass,
      [&](DeltaPass &acc, std::size_t first, std::size_t last) {
        for (std::size_t i = first; i < last; ++i) {
          const auto &delta = deltas[i];
          const auto raw = work.RawOf(delta.partkey);
          const auto promo = static_cast<TpchQ14PromoClass>(raw & TpchQ14State::kClassMask);
          if ((promo == TpchQ14PromoClass::Absent && !(raw & kGone)) ||
              (promo != TpchQ14PromoClass::Absent && (raw & kFresh)))
            acc.writes.push_back(static_cast<uint32_t>(i));
          if (promo == TpchQ14PromoClass::Absent)
            continue;
          auto &a = AdjustOf(acc.adj, delta.month);
          const auto signed_revenue = static_cast<int64_t>(delta.sign) * delta.revenue_raw;
          a.total_revenue_raw += signed_revenue;
          a.row_count += delta.sign;
          if (promo == TpchQ14PromoClass::Promo)
            a.promo_revenue_raw += signed_revenue;
        }
      },
      [&](DeltaPass &into, DeltaPass &&from) {
        into.writes.insert(into.writes.end(), from.writes.begin(), from.writes.end());
        for (const auto &entry : from.adj)
          AddTotals(AdjustOf(into.adj, entry.month), entry.totals);
      });
  for (const auto &entry : pass.adj)
    AddTotals(AdjustOf(adj, entry.month), entry.totals);
  for (const auto i : pass.writes) {
    const auto partkey = deltas[i].partkey;
    const auto raw = work.RawOf(partkey);
    if ((raw & TpchQ14State::kClassMask) == static_cast<uint8_t>(TpchQ14PromoClass::Absent)) {
      if (!(raw & kGone))
        work.Write(partkey, kGone);
    } else if (raw & kFresh) { // now referenced by lineitems
      work.Write(partkey, static_cast<uint8_t>(raw & ~kFresh));
    }
  }
}

// The published month image an adjustment applies to, and its version.
struct MonthBase {
  core::Version version = 0;
  const TpchQ14StateImage *image = nullptr;
};

const core::StateImage *FindMonthImage(const core::StateSnapshot &snapshot, int32_t month) {
  for (const auto &image : snapshot.partitions)
    if (image.partition == static_cast<core::PartitionId>(month) &&
        image.partition != core::TpchQ14Layout::kPartPromo)
      return &image;
  return nullptr;
}

// Current month totals + versions: from the combined read; months only
// reachable through the scan (not touched by a delta) are read now into
// `extra_snapshot`, which the result points into. False when a month is missing.
bool LoadMonthBases(core::AuxEngineServices &services, const core::StateSnapshot &read,
                    const std::vector<Adjust> &adj, core::StateSnapshot &extra_snapshot,
                    std::vector<MonthBase> &base) {
  base.assign(adj.size(), {});
  if (adj.empty())
    return true;
  std::vector<core::PartitionId> missing;
  for (const auto &entry : adj)
    if (!FindMonthImage(read, entry.month))
      missing.push_back(static_cast<core::PartitionId>(entry.month));
  if (!missing.empty())
    extra_snapshot = services.StateStore().ReadPartitions(services.CurrentVisibility(), missing);
  for (std::size_t i = 0; i < adj.size(); ++i) {
    const auto *image = FindMonthImage(read, adj[i].month);
    if (!image)
      image = FindMonthImage(extra_snapshot, adj[i].month);
    if (!image)
      return false;
    const auto *typed = AsQ14Image(*image, TpchQ14ImageKind::Month);
    if (!typed || typed->month != adj[i].month)
      return false;
    base[i].image = typed;
    base[i].version = image->version;
  }
  return true;
}

} // namespace

bool TpchQ14IsPromoType(std::string_view type) {
  static constexpr const char kPromo[] = "PROMO";
  return type.size() >= sizeof(kPromo) - 1 &&
         type.compare(0, sizeof(kPromo) - 1, kPromo) == 0;
}

int64_t TpchQ14RevenueRaw(int64_t extended_price_raw, int32_t extended_scale,
                          int64_t discount_raw, int32_t discount_scale) {
  if (extended_scale == 2 && discount_scale == 2)
    return extended_price_raw * (100 - discount_raw);
  double price = static_cast<double>(extended_price_raw);
  double discount = static_cast<double>(discount_raw);
  for (int32_t i = 0; i < extended_scale; ++i)
    price /= 10.0;
  for (int32_t i = 0; i < discount_scale; ++i)
    discount /= 10.0;
  return static_cast<int64_t>(std::llround(price * (1.0 - discount) * 10000.0));
}

bool TpchQ14TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ14TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  deltas.clear();
  part_events.clear();
}
bool TpchQ14TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
const std::vector<TpchQ14LineitemDelta> &TpchQ14TxnBuffer::Deltas() const {
  return deltas;
}
void TpchQ14TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  deltas.clear();
  part_events.clear();
}
const std::vector<TpchQ14PartEvent> &TpchQ14TxnBuffer::PartEvents() const {
  return part_events;
}
void TpchQ14TxnBuffer::AddPartEvent(TpchQ14PartEvent event) {
  pending = true;
  part_events.push_back(event);
}
void TpchQ14TxnBuffer::AddDelta(TpchQ14LineitemDelta delta) {
  pending = true;
  deltas.push_back(delta);
}

core::AcceleratorId TpchQ14Module::Id() const { return kId; }
std::vector<core::TableDependency> TpchQ14Module::Dependencies() const {
  return {{config.part_table, {0, 1}, {0, 4}},
          {config.lineitem_table, {0, 1, 2, 3}, {1, 5, 6, 10}}};
}
std::unique_ptr<core::AuxTxnBuffer> TpchQ14Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ14TxnBuffer>();
}

void TpchQ14Module::Build(core::AuxEngineServices &services,
                          core::StateBuilder &builder) {
  auto state = BuildState(services);
  for (const auto &entry : state.month_totals) {
    builder.Write(core::StateImage::Make(
        core::TpchQ14Layout::kAccelerator,
        static_cast<core::PartitionId>(entry.first), 0,
        MakeMonthImage(entry.first, entry.second)));
  }
  state.month_totals.clear(); // the part image carries classes only
  const auto typed_state =
      std::make_shared<const TpchQ14State>(std::move(state));
  builder.Write(core::StateImage::Make(
      core::TpchQ14Layout::kAccelerator, core::TpchQ14Layout::kPartPromo, 0,
      MakeStateImage(TpchQ14ImageKind::PartPromo, 0, typed_state)));
  builder.Finish();
}

core::InvalidationPlan
TpchQ14Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.lineitem_table) {
    core::InvalidationPlan plan;
    plan.scope =
        ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
    return plan;
  }
  if (mutation.table == config.part_table) {
    // Point part writes are maintained locally (PartPromo + month images);
    // StageMaintenance names what the commit touches.
    core::InvalidationPlan plan;
    plan.scope =
        ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
    return plan;
  }
  return {};
}

void TpchQ14Module::AccumulateMutation(const core::MutationBatch &mutation,
                                       core::AuxEngineServices &,
                                       core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ14TxnBuffer &>(base_buffer);
  if (mutation.table == config.part_table) {
    // A part's type decides whether its revenue counts as promo. Point
    // inserts/deletes/retypes are applied locally in StageMaintenance.
    auto read = [&](const core::AuxRow &row, TpchQ14PartEvent &event) {
      std::string type;
      if (!core::IntCell(row, 0, event.key) || !core::StringCell(row, 1, type))
        return false;
      event.new_class = TpchQ14IsPromoType(type) ? TpchQ14PromoClass::Promo
                                                 : TpchQ14PromoClass::NonPromo;
      return true;
    };
    switch (mutation.kind) {
    case core::MutationKind::Insert:
      for (const auto &row : mutation.new_rows) {
        TpchQ14PartEvent event;
        event.kind = core::MutationKind::Insert;
        if (!read(row, event))
          return buffer.MarkRebuild();
        buffer.AddPartEvent(event);
      }
      break;
    case core::MutationKind::Delete:
      for (const auto &row : mutation.old_rows) {
        TpchQ14PartEvent event;
        event.kind = core::MutationKind::Delete;
        if (!read(row, event))
          return buffer.MarkRebuild();
        buffer.AddPartEvent(event);
      }
      break;
    case core::MutationKind::Update:
      if (mutation.old_rows.size() != mutation.new_rows.size())
        return buffer.MarkRebuild();
      for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
        TpchQ14PartEvent old_event, new_event;
        if (!read(mutation.old_rows[i], old_event) ||
            !read(mutation.new_rows[i], new_event) ||
            old_event.key != new_event.key)
          return buffer.MarkRebuild();
        new_event.kind = core::MutationKind::Update;
        buffer.AddPartEvent(new_event);
      }
      break;
    }
    return;
  }
  if (mutation.table != config.lineitem_table)
    return;
  auto add = [&](const core::AuxRow &row, int8_t sign) {
    TpchQ14LineitemInput input;
    if (!ReadLineitem(row, input)) {
      buffer.MarkRebuild();
      return;
    }
    TpchQ14LineitemDelta delta;
    delta.month = MonthKey(input.shipdate_days);
    delta.partkey = input.partkey;
    delta.revenue_raw =
        TpchQ14RevenueRaw(input.extended_price_raw, input.extended_price_scale,
                          input.discount_raw, input.discount_scale);
    delta.sign = sign;
    buffer.AddDelta(delta);
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
TpchQ14Module::StageMaintenance(core::AuxEngineServices &services,
                                const core::InvalidationPlan &,
                                core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ14TxnBuffer &>(base_buffer);
  core::StagedMaintenanceOutcome outcome;
  if (buffer.RequiresRebuild()) {
    core::FinishRebuildAll(outcome, services.ChooseMaintenance(core::MaintenanceRequest::RebuildOnly(
                                        services.ExpectedQueriesBeforeNextWrite())));
    return outcome;
  }
  const auto &events = buffer.PartEvents();
  const auto &deltas = buffer.Deltas();
  if (deltas.empty() && events.empty())
    return outcome;

  // Distinct months of the lineitem deltas (sorted).
  std::vector<int32_t> touched;
  touched.reserve(deltas.size());
  for (const auto &delta : deltas)
    touched.push_back(delta.month);
  std::sort(touched.begin(), touched.end());
  touched.erase(std::unique(touched.begin(), touched.end()), touched.end());

  // Small batches (and every batch carrying part writes) are applied as
  // deltas; a policy-chosen partition rebuild is kept for big batches only.
  core::MaintenanceDecision decision;
  decision.action = core::MaintenanceAction::ApplyDelta;
  if (!deltas.empty()) {
    decision = services.ChooseMaintenance(core::MaintenanceRequest::IncrementalPartitioned(
        deltas.size(), touched.size(), services.ExpectedQueriesBeforeNextWrite()));
    if (!core::ForcedMaintenanceAction() &&
        (deltas.size() < kLocalDeltaLimit || !events.empty()))
      decision.action = core::MaintenanceAction::ApplyDelta;
  }

  // One read for the part classes and the months the deltas touch.
  std::vector<core::PartitionId> wanted;
  wanted.reserve(touched.size() + 1);
  wanted.push_back(core::TpchQ14Layout::kPartPromo);
  for (const auto month : touched)
    wanted.push_back(static_cast<core::PartitionId>(month));
  const auto part_snapshot =
      services.StateStore().ReadPartitions(services.CurrentVisibility(), wanted);
  const TpchQ14State *part_state = nullptr;
  core::Version part_version = 0;
  for (const auto &image : part_snapshot.partitions) {
    if (image.partition == core::TpchQ14Layout::kPartPromo) {
      const auto *typed = AsQ14Image(image, TpchQ14ImageKind::PartPromo);
      part_state = typed ? typed->state.get() : nullptr;
      part_version = image.version;
      break;
    }
  }

  if (decision.action == core::MaintenanceAction::RebuildPartition) {
    if (!RebuildMonths(initial_scan, part_state, touched, outcome))
      core::FinishRebuildAll(outcome);
    return outcome;
  }
  if (decision.action != core::MaintenanceAction::ApplyDelta) {
    core::FinishRebuildAll(outcome, decision);
    return outcome;
  }
  if (!part_state) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }

  WorkState work{part_state, {}};
  Transitions transitions;
  std::vector<Adjust> adj;
  if (!ApplyPartEvents(events, work, transitions) ||
      !AdjustForTransitions(initial_scan, !deltas.empty(), transitions, adj)) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }
  FoldLineitemDeltas(services, deltas, work, adj);

  core::StateSnapshot extra_snapshot;
  std::vector<MonthBase> base;
  if (!LoadMonthBases(services, part_snapshot, adj, extra_snapshot, base)) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }

  if (adj.empty() && !work.changed)
    return outcome;
  outcome.action = core::MaintenanceAction::ApplyDelta;
  outcome.updates.reserve(adj.size() + 1);
  for (std::size_t i = 0; i < adj.size(); ++i) {
    const auto month = adj[i].month;
    auto totals = base[i].image->totals;
    AddTotals(totals, adj[i].totals);
    totals.saw_contribution = totals.row_count > 0;
    outcome.updates.push_back(core::StateImage::Make(
        core::TpchQ14Layout::kAccelerator, static_cast<core::PartitionId>(month),
        base[i].version, MakeMonthImage(month, totals)));
  }
  if (work.changed) {
    outcome.updates.push_back(core::StateImage::Make(
        core::TpchQ14Layout::kAccelerator, core::TpchQ14Layout::kPartPromo, part_version,
        MakeStateImage(TpchQ14ImageKind::PartPromo, 0,
                       std::make_shared<const TpchQ14State>(
                           work.copied ? std::move(work.copy) : TpchQ14State(*part_state)))));
  }
  return outcome;
}

bool TpchQ14Module::CheckValidity(core::AuxEngineServices &services,
                                  const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void TpchQ14Module::Execute(core::AuxEngineServices &services,
                            const core::QueryArguments &arguments,
                            core::AuxRowSink &sink) {
  int32_t start = 0;
  if (!arguments.values.empty() && !arguments.values[0].is_null) {
    if (std::holds_alternative<core::AuxDate>(arguments.values[0].payload))
      start =
          std::get<core::AuxDate>(arguments.values[0].payload).days_since_epoch;
    else if (std::holds_alternative<int64_t>(arguments.values[0].payload))
      start = static_cast<int32_t>(std::get<int64_t>(arguments.values[0].payload));
  }
  const auto month = MonthKey(start);
  TpchQ14MonthTotals totals;
  if (!LoadMonth(services, month, totals)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  EmitResult(totals, sink);
}

TpchQ14State TpchQ14Module::BuildState(core::AuxEngineServices &services) const {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q14 requires a typed initial scan source");
  TpchQ14State state = core::PartitionedBuild<TpchQ14State>(
      services, [] { return TpchQ14State{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanParts(services, parts, [&](std::size_t p, int64_t key, bool promo) {
          const PartRow part{key, promo};
          sink(p, core::SingleRowBatch<PartRow>{part});
        });
      },
      [](TpchQ14State &part_state, const PartRow &part) {
        part_state.build_max_partkey =
            part_state.build_max_partkey == std::numeric_limits<int64_t>::max()
                ? part.key
                : std::max(part_state.build_max_partkey, part.key);
        part_state.SetPromo(part.key, part.promo ? TpchQ14PromoClass::Promo
                                                 : TpchQ14PromoClass::NonPromo);
      },
      [](TpchQ14State &into, TpchQ14State &&from) {
        if (from.build_max_partkey != std::numeric_limits<int64_t>::max())
          into.build_max_partkey =
              into.build_max_partkey == std::numeric_limits<int64_t>::max()
                  ? from.build_max_partkey
                  : std::max(into.build_max_partkey, from.build_max_partkey);
        into.classes.MergeFrom(std::move(from.classes), [](uint8_t mine, uint8_t theirs) {
          return theirs ? theirs : mine; // the last part row of a key wins
        });
        for (const auto &[key, raw] : from.promo_overflow)
          into.promo_overflow[key] = raw;
      });

  state.month_totals = core::PartitionedBuild<std::map<int32_t, TpchQ14MonthTotals>>(
      services, [] { return std::map<int32_t, TpchQ14MonthTotals>{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanLineitems(services, parts,
                                    [&](std::size_t p, const TpchQ14LineitemInput &input) {
                                      sink(p, core::SingleRowBatch<TpchQ14LineitemInput>{input});
                                    });
      },
      [&](std::map<int32_t, TpchQ14MonthTotals> &months, const TpchQ14LineitemInput &input) {
        const auto promo = state.PromoOf(input.partkey);
        if (promo == TpchQ14PromoClass::Absent)
          return;
        const auto revenue =
            TpchQ14RevenueRaw(input.extended_price_raw, input.extended_price_scale,
                              input.discount_raw, input.discount_scale);
        auto &totals = months[MonthKey(input.shipdate_days)];
        totals.total_revenue_raw += revenue;
        totals.row_count++;
        totals.saw_contribution = true;
        if (promo == TpchQ14PromoClass::Promo)
          totals.promo_revenue_raw += revenue;
      },
      [](std::map<int32_t, TpchQ14MonthTotals> &into,
         std::map<int32_t, TpchQ14MonthTotals> &&from) {
        for (const auto &[month, totals] : from) {
          AddTotals(into[month], totals);
          into[month].saw_contribution = true;
        }
      });
  return state;
}

// Validity and the month's image in one linearized observation (a missing
// month, an invalid store or a foreign payload all mean "use the fallback").
bool TpchQ14Module::LoadMonth(core::AuxEngineServices &services, int32_t month,
                              TpchQ14MonthTotals &totals) const {
  thread_local std::vector<core::PartitionId> wanted(1);
  wanted[0] = static_cast<core::PartitionId>(month);
  const auto snapshot = services.StateStore().ReadHitPartitions(
      services.CurrentVisibility(), {Id(), {}}, wanted);
  if (!snapshot.available || snapshot.partitions.size() != 1)
    return false;
  const auto *typed =
      AsQ14Image(snapshot.partitions[0], TpchQ14ImageKind::Month);
  if (!typed || typed->month != month)
    return false;
  totals = typed->totals;
  return true;
}

void TpchQ14Module::EmitResult(const TpchQ14MonthTotals &totals,
                               core::AuxRowSink &sink) const {
  const auto value = TpchQ14FinalizePromoRevenue(totals);
  static const core::OutputSchema kSchema{
      {{"promo_revenue", core::AuxType::Double, true}}};
  sink.Begin(kSchema);
  core::AuxRow row;
  row.values.resize(1);
  row.values[0].type = core::AuxType::Double;
  if (std::isnan(value)) {
    row.values[0].is_null = true;
  } else {
    row.values[0].is_null = false;
    row.values[0].payload = value;
  }
  sink.WriteRow(row);
  sink.End();
}

std::vector<core::CalibrationWrite> TpchQ14Module::CalibrationWrites() const {
  return TpchQ14CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
