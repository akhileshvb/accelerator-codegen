#include "tpch/q14/mysql/aux/module.hpp"
#include "tpch/q14/calibration_writes.hpp"
#include "prims/sql_date.hpp"
#include "prims/sql_like.hpp"
#include "substrate/aux/core/aux_cells.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/partitioned_build.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {

// The single state image's partition id.
constexpr core::PartitionId kStatePartition = 0;
// More affected parts than this per commit: rebuild instead of point scans.
constexpr std::size_t kMaxPointParts = 50000;
// Lineitem deltas per slice below which a commit's deltas are folded inline.
constexpr std::size_t kDeltaGrain = 4096;

std::shared_ptr<const TpchQ14StateImage>
MakeStateImage(std::shared_ptr<const TpchQ14State> state) {
  auto image = std::make_shared<TpchQ14StateImage>();
  image->state = std::move(state);
  return image;
}

const TpchQ14StateImage *AsQ14Image(const core::StateImage &image) {
  return image.As<TpchQ14StateImage>();
}

TpchQ14PromoClass ClassOfType(const std::string &type) {
  return TpchQ14IsPromoType(type) ? TpchQ14PromoClass::Promo
                                  : TpchQ14PromoClass::NonPromo;
}

// l_extendedprice * (1 - l_discount) at scale 4 (price and discount scale 2).
__int128 RevenueRaw(int64_t extended_price_raw, int32_t extended_scale,
                    int64_t discount_raw, int32_t discount_scale) {
  return static_cast<__int128>(core::ScaleToRaw(extended_price_raw, extended_scale, 2)) *
         (100 - core::ScaleToRaw(discount_raw, discount_scale, 2));
}

bool ReadLineitem(const core::AuxRow &row, TpchQ14LineitemDelta &delta,
                  int8_t sign) {
  int64_t extended_raw = 0, discount_raw = 0;
  int32_t extended_scale = 0, discount_scale = 0;
  if (!core::IntCell(row, 0, delta.partkey) ||
      !core::DecimalCell(row, 1, extended_raw, extended_scale) ||
      !core::DecimalCell(row, 2, discount_raw, discount_scale) ||
      !core::DateCell(row, 3, delta.shipdate_days))
    return false;
  delta.revenue_raw =
      RevenueRaw(extended_raw, extended_scale, discount_raw, discount_scale);
  delta.sign = sign;
  return true;
}

} // namespace

bool TpchQ14IsPromoType(const std::string &type) {
  // p_type LIKE 'PROMO%' under MySQL's default (case-insensitive) collation.
  static const auto kPromo = core::sql_like::LikeMatcher::Compile(
      "PROMO%", '\\', /*case_insensitive=*/true);
  return kPromo.Match(type);
}

void TpchQ14State::AddRevenue(int32_t day, TpchQ14PromoClass promo,
                              __int128 revenue, int64_t count) {
  if (promo == TpchQ14PromoClass::Absent || (revenue == 0 && count == 0))
    return;
  days.Update(day, [&](TpchQ14DayTotals &cell) {
    cell.total_revenue_raw += revenue;
    if (promo == TpchQ14PromoClass::Promo)
      cell.promo_revenue_raw += revenue;
    cell.count += count;
  });
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
const std::vector<TpchQ14PartEvent> &TpchQ14TxnBuffer::PartEvents() const {
  return part_events;
}
void TpchQ14TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  deltas.clear();
  part_events.clear();
}
void TpchQ14TxnBuffer::AddDelta(TpchQ14LineitemDelta delta) {
  pending = true;
  deltas.push_back(delta);
}
void TpchQ14TxnBuffer::AddPartEvent(TpchQ14PartEvent event) {
  pending = true;
  part_events.push_back(event);
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
  builder.Write(core::StateImage::Make(
      core::TpchQ14Layout::kAccelerator, kStatePartition, 0,
      MakeStateImage(BuildState(services))));
  builder.Finish();
}

