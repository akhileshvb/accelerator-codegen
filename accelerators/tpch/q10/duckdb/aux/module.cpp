#include "tpch/q10/duckdb/aux/module.hpp"
#include "tpch/q10/calibration_writes.hpp"
#include "prims/sql_date.hpp"
#include "substrate/aux/core/aux_cells.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/parallel_sort.hpp"
#include "substrate/aux/core/partitioned_build.hpp"
#include "tpch/common/dealt_rows.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include "tpch/q10/layout.hpp"

namespace accelerator_codegen::aux_state::query {
namespace {

namespace sql_date = core::sql_date;

// Lineitem deltas per slice when resolving them against the orders map (untimed).
constexpr std::size_t kResolveGrain = 4096;
// Order deltas per sorted slice, and shards per task, when applying order deltas.
constexpr std::size_t kOrderSortGrain = 65536;
constexpr std::size_t kShardRunGrain = 64;
// Month or window images per slice when building or publishing (untimed).
constexpr std::size_t kImageGrain = 1;
// Month tables per slice when sorting them after the scan (untimed).
constexpr std::size_t kMonthTableGrain = 1;
// Month delta entries below which the per-month / per-window images are built inline (untimed).
constexpr std::size_t kImageOpThreshold = 2048;
// Customer changes kept in the copy-on-write overlay before it is folded into the base.
constexpr std::size_t kCustomerOverlayCap = 2048;
// Windows sum this many consecutive months.
constexpr int32_t kWindowMonths = 3;

constexpr uint32_t kMetaPartition = 4;
constexpr uint32_t kFirstWindow = 3000000; // + YYYYMM of the window start

// A decimal cell, or a double cell rounded to scale 2.
bool DecimalOrDoubleCell(const core::AuxRow &row, std::size_t index, int64_t &raw,
                         int32_t &scale) {
  if (core::DecimalCell(row, index, raw, scale))
    return true;
  double value = 0.0;
  if (!core::DoubleCell(row, index, value))
    return false;
  raw = static_cast<int64_t>(std::llround(value * 100.0));
  scale = 2;
  return true;
}

int32_t MonthKey(int32_t days) {
  const auto civil = sql_date::CivilFromDays(days);
  return civil.year * 100 + civil.month;
}

int32_t MonthStart(int32_t month_key) {
  return static_cast<int32_t>(
      sql_date::DaysFromCivil(month_key / 100, month_key % 100, 1));
}

std::shared_ptr<const TpchQ10StateImage>
MakeStateImage(TpchQ10ImageKind kind, int32_t month,
               const std::shared_ptr<const TpchQ10State> &state) {
  auto image = std::make_shared<TpchQ10StateImage>();
  image->kind = kind;
  image->month = month;
  image->state = state;
  return image;
}

const TpchQ10StateImage *AsQ10Image(const core::StateImage &image,
                                    TpchQ10ImageKind kind) {
  const auto *payload = image.As<TpchQ10StateImage>();
  if (!payload || payload->kind != kind)
    return nullptr;
  if ((kind == TpchQ10ImageKind::Side || kind == TpchQ10ImageKind::Orders) &&
      !payload->state)
    return nullptr;
  return payload;
}

// Month ordinal arithmetic on YYYYMM keys.
int32_t MonthOrd(int32_t month_key) {
  return (month_key / 100) * 12 + (month_key % 100) - 1;
}
int32_t MonthFromOrd(int32_t ord) { return (ord / 12) * 100 + ord % 12 + 1; }

bool IsReturned(const core::AuxRow &row) {
  std::string flag;
  return core::StringCell(row, 3, flag) && flag == "R";
}

bool ReadLineitem(const core::AuxRow &row, int64_t &orderkey,
                  int64_t &revenue_raw, bool &returned) {
  int64_t extended_raw = 0, discount_raw = 0;
  int32_t extended_scale = 0, discount_scale = 0;
  if (!core::IntCell(row, 0, orderkey) ||
      !DecimalOrDoubleCell(row, 1, extended_raw, extended_scale) ||
      !DecimalOrDoubleCell(row, 2, discount_raw, discount_scale))
    return false;
  if (extended_scale == 2 && discount_scale == 2) {
    revenue_raw = extended_raw * (100 - discount_raw);
  } else {
    double price = 0.0, discount = 0.0;
    if (!core::DoubleCell(row, 1, price) || !core::DoubleCell(row, 2, discount))
      return false;
    revenue_raw = TpchQ10RevenueRaw(price, discount);
  }
  returned = IsReturned(row);
  return true;
}

// Row layout: o_orderkey, o_custkey, o_orderdate
bool ReadOrder(const core::AuxRow &row, int64_t &orderkey, int64_t &custkey,
               int32_t &orderdate_days) {
  return core::IntCell(row, 0, orderkey) && core::IntCell(row, 1, custkey) &&
         core::DateCell(row, 2, orderdate_days);
}

uint32_t MonthPartition(int32_t month) {
  return static_cast<uint32_t>(core::TpchQ10Layout::kFirstMonth +
                               static_cast<uint64_t>(month));
}

uint32_t WindowPartition(int32_t start_month) {
  return kFirstWindow + static_cast<uint32_t>(start_month);
}

// Fold the overlay into a fresh base (deep copy; only on compaction/encode).
std::shared_ptr<const TpchQ10SideBase>
CompactSideBase(const TpchQ10State &state) {
  auto base = std::make_shared<TpchQ10SideBase>(*state.side_base);
  if (state.customer_overlay) {
    for (const auto &[key, slot] : *state.customer_overlay) {
      if (slot.present)
        base->SetCustomer(key, slot.customer);
      else
        base->EraseCustomer(key);
    }
  }
  return base;
}

std::shared_ptr<const TpchQ10State>
ApplyCustomerDeltas(const TpchQ10State &prev,
                    const std::vector<TpchQ10CustomerDelta> &deltas) {
  auto overlay = prev.customer_overlay
                     ? std::make_shared<TpchQ10CustomerOverlay>(
                           *prev.customer_overlay)
                     : std::make_shared<TpchQ10CustomerOverlay>();
  for (const auto &delta : deltas) {
    if (!core::DenseIdArray<TpchQ10CustomerRank>::Fits(delta.custkey))
      return nullptr;
    auto &slot = (*overlay)[delta.custkey];
    if (delta.sign < 0) {
      slot.present = 0;
      slot.customer = TpchQ10Customer{};
    } else {
      slot.present = 1;
      slot.customer = delta.customer;
    }
  }
  auto next = std::make_shared<TpchQ10State>();
  next->nations = prev.nations;
  if (overlay->size() > kCustomerOverlayCap) {
    TpchQ10State folded;
    folded.side_base = prev.side_base;
    folded.customer_overlay = std::move(overlay);
    next->side_base = CompactSideBase(folded);
  } else {
    next->side_base = prev.side_base;
    next->customer_overlay = std::move(overlay);
  }
  return next;
}

// Sum of the key-sorted month tables `parts` (nonzero sums only), key-sorted.
void SumMonths(const std::vector<const std::vector<TpchQ10KV> *> &parts,
               std::vector<TpchQ10KV> &out) {
  out.clear();
  std::vector<std::size_t> pos(parts.size(), 0);
  for (;;) {
    int64_t best = std::numeric_limits<int64_t>::max();
    bool any = false;
    for (std::size_t i = 0; i < parts.size(); ++i)
      if (parts[i] && pos[i] < parts[i]->size()) {
        best = std::min(best, (*parts[i])[pos[i]].key);
        any = true;
      }
    if (!any)
      break;
    int64_t sum = 0;
    for (std::size_t i = 0; i < parts.size(); ++i)
      if (parts[i] && pos[i] < parts[i]->size() &&
          (*parts[i])[pos[i]].key == best) {
        sum += (*parts[i])[pos[i]].val;
        ++pos[i];
      }
    if (sum != 0)
      out.push_back({best, sum});
  }
}

// Key-sorted, duplicate keys summed, zero sums dropped.
void SortCoalesce(std::vector<TpchQ10KV> &v) {
  std::sort(v.begin(), v.end(), TpchQ10ByKey{});
  std::size_t w = 0;
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (w > 0 && v[w - 1].key == v[i].key)
      v[w - 1].val += v[i].val;
    else
      v[w++] = v[i];
  }
  v.resize(w);
  v.erase(std::remove_if(v.begin(), v.end(),
                         [](const TpchQ10KV &e) { return e.val == 0; }),
          v.end());
}

std::shared_ptr<const TpchQ10StateImage> MakeMonthImage(int32_t month,
                                                        TpchQ10KeyIndex data) {
  auto image = std::make_shared<TpchQ10StateImage>();
  image->kind = TpchQ10ImageKind::Month;
  image->month = month;
  image->by_key = std::move(data);
  return image;
}

std::shared_ptr<const TpchQ10StateImage> MakeWindowImage(int32_t start,
                                                         TpchQ10RankIndex data) {
  auto image = std::make_shared<TpchQ10StateImage>();
  image->kind = TpchQ10ImageKind::Window;
  image->month = start;
  image->ranked = std::move(data);
  return image;
}

std::shared_ptr<const TpchQ10StateImage> MakeMetaImage(int32_t lo, int32_t hi) {
  auto image = std::make_shared<TpchQ10StateImage>();
  image->kind = TpchQ10ImageKind::Meta;
  image->lo_month = lo;
  image->hi_month = hi;
  return image;
}

TpchQ10RankIndex RankedFromSums(std::vector<TpchQ10KV> &&sums) {
  std::sort(sums.begin(), sums.end(), TpchQ10ByRank{});
  return TpchQ10RankIndex::FromSorted(std::move(sums));
}

// Delta maintenance of the customer side, the month images and the window
// images for one commit. Run() is false when the delta cannot be applied
// exactly and the state must rebuild; otherwise `outcome` is complete.
class MonthMaintainer {
public:
  MonthMaintainer(core::AuxEngineServices &services, const TpchQ10TxnBuffer &buffer,
                  TpchQ10InitialScan *initial_scan,
                  core::StagedMaintenanceOutcome &outcome)
      : services(services), buffer(buffer), initial_scan(initial_scan),
        outcome(outcome), visibility(services.CurrentVisibility()) {}

  bool Run() {
    // Customer writes touch only the Side partition (revenue is keyed by custkey
    // and never joins customer): copy-on-write overlay, compacted when large.
    if (!buffer.CustomerDeltas().empty() && !StageCustomerSide())
      return false;
    if (buffer.Deltas().empty() && buffer.OrderDeltas().empty()) {
      outcome.action = core::MaintenanceAction::ApplyDelta;
      outcome.updates.push_back(std::move(*side_update));
      return true;
    }
    if (!OrderChangesAreDeltas() || !LoadOrders() || !ResolveLineitemDeltas())
      return false;

    const auto decision = services.ChooseMaintenance(
        core::MaintenanceRequest::IncrementalPartitioned(
            buffer.Deltas().size() + buffer.OrderDeltas().size(),
            std::max<std::size_t>(1, rebuild_months.size()),
            services.ExpectedQueriesBeforeNextWrite()));
    if (decision.action != core::MaintenanceAction::ApplyDelta &&
        decision.action != core::MaintenanceAction::RebuildPartition) {
      core::FinishRebuildAll(outcome, decision);
      return true;
    }
    const bool rescan = decision.action == core::MaintenanceAction::RebuildPartition;
    if (!LoadMonthRange() || !SelectTouchedImages(rescan))
      return false;
    if (rescan)
      return RescanMonths();
    if (!month_deltas.empty() && !ApplyMonthDeltas())
      return false;
    PublishOrders(order_version, /*erase_after_deltas=*/true);
    if (side_update) {
      outcome.action = core::MaintenanceAction::ApplyDelta;
      outcome.updates.push_back(std::move(*side_update));
    }
    return true;
  }

private:
  bool StageCustomerSide() {
    const auto snapshot = services.StateStore().ReadPartitions(
        visibility, {core::TpchQ10Layout::kCustomerDimensions});
    const TpchQ10StateImage *typed = nullptr;
    core::Version side_version = 0;
    for (const auto &image : snapshot.partitions) {
      if (image.partition == core::TpchQ10Layout::kCustomerDimensions) {
        typed = AsQ10Image(image, TpchQ10ImageKind::Side);
        side_version = image.version;
        break;
      }
    }
    if (!typed)
      return false;
    const auto next = ApplyCustomerDeltas(*typed->state, buffer.CustomerDeltas());
    if (!next)
      return false;
    side_update = core::StateImage::Make(
        core::TpchQ10Layout::kAccelerator,
        core::TpchQ10Layout::kCustomerDimensions, side_version,
        MakeStateImage(TpchQ10ImageKind::Side, 0, next));
    return true;
  }

  // An order whose (custkey, orderdate) changes moves all its lineitems'
  // revenue, which lineitem deltas cannot express: rebuild. A pure delete is a
  // delta only when the lineitem deltas cancel its revenue, i.e. no returned
  // lineitem of that order remains in the base table (RF2).
  bool OrderChangesAreDeltas() const {
    struct Change {
      bool has_old = false, has_new = false;
      TpchQ10Order old_order, new_order;
    };
    std::unordered_map<int64_t, Change> changes;
    for (const auto &delta : buffer.OrderDeltas()) {
      auto &c = changes[delta.orderkey];
      const TpchQ10Order order{delta.custkey, delta.orderdate_days};
      if (delta.sign < 0) {
        c.has_old = true;
        c.old_order = order;
      } else {
        c.has_new = true;
        c.new_order = order;
      }
    }
    std::vector<int64_t> deleted_only;
    for (const auto &[key, c] : changes) {
      if (c.has_old && c.has_new) {
        if (c.old_order.custkey != c.new_order.custkey ||
            c.old_order.orderdate_days != c.new_order.orderdate_days)
          return false;
      } else if (c.has_old) {
        deleted_only.push_back(key);
      }
    }
    if (deleted_only.empty())
      return true;
    std::set<int64_t> remaining;
    return initial_scan &&
           initial_scan->OrdersWithReturnedLineitems(deleted_only, remaining) &&
           remaining.empty();
  }