core::InvalidationPlan
TpchQ14Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.part_table ||
      mutation.table == config.lineitem_table) {
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
    // A part row appearing, disappearing or changing type moves its lineitems'
    // revenue (re-aggregated per part at maintenance, not a rebuild).
    auto part_of = [&](const core::AuxRow &row, int64_t &key,
                       TpchQ14PromoClass &promo) {
      std::string type;
      if (!core::IntCell(row, 0, key) || !core::StringCell(row, 1, type))
        return false;
      promo = ClassOfType(type);
      return true;
    };
    int64_t key = 0;
    TpchQ14PromoClass promo = TpchQ14PromoClass::Absent;
    switch (mutation.kind) {
    case core::MutationKind::Insert:
      for (const auto &row : mutation.new_rows) {
        if (!part_of(row, key, promo)) {
          buffer.MarkRebuild();
          return;
        }
        buffer.AddPartEvent({key, promo});
      }
      break;
    case core::MutationKind::Delete:
      for (const auto &row : mutation.old_rows) {
        if (!part_of(row, key, promo)) {
          buffer.MarkRebuild();
          return;
        }
        buffer.AddPartEvent({key, TpchQ14PromoClass::Absent});
      }
      break;
    case core::MutationKind::Update:
      if (mutation.old_rows.size() != mutation.new_rows.size()) {
        buffer.MarkRebuild();
        return;
      }
      for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
        int64_t old_key = 0;
        if (!part_of(mutation.old_rows[i], old_key, promo) ||
            !part_of(mutation.new_rows[i], key, promo)) {
          buffer.MarkRebuild();
          return;
        }
        if (old_key != key) // key change = delete + insert
          buffer.AddPartEvent({old_key, TpchQ14PromoClass::Absent});
        buffer.AddPartEvent({key, promo});
      }
      break;
    }
    return;
  }
  if (mutation.table != config.lineitem_table)
    return;
  auto add = [&](const core::AuxRow &row, int8_t sign) {
    TpchQ14LineitemDelta delta;
    if (!ReadLineitem(row, delta, sign)) {
      buffer.MarkRebuild();
      return;
    }
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
  if (!buffer.RequiresRebuild() && buffer.Deltas().empty() &&
      buffer.PartEvents().empty())
    return outcome;

  // The state is one copy-on-write image: applying the deltas is always
  // cheaper than a rebuild, which is chosen only when the deltas could not be
  // read, the state is missing, too many parts changed, or it is forced.
  std::shared_ptr<TpchQ14State> next;
  core::Version version = 0;
  std::unordered_map<int64_t, TpchQ14PromoClass> final_class;
  std::vector<int64_t> affected;
  if (!buffer.RequiresRebuild()) {
    const auto snapshot = services.StateStore().ReadPartitions(
        services.CurrentVisibility(), {kStatePartition});
    for (const auto &image : snapshot.partitions) {
      const auto *typed = AsQ14Image(image);
      if (image.partition == kStatePartition && typed && typed->state) {
        next = std::make_shared<TpchQ14State>(*typed->state); // COW copy
        version = image.version;
      }
    }
    if (next) {
      for (const auto &event : buffer.PartEvents())
        final_class[event.partkey] = event.promo; // last event wins
      for (const auto &entry : final_class)
        if (next->PromoOf(entry.first) != entry.second)
          affected.push_back(entry.first);
    }
  }
  auto request = core::MaintenanceRequest::IncrementalSingleState(
      buffer.Deltas().size() + affected.size(), services.ExpectedQueriesBeforeNextWrite());
  request.incremental_supported =
      next != nullptr && affected.size() <= kMaxPointParts && initial_scan;
  const auto decision = services.ChooseMaintenance(request);
  if (!next || affected.size() > kMaxPointParts ||
      decision.action != core::MaintenanceAction::ApplyDelta) {
    core::FinishRebuildAll(outcome, decision);
    return outcome;
  }

  // Parts whose class changed: their lineitems move between {absent, non-promo,
  // promo}. The table now holds this transaction's lineitem writes too, so the
  // post-write revenue per (part, day) comes from one indexed point scan, the
  // pre-write revenue is that minus this transaction's lineitem deltas, and
  // those deltas are not applied again below.
  struct DayRevenue {
    __int128 post = 0, pre = 0;
    int64_t post_count = 0, pre_count = 0;
  };
  std::map<std::pair<int64_t, int32_t>, DayRevenue> moved;
  if (!affected.empty()) {
    initial_scan->ScanPartDayRevenueSerial(
        affected, [&](const TpchQ14PartDayRevenue &row) {
          auto &cell = moved[{row.partkey, row.shipdate_days}];
          cell.post += row.revenue_raw;
          cell.post_count += row.count;
          cell.pre += row.revenue_raw;
          cell.pre_count += row.count;
        });
  }
  std::unordered_set<int64_t> is_affected;
  for (const auto key : affected)
    is_affected.insert(key);
  for (const auto &delta : buffer.Deltas()) {
    if (!is_affected.count(delta.partkey))
      continue;
    auto &cell = moved[{delta.partkey, delta.shipdate_days}];
    cell.pre -= delta.sign * delta.revenue_raw;
    cell.pre_count -= delta.sign;
  }
  for (const auto &entry : moved) {
    const auto key = entry.first.first;
    const auto day = entry.first.second;
    next->AddRevenue(day, final_class.at(key), entry.second.post,
                     entry.second.post_count);
    next->AddRevenue(day, next->PromoOf(key), -entry.second.pre,
                     -entry.second.pre_count);
  }
  for (const auto key : affected)
    next->SetPromo(key, final_class.at(key));

  // Lineitems of unaffected parts: -old +new with the part's (unchanged) class.
  // Slices sum per day against the finished part classes; the days are applied after.
  const auto &deltas = buffer.Deltas();
  std::map<int32_t, TpchQ14DayTotals> per_day;
  core::ParallelReduce(
      services, deltas.size(), kDeltaGrain, per_day,
      [&](std::map<int32_t, TpchQ14DayTotals> &acc, std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
          const auto &delta = deltas[i];
          if (is_affected.count(delta.partkey))
            continue;
          const auto promo = next->PromoOf(delta.partkey);
          if (promo == TpchQ14PromoClass::Absent)
            continue;
          auto &cell = acc[delta.shipdate_days];
          const __int128 revenue = delta.sign * delta.revenue_raw;
          cell.total_revenue_raw += revenue;
          if (promo == TpchQ14PromoClass::Promo)
            cell.promo_revenue_raw += revenue;
          cell.count += delta.sign;
        }
      },
      [](std::map<int32_t, TpchQ14DayTotals> &into, std::map<int32_t, TpchQ14DayTotals> &&from) {
        for (const auto &[day, cell] : from) {
          auto &sum = into[day];
          sum.total_revenue_raw += cell.total_revenue_raw;
          sum.promo_revenue_raw += cell.promo_revenue_raw;
          sum.count += cell.count;
        }
      });
  for (const auto &[day, cell] : per_day)
    next->days.Update(day, [&](TpchQ14DayTotals &into) {
      into.total_revenue_raw += cell.total_revenue_raw;
      into.promo_revenue_raw += cell.promo_revenue_raw;
      into.count += cell.count;
    });

  outcome.action = core::MaintenanceAction::ApplyDelta;
  outcome.updates.push_back(core::StateImage::Make(
      core::TpchQ14Layout::kAccelerator, kStatePartition, version,
      MakeStateImage(std::move(next))));
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
  // ReadHit: validity + the one immutable state image under one visibility.
  const auto snapshot = services.StateStore().ReadHitPartitions(
      services.CurrentVisibility(), {Id(), {}}, {kStatePartition});
  const auto *typed = snapshot.available && snapshot.partitions.size() == 1
                          ? AsQ14Image(snapshot.partitions[0])
                          : nullptr;
  if (!typed || !typed->state) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  Answer(*typed->state, arguments, sink);
}