  // Runs op(delta) for the order deltas of `sign`, shards in parallel (a shard's
  // deltas keep their arrival order). False when there was none.
  template <class Op> bool ForOrderDeltasByShard(int sign, Op &&op) {
    const auto &deltas = buffer.OrderDeltas();
    const auto &orders = order_metadata->orders;
    std::vector<std::pair<std::size_t, uint32_t>> by_shard; // (shard, delta index)
    for (uint32_t i = 0; i < deltas.size(); ++i)
      if ((deltas[i].sign > 0) == (sign > 0))
        by_shard.emplace_back(orders.ShardOf(deltas[i].orderkey), i);
    const auto shard_of = [](const auto &entry) { return entry.first; };
    core::ParallelStableSort(
        services, by_shard, [&](const auto &l, const auto &r) { return l.first < r.first; },
        kOrderSortGrain);
    const auto runs = core::RunStarts(by_shard, shard_of);
    core::ParallelEach(services, runs.size() - 1, kShardRunGrain, [&](std::size_t run) {
      for (std::size_t k = runs[run]; k < runs[run + 1]; ++k)
        op(deltas[by_shard[k].second]);
    });
    return !by_shard.empty();
  }

  // Loads a private copy of the Orders partition when the build kept one, and
  // applies the inserts to it. SQL-pushdown builds skip it and resolve via
  // LookupOrders; publishing a sparse RF-only map would break later lookups
  // for baseline orderkeys.
  bool LoadOrders() {
    const auto snapshot = services.StateStore().ReadPartitions(
        visibility, {core::TpchQ10Layout::kOrderMetadata});
    for (const auto &image : snapshot.partitions) {
      if (image.partition == core::TpchQ10Layout::kOrderMetadata) {
        const auto *typed = AsQ10Image(image, TpchQ10ImageKind::Orders);
        if (!typed)
          return false;
        order_metadata = std::make_shared<TpchQ10State>(*typed->state);
        order_version = image.version;
        break;
      }
    }
    if (order_metadata && ForOrderDeltasByShard(+1, [&](const auto &delta) {
          order_metadata->orders.Upsert(
              delta.orderkey, TpchQ10Order{delta.custkey, delta.orderdate_days});
        }))
      orders_changed = true;
    return order_metadata || buffer.Deltas().empty() || LookupTouchedOrders();
  }

  // Resolves only the touched keys. Same-txn RF1 inserts (and RF2 deletes,
  // which carry their old values) are taken from the order deltas themselves.
  bool LookupTouchedOrders() {
    for (const auto &delta : buffer.OrderDeltas())
      overlay_orders[delta.orderkey] =
          TpchQ10Order{delta.custkey, delta.orderdate_days};
    std::vector<int64_t> orderkeys;
    orderkeys.reserve(buffer.Deltas().size());
    for (const auto &delta : buffer.Deltas()) {
      if (overlay_orders.find(delta.orderkey) == overlay_orders.end())
        orderkeys.push_back(delta.orderkey);
    }
    std::unordered_map<int64_t, TpchQ10Order> looked_up;
    if (!orderkeys.empty() &&
        (!initial_scan || !initial_scan->LookupOrders(orderkeys, looked_up)))
      return false;
    for (const auto &[key, order] : looked_up)
      overlay_orders.emplace(key, order);
    return true;
  }

  const TpchQ10Order *FindOrder(int64_t orderkey) const {
    if (order_metadata)
      return order_metadata->orders.Find(orderkey);
    const auto it = overlay_orders.find(orderkey);
    return it == overlay_orders.end() ? nullptr : &it->second;
  }

  // Fills rebuild_months and month_deltas: month -> (custkey, signed revenue).
  bool ResolveLineitemDeltas() {
    for (const auto &delta : buffer.OrderDeltas())
      rebuild_months.insert(MonthKey(delta.orderdate_days));
    if (buffer.Deltas().empty())
      return true;
    struct Resolved {
      int32_t month;
      int64_t custkey, revenue;
    };
    struct Resolution {
      std::vector<Resolved> deltas;
      bool ok = true;
    } resolution;
    const auto &deltas = buffer.Deltas();
    core::ParallelReduce(
        services, deltas.size(), kResolveGrain, resolution,
        [&](Resolution &acc, std::size_t begin, std::size_t end) {
          for (std::size_t i = begin; i < end && acc.ok; ++i) {
            const TpchQ10Order *order = FindOrder(deltas[i].orderkey);
            if (!order) {
              acc.ok = false;
              return;
            }
            acc.deltas.push_back(
                {MonthKey(order->orderdate_days), order->custkey,
                 static_cast<int64_t>(deltas[i].sign) * deltas[i].revenue_raw});
          }
        },
        [](Resolution &into, Resolution &&from) {
          into.ok = into.ok && from.ok;
          into.deltas.insert(into.deltas.end(), from.deltas.begin(),
                             from.deltas.end());
        });
    if (!resolution.ok)
      return false;
    for (const auto &d : resolution.deltas) {
      rebuild_months.insert(d.month);
      month_deltas[d.month].emplace_back(d.custkey, d.revenue);
      ++delta_entries;
    }
    return true;
  }

  // Month range of the published images (Meta partition).
  bool LoadMonthRange() {
    const auto snapshot =
        services.StateStore().ReadPartitions(visibility, {kMetaPartition});
    bool have_meta = false;
    for (const auto &image : snapshot.partitions) {
      if (image.partition != kMetaPartition)
        continue;
      const auto *typed = AsQ10Image(image, TpchQ10ImageKind::Meta);
      if (!typed)
        return false;
      if (typed->hi_month >= typed->lo_month && typed->hi_month > 0) {
        lo_ord = MonthOrd(typed->lo_month);
        hi_ord = MonthOrd(typed->hi_month);
      }
      have_meta = true;
    }
    return have_meta;
  }

  // A touched month outside the imaged range needs new month / window images:
  // rebuild (rare: the data's month range is fixed between rebuilds). A rescan
  // touches every order / lineitem month; deltas only the months that carry a
  // revenue delta. Also collects the windows containing a touched month and
  // the months they sum.
  bool SelectTouchedImages(bool rescan) {
    if (rescan)
      touched_months = rebuild_months;
    else
      for (const auto &entry : month_deltas)
        touched_months.insert(entry.first);
    for (const auto month : touched_months) {
      const auto o = MonthOrd(month);
      if (o < lo_ord || o > hi_ord)
        return false;
    }
    for (const auto month : touched_months)
      for (int32_t k = 0; k < kWindowMonths; ++k)
        touched_window_ords.insert(MonthOrd(month) - k);
    for (const auto w : touched_window_ords)
      for (int32_t k = 0; k < kWindowMonths; ++k) {
        const auto o = w + k;
        if (o >= lo_ord && o <= hi_ord)
          needed_month_ords.insert(o);
      }
    return true;
  }

  // Rescans every month the affected windows sum; republishes the touched
  // months and all affected windows.
  bool RescanMonths() {
    std::set<int32_t> scan_months;
    for (const auto o : needed_month_ords)
      scan_months.insert(MonthFromOrd(o));
    std::map<int32_t, std::vector<TpchQ10KV>> scanned;
    if (!initial_scan || scan_months.empty() ||
        !initial_scan->ScanReturnedCustomerRevenueForMonths(
            scan_months, [&](const TpchQ10ReturnedRevenueInput &input) {
              if (input.revenue_raw != 0)
                scanned[input.month_key].push_back(
                    {input.custkey, input.revenue_raw});
            }))
      return false;
    for (auto &entry : scanned)
      SortCoalesce(entry.second);
    static const std::vector<TpchQ10KV> kNone;
    const auto table = [&](int32_t ord) -> const std::vector<TpchQ10KV> * {
      if (ord < lo_ord || ord > hi_ord)
        return nullptr;
      const auto it = scanned.find(MonthFromOrd(ord));
      return it == scanned.end() ? &kNone : &it->second;
    };
    for (const auto month : touched_months) {
      std::vector<TpchQ10KV> copy = *table(MonthOrd(month));
      outcome.updates.push_back(core::StateImage::Make(
          core::TpchQ10Layout::kAccelerator, MonthPartition(month), 0,
          MakeMonthImage(month, TpchQ10KeyIndex::FromSorted(std::move(copy)))));
    }
    for (const auto w : touched_window_ords) {
      std::vector<const std::vector<TpchQ10KV> *> parts;
      for (int32_t k = 0; k < kWindowMonths; ++k)
        parts.push_back(table(w + k));
      std::vector<TpchQ10KV> sums;
      SumMonths(parts, sums);
      outcome.updates.push_back(core::StateImage::Make(
          core::TpchQ10Layout::kAccelerator, WindowPartition(MonthFromOrd(w)), 0,
          MakeWindowImage(MonthFromOrd(w), RankedFromSums(std::move(sums)))));
    }
    if (orders_changed && order_metadata)
      PublishOrders(0, /*erase_after_deltas=*/false);
    if (side_update)
      outcome.updates.push_back(std::move(*side_update));
    core::FinishRebuildPartition(outcome);
    return true;
  }

  // RF2: order metadata is removed after the lineitem deltas have resolved
  // against it.
  void PublishOrders(core::Version version, bool erase_after_deltas) {
    if (!order_metadata)
      return;
    const bool erased = ForOrderDeltasByShard(
        -1, [&](const auto &delta) { order_metadata->orders.Erase(delta.orderkey); });
    if (erased && erase_after_deltas)
      orders_changed = true;
    if (!orders_changed)
      return;
    if (erase_after_deltas)
      outcome.action = core::MaintenanceAction::ApplyDelta;
    const auto typed_orders =
        std::static_pointer_cast<const TpchQ10State>(std::move(order_metadata));
    outcome.updates.push_back(core::StateImage::Make(
        core::TpchQ10Layout::kAccelerator, core::TpchQ10Layout::kOrderMetadata,
        version, MakeStateImage(TpchQ10ImageKind::Orders, 0, typed_orders)));
  }

  // Per-month sorted (custkey, delta), dropping zero sums.
  void AggregateMonthDeltas() {
    std::vector<int32_t> months;
    for (const auto &entry : month_deltas)
      months.push_back(entry.first);
    std::vector<std::vector<TpchQ10KV>> month_agg(months.size());
    ForEachImage(months.size(), [&](std::size_t i) {
      auto &v = month_agg[i];
      const auto &deltas = month_deltas.at(months[i]);
      v.reserve(deltas.size());
      for (const auto &[key, d] : deltas)
        v.push_back({key, d});
      SortCoalesce(v);
    });
    for (std::size_t i = 0; i < months.size(); ++i)
      if (!month_agg[i].empty())
        agg.emplace(months[i], std::move(month_agg[i]));
  }

  // Months, windows and their images are independent; below the work
  // threshold they run as one inline slice.
  template <class Fn> void ForEachImage(std::size_t n, Fn &&fn) {
    core::ParallelEachWhen(services, n, delta_entries, kImageOpThreshold,
                           std::forward<Fn>(fn));
  }

  bool LoadImages() {
    std::vector<core::PartitionId> partitions;
    for (const auto o : needed_month_ords)
      partitions.push_back(MonthPartition(MonthFromOrd(o)));
    for (const auto w : touched_window_ords)
      partitions.push_back(WindowPartition(MonthFromOrd(w)));
    const auto snapshot =
        services.StateStore().ReadPartitions(visibility, partitions);
    for (const auto &image : snapshot.partitions) {
      if (image.partition >= kFirstWindow) {
        const auto *typed = AsQ10Image(image, TpchQ10ImageKind::Window);
        if (!typed)
          return false;
        const auto w = MonthOrd(typed->month);
        win_imgs[w] = typed;
        win_versions[w] = image.version;
      } else if (image.partition >= core::TpchQ10Layout::kFirstMonth) {
        const auto *typed = AsQ10Image(image, TpchQ10ImageKind::Month);
        if (!typed)
          return false;
        const auto o = MonthOrd(typed->month);
        month_imgs[o] = typed;
        month_versions[o] = image.version;
      }
    }
    return month_imgs.size() == needed_month_ords.size() &&
           win_imgs.size() == touched_window_ords.size();
  }