std::shared_ptr<TpchQ14State>
TpchQ14Module::BuildState(core::AuxEngineServices &services) const {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q14 requires a typed initial scan source");
  auto state = std::make_shared<TpchQ14State>();
  using PartClasses = core::AnyKeyCowArray<uint8_t, 16>;
  state->part_class = core::PartitionedBuild<PartClasses>(
      services, [] { return PartClasses{0}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanParts(
            services, parts, [&](std::size_t partition, const TpchQ14PartInput &input) {
              sink(partition, core::SingleRowBatch<TpchQ14PartInput>{input});
            });
      },
      [](PartClasses &into, const TpchQ14PartInput &input) {
        into.Set(input.key, static_cast<uint8_t>(ClassOfType(input.type)));
      },
      [](PartClasses &into, PartClasses &&from) {
        into.MergeFrom(std::move(from), [](uint8_t mine, uint8_t theirs) {
          return theirs ? theirs : mine; // the last part row of a key wins
        });
      },
      core::BuildPartitions(services));
  // Each partition joins its own range of lineitems with every part (the engine
  // repeats the part side per partition), so there is one partition per worker.
  const auto days = core::PartitionedBuild<std::vector<TpchQ14DayAggregate>>(
      services, [] { return std::vector<TpchQ14DayAggregate>{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanDayAggregates(
            services, parts, [&](std::size_t partition, const TpchQ14DayAggregate &input) {
              sink(partition, core::SingleRowBatch<TpchQ14DayAggregate>{input});
            });
      },
      [](std::vector<TpchQ14DayAggregate> &into, const TpchQ14DayAggregate &agg) {
        into.push_back(agg);
      },
      [](std::vector<TpchQ14DayAggregate> &into, std::vector<TpchQ14DayAggregate> &&from) {
        into.insert(into.end(), from.begin(), from.end());
      },
      services.WorkerCount());
  for (const auto &agg : days)
    state->days.Update(agg.shipdate_days, [&](TpchQ14DayTotals &cell) {
      cell.total_revenue_raw += agg.total_revenue_raw;
      cell.promo_revenue_raw += agg.promo_revenue_raw;
      cell.count += agg.count;
    });
  return state;
}

void TpchQ14Module::Answer(const TpchQ14State &state,
                           const core::QueryArguments &arguments,
                           core::AuxRowSink &sink) const {
  // shipdate in [D, D + INTERVAL '1' MONTH): day-exact (not month-aligned),
  // end-of-month clamped. A NULL date makes the predicate NULL: no rows.
  TpchQ14DayTotals totals;
  if (!arguments.values.empty() && !arguments.values[0].is_null) {
    const auto &payload = arguments.values[0].payload;
    int64_t start = 0;
    bool have = true;
    if (std::holds_alternative<core::AuxDate>(payload))
      start = std::get<core::AuxDate>(payload).days_since_epoch;
    else if (std::holds_alternative<int64_t>(payload))
      start = std::get<int64_t>(payload);
    else
      have = false;
    if (have) {
      const int64_t end = core::sql_date::AddMonths(start, 1);
      const auto add = [&](int sign) {
        return [&totals, sign](const TpchQ14DayTotals &cell) {
          totals.total_revenue_raw += sign * cell.total_revenue_raw;
          totals.promo_revenue_raw += sign * cell.promo_revenue_raw;
          totals.count += sign * cell.count;
        };
      };
      if (start < end) {
        state.days.VisitPrefix(end - 1, add(1));
        state.days.VisitPrefix(start - 1, add(-1));
      }
    }
  }
  EmitResult(totals, sink);
}

void TpchQ14Module::EmitResult(const TpchQ14DayTotals &totals,
                               core::AuxRowSink &sink) const {
  std::string text;
  const bool defined = TpchQ14FinalizePromoRevenue(totals, text);
  sink.Begin({{{"promo_revenue", core::AuxType::String, true}}});
  if (defined)
    sink.WriteRow({{core::AuxValue::Of(core::AuxType::String, text)}});
  else
    sink.WriteRow({{core::AuxValue::Null(core::AuxType::String)}});
  sink.End();
}

std::vector<core::CalibrationWrite> TpchQ14Module::CalibrationWrites() const {
  return TpchQ14CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