  // For each key with a delta in any of the window's months, old window
  // revenue = sum of the (published) month values.
  bool UpdateWindows() {
    const std::vector<int32_t> window_ords(touched_window_ords.begin(),
                                           touched_window_ords.end());
    std::vector<std::optional<core::StateImage>> updates(window_ords.size());
    std::vector<uint8_t> failed(window_ords.size(), 0);
    ForEachImage(window_ords.size(), [&](std::size_t i) {
      const auto w = window_ords[i];
      std::vector<int64_t> keys;
      for (int32_t k = 0; k < kWindowMonths; ++k) {
        const auto it = agg.find(MonthFromOrd(w + k));
        if (it == agg.end())
          continue;
        for (const auto &e : it->second)
          keys.push_back(e.key);
      }
      if (keys.empty())
        return;
      std::sort(keys.begin(), keys.end());
      keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
      std::vector<TpchQ10KV> rem, ins;
      for (const auto key : keys) {
        int64_t old_sum = 0, delta = 0;
        for (int32_t k = 0; k < kWindowMonths; ++k) {
          const auto mit = month_imgs.find(w + k);
          if (mit != month_imgs.end())
            old_sum += mit->second->by_key.FindVal(key);
          const auto it = agg.find(MonthFromOrd(w + k));
          if (it == agg.end())
            continue;
          const auto pos = std::lower_bound(
              it->second.begin(), it->second.end(), TpchQ10KV{key, 0},
              TpchQ10ByKey{});
          if (pos != it->second.end() && pos->key == key)
            delta += pos->val;
        }
        if (delta == 0)
          continue;
        if (old_sum != 0)
          rem.push_back({key, old_sum});
        if (old_sum + delta != 0)
          ins.push_back({key, old_sum + delta});
      }
      std::sort(rem.begin(), rem.end(), TpchQ10ByRank{});
      std::sort(ins.begin(), ins.end(), TpchQ10ByRank{});
      TpchQ10RankIndex next;
      if (!TpchQ10RankIndex::Apply(win_imgs.at(w)->ranked, rem, ins, next)) {
        failed[i] = 1;
        return;
      }
      updates[i] = core::StateImage::Make(
          core::TpchQ10Layout::kAccelerator, WindowPartition(MonthFromOrd(w)),
          win_versions.at(w), MakeWindowImage(MonthFromOrd(w), std::move(next)));
    });
    return Publish(updates, failed);
  }

  bool UpdateMonths() {
    std::vector<std::pair<int32_t, const std::vector<TpchQ10KV> *>> month_list;
    for (const auto &[month, deltas] : agg)
      month_list.emplace_back(month, &deltas);
    std::vector<std::optional<core::StateImage>> updates(month_list.size());
    std::vector<uint8_t> failed(month_list.size(), 0);
    ForEachImage(month_list.size(), [&](std::size_t i) {
      const auto month = month_list[i].first;
      const auto o = MonthOrd(month);
      std::vector<TpchQ10KV> rem, ins;
      const auto &prev = month_imgs.at(o)->by_key;
      for (const auto &e : *month_list[i].second) {
        const int64_t old_val = prev.FindVal(e.key);
        if (old_val != 0)
          rem.push_back({e.key, old_val});
        if (old_val + e.val != 0)
          ins.push_back({e.key, old_val + e.val});
      }
      TpchQ10KeyIndex next;
      if (!TpchQ10KeyIndex::Apply(prev, rem, ins, next)) {
        failed[i] = 1;
        return;
      }
      updates[i] = core::StateImage::Make(
          core::TpchQ10Layout::kAccelerator, MonthPartition(month),
          month_versions.at(o), MakeMonthImage(month, std::move(next)));
    });
    return Publish(updates, failed);
  }

  bool Publish(std::vector<std::optional<core::StateImage>> &updates,
               const std::vector<uint8_t> &failed) {
    if (std::any_of(failed.begin(), failed.end(), [](uint8_t f) { return f != 0; }))
      return false;
    for (auto &update : updates)
      if (update)
        outcome.updates.push_back(std::move(*update));
    return true;
  }

  bool ApplyMonthDeltas() {
    AggregateMonthDeltas();
    if (!LoadImages() || !UpdateWindows() || !UpdateMonths())
      return false;
    if (!outcome.updates.empty())
      outcome.action = core::MaintenanceAction::ApplyDelta;
    return true;
  }

  core::AuxEngineServices &services;
  const TpchQ10TxnBuffer &buffer;
  TpchQ10InitialScan *initial_scan;
  core::StagedMaintenanceOutcome &outcome;
  const core::VisibilityToken visibility;

  std::optional<core::StateImage> side_update;
  std::shared_ptr<TpchQ10State> order_metadata;
  core::Version order_version = 0;
  bool orders_changed = false;
  std::unordered_map<int64_t, TpchQ10Order> overlay_orders;

  std::set<int32_t> rebuild_months;
  std::map<int32_t, std::vector<std::pair<int64_t, int64_t>>> month_deltas;
  std::size_t delta_entries = 0;
  int32_t lo_ord = 0, hi_ord = -1;
  std::set<int32_t> touched_months, touched_window_ords, needed_month_ords;
  std::map<int32_t, std::vector<TpchQ10KV>> agg;
  std::map<int32_t, const TpchQ10StateImage *> month_imgs, win_imgs;
  std::map<int32_t, core::Version> month_versions, win_versions;
};

} // namespace

std::vector<std::pair<int64_t, int64_t>>
TpchQ10WindowTop(const TpchQ10State &side, const TpchQ10RankIndex &window,
                 std::size_t limit) {
  std::vector<std::pair<int64_t, int64_t>> out;
  out.reserve(limit);
  for (const auto &chunk : window.chunks) {
    for (const auto &e : chunk->e) {
      if (e.val <= 0)
        return out;
      if (!side.HasCustomer(e.key))
        continue;
      out.emplace_back(e.key, e.val);
      if (out.size() >= limit)
        return out;
    }
  }
  return out;
}

void TpchQ10EmitRows(const TpchQ10State &side,
                     const std::vector<std::pair<int64_t, int64_t>> &ranked,
                     core::AuxRowSink &sink) {
  sink.Begin({{{"c_custkey", core::AuxType::Int64, false},
               {"c_name", core::AuxType::String, false},
               {"revenue", core::AuxType::ScaledDecimal, false},
               {"c_acctbal", core::AuxType::ScaledDecimal, false},
               {"n_name", core::AuxType::String, false},
               {"c_address", core::AuxType::String, false},
               {"c_phone", core::AuxType::String, false},
               {"c_comment", core::AuxType::String, false}}});
  const std::string empty;
  for (const auto &[custkey, revenue] : ranked) {
    const auto rank = side.RankOf(custkey);
    const auto *strings = side.CustomerOf(custkey);
    const auto nation = side.nations.find(rank.nationkey);
    const auto &name = strings ? strings->name : empty;
    const auto &address = strings ? strings->address : empty;
    const auto &phone = strings ? strings->phone : empty;
    const auto &comment = strings ? strings->comment : empty;
    sink.WriteRow(
        {{core::AuxValue::Of(core::AuxType::Int64, custkey),
          core::AuxValue::Of(core::AuxType::String, name),
          core::AuxValue::Of(core::AuxType::ScaledDecimal,
                             core::ScaledDecimal{revenue, 4}),
          core::AuxValue::Of(core::AuxType::ScaledDecimal,
                             core::ScaledDecimal{rank.acctbal_raw, 2}),
          core::AuxValue::Of(core::AuxType::String,
                             nation == side.nations.end() ? empty
                                                          : nation->second),
          core::AuxValue::Of(core::AuxType::String, address),
          core::AuxValue::Of(core::AuxType::String, phone),
          core::AuxValue::Of(core::AuxType::String, comment)}});
  }
  sink.End();
}

int64_t TpchQ10RevenueRaw(double extended_price, double discount) {
  return static_cast<int64_t>(
      std::llround(extended_price * (1.0 - discount) * 10000.0));
}

bool TpchQ10TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ10TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  deltas.clear();
  order_deltas.clear();
  customer_deltas.clear();
}
bool TpchQ10TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
const std::vector<TpchQ10LineitemDelta> &TpchQ10TxnBuffer::Deltas() const {
  return deltas;
}
const std::vector<TpchQ10OrderDelta> &TpchQ10TxnBuffer::OrderDeltas() const {
  return order_deltas;
}
const std::vector<TpchQ10CustomerDelta> &
TpchQ10TxnBuffer::CustomerDeltas() const {
  return customer_deltas;
}
void TpchQ10TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  deltas.clear();
  order_deltas.clear();
  customer_deltas.clear();
}
void TpchQ10TxnBuffer::AddCustomerDelta(TpchQ10CustomerDelta delta) {
  pending = true;
  customer_deltas.push_back(std::move(delta));
}
void TpchQ10TxnBuffer::AddDelta(TpchQ10LineitemDelta delta) {
  pending = true;
  if (delta.revenue_raw != 0)
    deltas.push_back(delta);
}
void TpchQ10TxnBuffer::AddOrderDelta(TpchQ10OrderDelta delta) {
  pending = true;
  order_deltas.push_back(delta);
}


core::AcceleratorId TpchQ10Module::Id() const { return kId; }
std::vector<core::TableDependency> TpchQ10Module::Dependencies() const {
  return {{config.customer_table, {0, 1, 2, 3, 4, 5, 6}, {0, 1, 5, 3, 2, 4, 7}},
          {config.nation_table, {0, 1}, {0, 1}},
          {config.orders_table, {0, 1, 2}, {0, 1, 4}},
          {config.lineitem_table, {0, 1, 2, 3}, {0, 5, 6, 8}}};
}
std::unique_ptr<core::AuxTxnBuffer> TpchQ10Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ10TxnBuffer>();
}
void TpchQ10Module::Build(core::AuxEngineServices &services,
                          core::StateBuilder &builder) {
  auto data = BuildState(services);

  // Publish lean per-kind images so Side/Month payloads do not retain the
  // full orders map (orders are write-maintenance only).
  auto side_state = std::make_shared<TpchQ10State>();
  side_state->side_base = std::move(data.state.side_base);
  side_state->nations = std::move(data.state.nations);
  builder.Write(core::StateImage::Make(
      core::TpchQ10Layout::kAccelerator,
      core::TpchQ10Layout::kCustomerDimensions, 0,
      MakeStateImage(TpchQ10ImageKind::Side, 0,
                     std::shared_ptr<const TpchQ10State>(side_state))));

  // Orders partition is optional: SQL-pushdown builds skip it and resolve
  // orderkeys via LookupOrders during maintenance.
  if (data.state.orders.size() != 0) {
    auto orders_state = std::make_shared<TpchQ10State>();
    orders_state->orders = std::move(data.state.orders);
    builder.Write(core::StateImage::Make(
        core::TpchQ10Layout::kAccelerator,
        core::TpchQ10Layout::kOrderMetadata, 0,
        MakeStateImage(TpchQ10ImageKind::Orders, 0,
                       std::shared_ptr<const TpchQ10State>(orders_state))));
  }

  // Month range: every ordinal in [lo, hi] gets a month image (possibly
  // empty) so a missing month image inside the range never happens.
  int32_t lo_ord = 0, hi_ord = -1;
  if (!data.months.empty()) {
    lo_ord = MonthOrd(data.months.begin()->first);
    hi_ord = MonthOrd(data.months.rbegin()->first);
  }
  builder.Write(core::StateImage::Make(
      core::TpchQ10Layout::kAccelerator, kMetaPartition, 0,
      hi_ord < lo_ord ? MakeMetaImage(0, -1)
                      : MakeMetaImage(MonthFromOrd(lo_ord), MonthFromOrd(hi_ord))));
  if (hi_ord >= lo_ord) {
    // Flat key-sorted tables for every ordinal (null = empty month).
    const int32_t span = hi_ord - lo_ord + 1;
    std::vector<const std::vector<TpchQ10KV> *> tables(
        static_cast<std::size_t>(span), nullptr);
    for (const auto &[month, vec] : data.months)
      tables[static_cast<std::size_t>(MonthOrd(month) - lo_ord)] = &vec;
    // Windows start at ordinals [lo-2, hi]: every window containing a month
    // with data. Windows and month images are independent: built in parallel,
    // written in ordinal order.
    std::vector<std::shared_ptr<const TpchQ10StateImage>> windows(
        static_cast<std::size_t>(span + 2));
    std::vector<std::shared_ptr<const TpchQ10StateImage>> month_images(
        static_cast<std::size_t>(span));
    core::ParallelEach(services, windows.size(), kImageGrain, [&](std::size_t i) {
      const int32_t w_ord = lo_ord - 2 + static_cast<int32_t>(i);
      std::vector<const std::vector<TpchQ10KV> *> parts;
      for (int32_t j = 0; j < kWindowMonths; ++j) {
        const int32_t o = w_ord + j;
        parts.push_back(o >= lo_ord && o <= hi_ord
                            ? tables[static_cast<std::size_t>(o - lo_ord)]
                            : nullptr);
      }
      std::vector<TpchQ10KV> sums;
      SumMonths(parts, sums);
      windows[i] =
          MakeWindowImage(MonthFromOrd(w_ord), RankedFromSums(std::move(sums)));
    });
    core::ParallelEach(services, month_images.size(), kImageGrain, [&](std::size_t i) {
      const auto *table = tables[i];
      std::vector<TpchQ10KV> copy;
      if (table)
        copy = *table;
      month_images[i] = MakeMonthImage(
          MonthFromOrd(lo_ord + static_cast<int32_t>(i)),
          TpchQ10KeyIndex::FromSorted(std::move(copy)));
    });
    for (int32_t o = lo_ord; o <= hi_ord; ++o)
      builder.Write(core::StateImage::Make(
          core::TpchQ10Layout::kAccelerator, MonthPartition(MonthFromOrd(o)), 0,
          std::move(month_images[static_cast<std::size_t>(o - lo_ord)])));
    for (int32_t w = lo_ord - 2; w <= hi_ord; ++w) {
      auto &win = windows[static_cast<std::size_t>(w - (lo_ord - 2))];
      builder.Write(core::StateImage::Make(
          core::TpchQ10Layout::kAccelerator, WindowPartition(MonthFromOrd(w)), 0,
          std::move(win)));
    }
  }
  builder.Finish();
}


core::InvalidationPlan
TpchQ10Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.lineitem_table ||
      mutation.table == config.orders_table ||
      mutation.table == config.customer_table) {
    core::InvalidationPlan plan;
    plan.scope =
        ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
    return plan;
  }
  if (mutation.table == config.nation_table)
    return core::AllPartitionsPlan();
  return {};
}

void TpchQ10Module::AccumulateMutation(const core::MutationBatch &mutation,
                                       core::AuxEngineServices &,
                                       core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ10TxnBuffer &>(base_buffer);
  if (mutation.table == config.nation_table) {
    buffer.MarkRebuild();
    return;
  }
  if (mutation.table == config.customer_table) {
    // Row layout: custkey, name, acctbal, nationkey, address, phone, comment.
    auto add = [&](const core::AuxRow &row, int8_t sign) {
      TpchQ10CustomerDelta delta;
      delta.sign = sign;
      int64_t key = 0, acct = 0, nation = 0;
      int32_t scale = 0;
      if (!core::IntCell(row, 0, key) || !core::StringCell(row, 1, delta.customer.name) ||
          !DecimalOrDoubleCell(row, 2, acct, scale) || scale != 2 ||
          !core::IntCell(row, 3, nation) ||
          nation < std::numeric_limits<int32_t>::min() ||
          nation > std::numeric_limits<int32_t>::max() ||
          !core::StringCell(row, 4, delta.customer.address) ||
          !core::StringCell(row, 5, delta.customer.phone) ||
          !core::StringCell(row, 6, delta.customer.comment)) {
        buffer.MarkRebuild();
        return;
      }
      delta.custkey = key;
      delta.customer.acctbal_raw = acct;
      delta.customer.nationkey = static_cast<int32_t>(nation);
      buffer.AddCustomerDelta(std::move(delta));
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
    return;
  }
  if (mutation.table == config.orders_table) {
    auto add = [&](const core::AuxRow &row, int8_t sign) {
      int64_t orderkey = 0, custkey = 0;
      int32_t orderdate = 0;
      if (!ReadOrder(row, orderkey, custkey, orderdate)) {
        buffer.MarkRebuild();
        return;
      }
      buffer.AddOrderDelta({orderkey, custkey, orderdate, sign});
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
    return;
  }
  if (mutation.table != config.lineitem_table)
    return;
  auto add = [&](const core::AuxRow &row, int8_t sign) {
    int64_t orderkey = 0, revenue = 0;
    bool returned = false;
    if (!ReadLineitem(row, orderkey, revenue, returned)) {
      buffer.MarkRebuild();
      return;
    }
    if (returned)
      buffer.AddDelta({orderkey, revenue, sign});
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
TpchQ10Module::StageMaintenance(core::AuxEngineServices &services,
                                const core::InvalidationPlan &,
                                core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ10TxnBuffer &>(base_buffer);
  core::StagedMaintenanceOutcome outcome;
  if (buffer.RequiresRebuild()) {
    core::FinishRebuildAll(
        outcome, services.ChooseMaintenance(core::MaintenanceRequest::RebuildOnly(
                     services.ExpectedQueriesBeforeNextWrite())));
    return outcome;
  }
  if (buffer.Deltas().empty() && buffer.OrderDeltas().empty() &&
      buffer.CustomerDeltas().empty())
    return outcome;

  MonthMaintainer maintainer(services, buffer, initial_scan, outcome);
  if (!maintainer.Run())
    core::FinishRebuildAll(outcome);
  return outcome;
}

bool TpchQ10Module::CheckValidity(core::AuxEngineServices &services,
                                  const core::QueryArguments &arguments) const {
  if (arguments.values.empty())
    return false;
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}
void TpchQ10Module::Execute(core::AuxEngineServices &services,
                            const core::QueryArguments &arguments,
                            core::AuxRowSink &sink) {
  int32_t start = 0;
  if (!arguments.values.empty() && !arguments.values[0].is_null) {
    if (std::holds_alternative<core::AuxDate>(arguments.values[0].payload))
      start =
          std::get<core::AuxDate>(arguments.values[0].payload).days_since_epoch;
    else {
      int64_t value = 0;
      if (std::holds_alternative<int64_t>(arguments.values[0].payload))
        value = std::get<int64_t>(arguments.values[0].payload);
      start = static_cast<int32_t>(value);
    }
  }
  const auto start_month = MonthKey(start);
  if (MonthStart(start_month) != start) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  if (!CheckValidity(services, arguments)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  // Side + the one precomputed window for this month start: the answer is
  // the window's first 20 entries (skipping customers that no longer exist).
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(),
      {core::TpchQ10Layout::kCustomerDimensions, WindowPartition(start_month)});
  const TpchQ10State *side = nullptr;
  const TpchQ10StateImage *window = nullptr;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == core::TpchQ10Layout::kCustomerDimensions) {
      const auto *typed = AsQ10Image(image, TpchQ10ImageKind::Side);
      if (typed)
        side = typed->state.get();
    } else if (image.partition == WindowPartition(start_month)) {
      window = AsQ10Image(image, TpchQ10ImageKind::Window);
    }
  }
  if (!side) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  if (!window) {
    // No image: a window that starts outside [lo - 2, hi] has no revenue at
    // all (empty answer); anything else is unexpected, so recompute.
    const auto meta_snapshot = services.StateStore().ReadPartitions(
        services.CurrentVisibility(), {kMetaPartition});
    for (const auto &image : meta_snapshot.partitions) {
      const auto *meta = AsQ10Image(image, TpchQ10ImageKind::Meta);
      if (!meta)
        continue;
      const bool empty_data = meta->hi_month < meta->lo_month;
      const auto ord = MonthOrd(start_month);
      if (empty_data || ord < MonthOrd(meta->lo_month) - 2 ||
          ord > MonthOrd(meta->hi_month)) {
        TpchQ10EmitRows(*side, {}, sink);
        return;
      }
    }
    ExecuteFallback(services, arguments, sink);
    return;
  }
  TpchQ10EmitRows(*side, TpchQ10WindowTop(*side, window->ranked, kTpchQ10ResultLimit),
                  sink);
}

namespace {

using scan::DateColumn;
using scan::DecimalColumn;
using scan::IntColumn;
using scan::TextColumn;

struct CustomerRow {
  int64_t key = 0;
  TpchQ10Customer customer;
};

using DenseRanks = core::DenseIdArray<TpchQ10CustomerRank>;

// Customers are dealt by rank-array leaf, so a bucket owns whole leaves.
DealtRows<CustomerRow> ScanCustomers(core::AuxEngineServices &services, TpchQ10InitialScan &source) {
  return DealTable<CustomerRow>(
      services, source, "customer",
      {IntColumn("c_custkey"), TextColumn("c_name"), DecimalColumn("c_acctbal", 2),
       IntColumn("c_nationkey"), TextColumn("c_address"), TextColumn("c_phone"),
       TextColumn("c_comment")},
      kDealBuckets, [](DealtRows<CustomerRow> &rows, const auto &row) {
        const int64_t key = row.Int(0);
        if (!DenseRanks::Fits(key))
          return;
        rows.Add(static_cast<std::size_t>(key >> DenseRanks::kShift) % kDealBuckets, key,
                 {key,
                  {row.Text(1), row.Decimal(2), static_cast<int32_t>(row.Int(3)), row.Text(4),
                   row.Text(5), row.Text(6)}});
      });
}

// Each bucket becomes one segment of strings and sets the ranks of its own leaves.
void FillCustomerSide(core::AuxEngineServices &services, DealtRows<CustomerRow> &&rows,
                      TpchQ10SideBase &base) {
  if (rows.MaxKey() >= 0)
    base.ranks.Ensure(rows.MaxKey()); // sizes the leaf directory
  base.segments.resize(rows.BucketCount());
  core::ParallelEach(services, rows.BucketCount(), 1, [&](std::size_t bucket) {
    auto &segment = base.segments[bucket];
    for (auto &row : rows.TakeBucket(bucket)) {
      TpchQ10CustomerRank rank;
      rank.acctbal_raw = row.customer.acctbal_raw;
      rank.nationkey = row.customer.nationkey;
      rank.index = static_cast<uint32_t>(segment.size());
      rank.segment = static_cast<uint16_t>(bucket);
      rank.present = 1;
      base.ranks.Set(row.key, rank);
      segment.push_back(std::move(row.customer));
    }
  });
}

struct OrderRow {
  int64_t key = 0;
  TpchQ10Order order;
};
using OrderMap = decltype(TpchQ10State::orders);

// Orders are dealt by the top bits of their shard, so each bucket owns whole shards.
constexpr unsigned kOrderBucketBits = 10;
// Small shards bound what a touched shard copies.
constexpr std::size_t kOrdersPerShard = 512;

OrderMap ScanOrders(core::AuxEngineServices &services, TpchQ10InitialScan &source) {
  auto dealt = DealTable<OrderRow>(
      services, source, "orders",
      {IntColumn("o_orderkey"), IntColumn("o_custkey"), DateColumn("o_orderdate")},
      std::size_t{1} << kOrderBucketBits, [](DealtRows<OrderRow> &rows, const auto &row) {
        const int64_t key = row.Int(0);
        rows.Add(OrderMap::ShardOf(key, kOrderBucketBits), key,
                 {key, TpchQ10Order{row.Int(1), row.Date(2)}});
      });
  OrderMap orders(std::max(kOrderBucketBits, OrderMap::ShardBitsFor(dealt.Size(), kOrdersPerShard)));
  ParallelForEachRow(services, dealt,
                     [&](std::size_t, const OrderRow &row) { orders.Upsert(row.key, row.order); });
  return orders;
}

using MonthTables = std::map<int32_t, std::vector<TpchQ10KV>>;

// Returned-lineitem revenue entries per order month, unsorted. The partitions' tables
// are gathered month by month.
std::map<int32_t, std::vector<TpchQ10KV>>
ScanReturnedRevenue(core::AuxEngineServices &services, TpchQ10InitialScan &source,
                    const OrderMap &orders) {
  std::vector<MonthTables> runs = core::PartitionedBuild<std::vector<MonthTables>>(
      services, [] { return std::vector<MonthTables>(1); },
      [&](std::size_t parts, auto &&sink) {
        source.ScanTable(services, "lineitem",
                         {IntColumn("l_orderkey"), DecimalColumn("l_extendedprice", 2),
                          DecimalColumn("l_discount", 2), TextColumn("l_returnflag")},
                         parts, sink);
      },
      [&](std::vector<MonthTables> &partial, const auto &row) {
        if (row.Char(3) != 'R')
          return;
        const TpchQ10Order *order = orders.Find(row.Int(0));
        if (!order)
          return;
        const auto revenue = TpchQ10RevenueRaw(static_cast<double>(row.Decimal(1)) / 100.0,
                                               static_cast<double>(row.Decimal(2)) / 100.0);
        partial.front()[MonthKey(order->orderdate_days)].push_back({order->custkey, revenue});
      },
      [](std::vector<MonthTables> &into, std::vector<MonthTables> &&from) {
        std::move(from.begin(), from.end(), std::back_inserter(into));
      },
      core::BuildPartitions(services));

  std::map<int32_t, std::vector<TpchQ10KV>> months;
  for (const auto &run : runs)
    for (const auto &entry : run)
      months[entry.first];
  std::vector<std::pair<const int32_t, std::vector<TpchQ10KV>> *> slots;
  for (auto &entry : months)
    slots.push_back(&entry);
  core::ParallelEach(services, slots.size(), 1, [&](std::size_t i) {
    auto &[month, table] = *slots[i];
    for (auto &run : runs) {
      const auto it = run.find(month);
      if (it == run.end())
        continue;
      table.insert(table.end(), it->second.begin(), it->second.end());
      it->second = {};
    }
  });
  return months;
}

} // namespace

TpchQ10BuildData
TpchQ10Module::BuildState(core::AuxEngineServices &services) const {
  TpchQ10BuildData data;
  TpchQ10State &state = data.state;
  if (!initial_scan)
    throw std::logic_error("TPC-H Q10 requires a typed initial scan source");

  auto base = std::make_shared<TpchQ10SideBase>();
  FillCustomerSide(services, ScanCustomers(services, *initial_scan), *base);
  state.side_base = std::move(base);

  initial_scan->ScanNationsSerial([&](const TpchQ10NationInput &input) {
    state.nations[input.key] = input.name;
  });

  // One build thread: the engine joins and aggregates the returned revenue in
  // SQL (write maintenance then uses LookupOrders, not an orders map). More
  // threads: partitioned orders and lineitem scans.
  const bool used_sql_revenue =
      services.WorkerCount() == 1 &&
      initial_scan->ScanReturnedCustomerRevenue([&](const TpchQ10ReturnedRevenueInput &input) {
        if (input.revenue_raw == 0)
          return;
        data.months[input.month_key].push_back({input.custkey, input.revenue_raw});
      });

  if (!used_sql_revenue) {
    state.orders = ScanOrders(services, *initial_scan);
    data.months = ScanReturnedRevenue(services, *initial_scan, state.orders);
  }
  // Key-sorted, deduplicated (summed), nonzero month tables.
  std::vector<std::vector<TpchQ10KV> *> tables;
  for (auto &entry : data.months)
    tables.push_back(&entry.second);
  core::ParallelEach(services, tables.size(), kMonthTableGrain, [&](std::size_t i) {
    auto &vec = *tables[i];
    SortCoalesce(vec);
    vec.shrink_to_fit();
  });
  for (auto it = data.months.begin(); it != data.months.end();)
    it = it->second.empty() ? data.months.erase(it) : std::next(it);
  return data;
}

std::vector<core::CalibrationWrite> TpchQ10Module::CalibrationWrites() const {
  return TpchQ10CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
