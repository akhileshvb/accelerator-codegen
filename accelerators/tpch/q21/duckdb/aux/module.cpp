#include "tpch/q21/duckdb/aux/module.hpp"
#include "tpch/q21/calibration_writes.hpp"
#include "prims/bucketed_rows.hpp"
#include "prims/key_range_sort.hpp"
#include "substrate/aux/core/aux_cells.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/parallel_sort.hpp"
#include "substrate/aux/core/partitioned_build.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {

// Orders per maintenance slice (untimed): a few hash probes and a small-vector scan each.
constexpr std::size_t kOrderGrain = 2048;
// Elements per sorted slice (untimed): a key compare each.
constexpr std::size_t kSortGrain = 65536;
// Shard runs per maintenance slice (untimed): one shard write per item.
constexpr std::size_t kRunGrain = 1024;
// Nation row compares below which a commit patches its nations inline (untimed).
constexpr std::size_t kNationOpThreshold = 4096;

using core::CharCell;
using core::DateCell;
using core::IntCell;
using core::StringCell;

// TPC-H Q21: ORDER BY numwait DESC, s_name LIMIT 100.
constexpr std::size_t kTpchQ21Limit = 100;

bool BetterOutputRow(const TpchQ21OutputRow &lhs, const TpchQ21OutputRow &rhs) {
  if (lhs.numwait != rhs.numwait)
    return lhs.numwait > rhs.numwait;
  return lhs.name < rhs.name;
}

// Image rows ordered (numwait DESC, name ASC).
bool ImageRowBefore(const TpchQ21ImageRow &a, const TpchQ21ImageRow &b) {
  if (a.numwait != b.numwait)
    return a.numwait > b.numwait;
  return *a.name < *b.name;
}

std::shared_ptr<const TpchQ21StateImage>
MakeDirImage(std::shared_ptr<const TpchQ21Dir> dir) {
  auto image = std::make_shared<TpchQ21StateImage>();
  image->kind = TpchQ21ImageKind::SupplierMetadata;
  image->dir = std::move(dir);
  return image;
}

std::shared_ptr<const TpchQ21StateImage>
MakeCoreImage(std::shared_ptr<const TpchQ21Core> core) {
  auto image = std::make_shared<TpchQ21StateImage>();
  image->kind = TpchQ21ImageKind::OrderSummaries;
  image->core = std::move(core);
  return image;
}

std::shared_ptr<const TpchQ21StateImage>
MakeIndexImage(std::shared_ptr<const TpchQ21NationIndex> index) {
  auto image = std::make_shared<TpchQ21StateImage>();
  image->kind = TpchQ21ImageKind::NationNameIndex;
  image->nation_index = std::move(index);
  return image;
}

std::shared_ptr<const TpchQ21StateImage>
MakeNationImage(int32_t nationkey,
                std::shared_ptr<const TpchQ21NationRows> rows,
                const std::string &nation_name = "") {
  auto image = std::make_shared<TpchQ21StateImage>();
  image->kind = TpchQ21ImageKind::Nation;
  image->nationkey = nationkey;
  image->nation_name = nation_name;
  image->rows = std::move(rows);
  return image;
}

const TpchQ21StateImage *AsQ21Image(const core::StateImage &image,
                                    TpchQ21ImageKind kind) {
  const auto *payload = image.As<TpchQ21StateImage>();
  if (!payload || payload->kind != kind)
    return nullptr;
  switch (kind) {
  case TpchQ21ImageKind::SupplierMetadata:
    return payload->dir ? payload : nullptr;
  case TpchQ21ImageKind::OrderSummaries:
    return payload->core ? payload : nullptr;
  case TpchQ21ImageKind::NationNameIndex:
    return payload->nation_index ? payload : nullptr;
  case TpchQ21ImageKind::Nation:
    return payload->rows ? payload : nullptr;
  }
  return nullptr;
}

core::PartitionId NationPartition(int32_t nationkey) {
  return TpchQ21Layout::kNationBase + static_cast<core::PartitionId>(nationkey);
}

// Key the read image by the query parameter rather than first loading a global
// name->nation metadata image.  FNV-1a is stable across processes; collisions
// are detected by the image's retained name and take the safe fallback path.
core::PartitionId NationNamePartition(const std::string &name) {
  uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char c : name) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  return TpchQ21Layout::kNationNameBase +
         static_cast<core::PartitionId>(hash % TpchQ21Layout::kNationNameSlots);
}

// Reduce the two correlated EXISTS predicates to per-order sufficient facts:
// Q21 can contribute only if exactly one supplier is late and at least one
// different supplier participates. Returns true with the sole late supplier and
// its late-item count when the order contributes.
bool ComputeOrderContribution(const OrderSummary &order, int64_t &suppkey,
                              int64_t &count) {
  if (order.orderstatus != 'F' || order.suppliers.size() <= 1)
    return false;
  uint32_t late_suppliers = 0;
  for (const auto &info : order.suppliers) {
    if (info.total_count <= 0 || info.late_count <= 0)
      continue;
    if (++late_suppliers > 1)
      return false; // NOT EXISTS fails for every candidate.
    suppkey = info.suppkey;
    count = info.late_count;
  }
  return late_suppliers == 1;
}

bool ReadLineitem(const core::AuxRow &row, int64_t &orderkey, int64_t &suppkey,
                  int32_t &commitdate, int32_t &receiptdate) {
  return IntCell(row, 0, orderkey) && IntCell(row, 1, suppkey) &&
         DateCell(row, 2, commitdate) && DateCell(row, 3, receiptdate);
}

bool ReadSupplier(const core::AuxRow &row, int64_t &suppkey, std::string &name,
                  int32_t &nationkey) {
  int64_t nk = 0;
  if (!IntCell(row, 0, suppkey) || !StringCell(row, 1, name) ||
      !IntCell(row, 2, nk))
    return false;
  if (nk < std::numeric_limits<int32_t>::min() ||
      nk > std::numeric_limits<int32_t>::max())
    return false;
  nationkey = static_cast<int32_t>(nk);
  return true;
}

bool ReadOrder(const core::AuxRow &row, int64_t &orderkey, char &orderstatus) {
  return IntCell(row, 0, orderkey) && CharCell(row, 1, orderstatus);
}

using RowVec = std::vector<TpchQ21ImageRow>;

// A net change of one s_name's merged row. `hint` is the row's current numwait
// when the changed suppliers account for all of it (a binary-search key).
struct NameChange {
  std::shared_ptr<const std::string> name;
  int64_t delta = 0;
  int64_t hint = 0;
};

// Rewrites a nation's rows with `changes` in one pass: the touched rows are
// located (binary search by hint, one scan for the rest), dropped, and the
// re-priced rows merged back in order. False when a change would decrement a
// row that does not exist (inconsistent state).
bool PatchNationRows(const RowVec &rows, std::vector<NameChange> changes, RowVec &out) {
  std::stable_sort(changes.begin(), changes.end(),
                   [](const NameChange &a, const NameChange &b) { return *a.name < *b.name; });
  std::size_t kept = 0;
  for (std::size_t i = 0; i < changes.size(); ++i) {
    if (kept != 0 && *changes[kept - 1].name == *changes[i].name) {
      changes[kept - 1].delta += changes[i].delta;
      changes[kept - 1].hint += changes[i].hint;
    } else if (kept++ != i) {
      changes[kept - 1] = std::move(changes[i]);
    }
  }
  changes.resize(kept);
  changes.erase(std::remove_if(changes.begin(), changes.end(),
                               [](const NameChange &c) { return c.delta == 0; }),
                changes.end());

  constexpr std::size_t kMissing = ~std::size_t{0};
  std::vector<std::size_t> at(changes.size(), kMissing);
  std::unordered_map<std::string_view, std::size_t> unlocated;
  for (std::size_t i = 0; i < changes.size(); ++i) {
    if (changes[i].hint > 0) {
      const TpchQ21ImageRow probe{changes[i].name, changes[i].hint};
      const auto it = std::lower_bound(rows.begin(), rows.end(), probe, ImageRowBefore);
      if (it != rows.end() && it->numwait == probe.numwait && *it->name == *probe.name)
        at[i] = static_cast<std::size_t>(it - rows.begin());
    }
    if (at[i] == kMissing)
      unlocated.emplace(*changes[i].name, i);
  }
  if (!unlocated.empty()) {
    for (std::size_t r = 0; r < rows.size(); ++r) {
      const auto it = unlocated.find(*rows[r].name);
      if (it != unlocated.end())
        at[it->second] = r;
    }
  }

  std::vector<std::size_t> dropped;
  RowVec fresh;
  for (std::size_t i = 0; i < changes.size(); ++i) {
    const bool found = at[i] != kMissing;
    if (!found && changes[i].delta < 0)
      return false;
    if (found)
      dropped.push_back(at[i]);
    const int64_t sum = changes[i].delta + (found ? rows[at[i]].numwait : 0);
    if (sum > 0)
      fresh.push_back({changes[i].name, sum});
  }
  std::sort(dropped.begin(), dropped.end());
  std::sort(fresh.begin(), fresh.end(), ImageRowBefore);

  out.clear();
  out.reserve(rows.size() + fresh.size());
  std::size_t next_drop = 0, next_fresh = 0;
  for (std::size_t r = 0; r < rows.size(); ++r) {
    if (next_drop < dropped.size() && dropped[next_drop] == r) {
      ++next_drop;
      continue;
    }
    for (; next_fresh < fresh.size() && ImageRowBefore(fresh[next_fresh], rows[r]); ++next_fresh)
      out.push_back(fresh[next_fresh]);
    out.push_back(rows[r]);
  }
  out.insert(out.end(), fresh.begin() + static_cast<std::ptrdiff_t>(next_fresh), fresh.end());
  return true;
}

void EmitRow(core::AuxRowSink &sink, const std::string &name, int64_t numwait) {
  core::AuxRow out;
  out.values.reserve(2);
  out.values.push_back(core::AuxValue::Of(core::AuxType::String, name));
  out.values.push_back(core::AuxValue::Of(core::AuxType::Int64, numwait));
  sink.WriteRow(out);
}

// An order touched by the commit, with its lineitem deltas as the range
// [first, last) of the orderkey-sorted delta index.
struct AffectedOrder {
  enum class Source : uint8_t { Tracked, Inserted, Lookup, Skip };
  int64_t orderkey = 0;
  bool has_order_delta = false;
  char first_old_status = 0; // status before the commit's first order delta
  bool status_changed = false;
  char new_status = 0; // status after the commit's last status change
  std::size_t first = 0, last = 0;
  Source source = Source::Skip;
  std::size_t lookup = 0; // index into the batched lookups (Source::Lookup)
};

// The new state of one affected order: put it in the map, take it out, or leave
// the map alone.
struct OrderUpdate {
  enum class Action : uint8_t { None, Put, Erase };
  Action action = Action::None;
  OrderSummary summary;
};

using KeyedIndex = std::vector<std::pair<int64_t, std::size_t>>;

template <class Deltas>
KeyedIndex SortedByOrder(const core::AuxEngineServices &services, const Deltas &deltas) {
  KeyedIndex index(deltas.size());
  for (std::size_t i = 0; i < deltas.size(); ++i)
    index[i] = {deltas[i].orderkey, i};
  core::ParallelStableSort(
      services, index, [](const auto &a, const auto &b) { return a.first < b.first; },
      kSortGrain);
  return index;
}

struct AffectedOrders {
  std::vector<AffectedOrder> orders; // ascending orderkey
  KeyedIndex lineitems;              // (orderkey, delta position), arrival order within an order
};

// Joins the orderkey-sorted order and lineitem deltas into one entry per order.
AffectedOrders CollectAffectedOrders(const core::AuxEngineServices &services,
                                     const TpchQ21TxnBuffer &buffer) {
  const auto &order_deltas = buffer.OrderDeltas();
  const auto by_order = SortedByOrder(services, order_deltas);
  AffectedOrders affected;
  affected.lineitems = SortedByOrder(services, buffer.LineitemDeltas());
  const auto &by_lineitem = affected.lineitems;
  std::size_t o = 0, l = 0;
  while (o < by_order.size() || l < by_lineitem.size()) {
    AffectedOrder order;
    order.orderkey = o == by_order.size()      ? by_lineitem[l].first
                     : l == by_lineitem.size() ? by_order[o].first
                                               : std::min(by_order[o].first, by_lineitem[l].first);
    for (; o < by_order.size() && by_order[o].first == order.orderkey; ++o) {
      const auto &delta = order_deltas[by_order[o].second];
      if (!order.has_order_delta)
        order.first_old_status = delta.old_orderstatus;
      order.has_order_delta = true;
      if (delta.old_orderstatus != delta.new_orderstatus) {
        order.status_changed = true;
        order.new_status = delta.new_orderstatus;
      }
    }
    order.first = l;
    while (l < by_lineitem.size() && by_lineitem[l].first == order.orderkey)
      ++l;
    order.last = l;
    affected.orders.push_back(order);
  }
  return affected;
}

// Absent orders contribute nothing, so an absent one is only tracked again when
// this commit inserts it; any other absent order is looked up in the table.
AffectedOrder::Source ClassifyOrder(const AffectedOrder &order, bool tracked) {
  using Source = AffectedOrder::Source;
  if (tracked)
    return Source::Tracked;
  if (order.status_changed && order.new_status == 0)
    return Source::Skip; // deleted by this commit
  if (!order.has_order_delta || order.first_old_status != 0 || !order.status_changed)
    return Source::Lookup;
  return Source::Inserted; // RF1: its lineitems cannot predate it
}

OrderSummary SummaryOf(const TpchQ21InitialScan::OrderLookup &lookup) {
  OrderSummary summary;
  summary.orderstatus = lookup.status;
  for (const auto &entry : lookup.suppliers)
    if (entry[1] > 0)
      summary.suppliers.push_back({entry[0], entry[1], entry[2]});
  return summary;
}

using Numwait = std::unordered_map<int64_t, int64_t>; // suppkey -> numwait change

// The side images a commit's maintenance builds on.
struct Parents {
  std::shared_ptr<const TpchQ21Dir> supplier_dir;
  std::shared_ptr<const TpchQ21Core> order_core;
  std::shared_ptr<const TpchQ21NationIndex> nation_index;
};

// Pointer fetches only; nothing is copied. Null when an image is missing or
// not of its kind.
std::optional<Parents> LoadParents(core::AuxEngineServices &services,
                                   const core::VisibilityToken &visibility) {
  const auto snapshot = services.StateStore().ReadPartitions(
      visibility, {TpchQ21Layout::kSupplierMetadata, TpchQ21Layout::kOrderSummaries,
                   TpchQ21Layout::kNationNameIndex});
  Parents parents;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == TpchQ21Layout::kSupplierMetadata) {
      const auto *typed = AsQ21Image(image, TpchQ21ImageKind::SupplierMetadata);
      if (!typed)
        return std::nullopt;
      parents.supplier_dir = typed->dir;
    } else if (image.partition == TpchQ21Layout::kOrderSummaries) {
      const auto *typed = AsQ21Image(image, TpchQ21ImageKind::OrderSummaries);
      if (!typed)
        return std::nullopt;
      parents.order_core = typed->core;
    } else if (image.partition == TpchQ21Layout::kNationNameIndex) {
      const auto *typed = AsQ21Image(image, TpchQ21ImageKind::NationNameIndex);
      if (!typed)
        return std::nullopt;
      parents.nation_index = typed->nation_index;
    }
  }
  if (!parents.supplier_dir || !parents.order_core || !parents.nation_index)
    return std::nullopt;
  return parents;
}

// What the commit does to the order summaries and, through them, to each
// supplier's numwait.
struct OrderUpdates {
  AffectedOrders affected;
  std::vector<OrderUpdate> updates; // parallel to affected.orders
  Numwait numwait_delta;
};

// Classifies the affected orders, looks up the untracked ones in the table and
// computes each order's new state. Nullopt when the lookups are unavailable.
std::optional<OrderUpdates>
ComputeOrderUpdates(const core::AuxEngineServices &services, TpchQ21InitialScan *initial_scan,
                    const TpchQ21TxnBuffer &buffer, const TpchQ21Core &parent_core) {
  using Source = AffectedOrder::Source;
  OrderUpdates result;
  result.affected = CollectAffectedOrders(services, buffer);
  auto &orders = result.affected.orders;
  const auto &sorted_lineitems = result.affected.lineitems;
  const auto &lineitem_deltas = buffer.LineitemDeltas();
  core::ParallelEach(services, orders.size(), kOrderGrain, [&](std::size_t i) {
    orders[i].source = ClassifyOrder(orders[i], parent_core.orders.Contains(orders[i].orderkey));
  });
  std::vector<int64_t> lookup_keys;
  for (auto &order : orders) {
    if (order.source == Source::Lookup) {
      order.lookup = lookup_keys.size();
      lookup_keys.push_back(order.orderkey);
    }
  }
  std::vector<TpchQ21InitialScan::OrderLookup> lookups;
  if (!lookup_keys.empty() &&
      (!initial_scan || !initial_scan->LookupOrderSummaries(lookup_keys, lookups)))
    return std::nullopt;

  result.updates.resize(orders.size());
  auto &updates = result.updates;
  const auto compute_order = [&](std::size_t i, Numwait &sums) {
    const AffectedOrder &order = orders[i];
    OrderUpdate &update = updates[i];
    if (order.source == Source::Skip)
      return;
    if (order.source == Source::Lookup) {
      const auto &lookup = lookups[order.lookup];
      if (!lookup.found || lookup.status != 'F')
        return;
      auto fresh = SummaryOf(lookup);
      int64_t supp = 0, count = 0;
      if (ComputeOrderContribution(fresh, supp, count)) {
        sums[supp] += count;
        update = {OrderUpdate::Action::Put, std::move(fresh)};
      }
      return;
    }
    const bool tracked = order.source == Source::Tracked;
    OrderSummary summary = tracked ? *parent_core.orders.Find(order.orderkey) : OrderSummary{};
    int64_t old_supp = 0, old_count = 0;
    const bool had_contrib = ComputeOrderContribution(summary, old_supp, old_count);
    if (order.status_changed)
      summary.orderstatus = order.new_status;
    for (std::size_t k = order.first; k < order.last; ++k) {
      const auto &delta = lineitem_deltas[sorted_lineitems[k].second];
      auto *info = summary.FindSupplier(delta.suppkey);
      if (!info) {
        summary.suppliers.push_back({delta.suppkey, 0, 0});
        info = &summary.suppliers.back();
      }
      info->total_count += delta.sign;
      if (delta.receiptdate > delta.commitdate)
        info->late_count += delta.sign;
      if (info->total_count <= 0 && info->late_count <= 0)
        summary.suppliers.erase(summary.suppliers.begin() + (info - summary.suppliers.data()));
    }
    int64_t new_supp = 0, new_count = 0;
    const bool has_contrib = ComputeOrderContribution(summary, new_supp, new_count);
    if (had_contrib)
      sums[old_supp] -= old_count;
    if (has_contrib)
      sums[new_supp] += new_count;
    if (has_contrib)
      update = {OrderUpdate::Action::Put, std::move(summary)};
    else if (tracked)
      update.action = OrderUpdate::Action::Erase;
  };
  core::ParallelReduce(
      services, orders.size(), kOrderGrain, result.numwait_delta,
      [&](Numwait &sums, std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i)
          compute_order(i, sums);
      },
      [](Numwait &into, Numwait &&from) {
        for (const auto &[suppkey, delta] : from)
          into[suppkey] += delta;
      });
  return result;
}

// Writes the order updates into `next_core`: each task owns whole shards of the map.
void ApplyOrderUpdates(const core::AuxEngineServices &services, OrderUpdates &order_updates,
                       TpchQ21Core &next_core) {
  const auto &orders = order_updates.affected.orders;
  auto &updates = order_updates.updates;
  std::vector<std::pair<std::size_t, std::size_t>> items; // (shard, order)
  for (std::size_t i = 0; i < updates.size(); ++i)
    if (updates[i].action != OrderUpdate::Action::None)
      items.push_back({next_core.orders.ShardOf(orders[i].orderkey), i});
  const auto shard_of = [](const auto &item) { return item.first; };
  core::ParallelStableSort(
      services, items, [&](const auto &a, const auto &b) { return shard_of(a) < shard_of(b); },
      kSortGrain);
  const auto runs = core::RunStarts(items, shard_of);
  core::ParallelEach(services, runs.size() - 1, kRunGrain, [&](std::size_t run) {
    for (std::size_t k = runs[run]; k < runs[run + 1]; ++k) {
      const std::size_t i = items[k].second;
      if (updates[i].action == OrderUpdate::Action::Put)
        next_core.orders.Upsert(orders[i].orderkey, std::move(updates[i].summary));
      else
        next_core.orders.Erase(orders[i].orderkey);
    }
  });
}

// A supplier whose numwait changed, or whose directory row did.
struct Affected {
  int64_t suppkey = 0;
  int64_t old_nw = 0;
  int64_t new_nw = 0;
};

bool BySuppkey(const Affected &a, const Affected &b) { return a.suppkey < b.suppkey; }

// Applies the numwait deltas to `next_core`, one task per run of shards.
// Returns the suppliers whose numwait changed, ascending by suppkey.
std::vector<Affected> ApplyNumwaitDeltas(const core::AuxEngineServices &services,
                                         const Numwait &numwait_delta, TpchQ21Core &next_core) {
  struct NumwaitChange {
    std::size_t shard = 0;
    Affected supplier;
    int64_t delta = 0;
  };
  std::vector<NumwaitChange> changes;
  for (const auto &[suppkey, delta] : numwait_delta)
    if (delta != 0)
      changes.push_back({next_core.numwait.ShardOf(suppkey), {suppkey, 0, 0}, delta});
  core::ParallelStableSort(
      services, changes,
      [](const NumwaitChange &a, const NumwaitChange &b) {
        return a.shard != b.shard ? a.shard < b.shard : a.supplier.suppkey < b.supplier.suppkey;
      },
      kSortGrain);
  const auto runs = core::RunStarts(changes, [](const NumwaitChange &c) { return c.shard; });
  core::ParallelEach(services, runs.size() - 1, kRunGrain, [&](std::size_t run) {
    for (std::size_t k = runs[run]; k < runs[run + 1]; ++k) {
      auto &change = changes[k];
      const int64_t *cur = next_core.numwait.Find(change.supplier.suppkey);
      change.supplier.old_nw = cur ? *cur : 0;
      change.supplier.new_nw = change.supplier.old_nw + change.delta;
      if (change.supplier.new_nw == 0)
        next_core.numwait.Erase(change.supplier.suppkey);
      else
        next_core.numwait[change.supplier.suppkey] = change.supplier.new_nw;
    }
  });
  std::vector<Affected> affected;
  affected.reserve(changes.size());
  for (const auto &change : changes)
    affected.push_back(change.supplier);
  std::sort(affected.begin(), affected.end(), BySuppkey);
  return affected;
}

// Applies the supplier directory deltas to `next_dir` and adds the suppliers
// they touch to `affected` (those not already in its sorted numwait prefix).
void ApplySupplierDeltas(const std::vector<TpchQ21SupplierDelta> &supplier_deltas,
                         const TpchQ21Core &next_core, TpchQ21Dir &next_dir,
                         std::vector<Affected> &affected) {
  const auto numwait_changed = affected.size();
  affected.reserve(numwait_changed + supplier_deltas.size());
  std::unordered_set<int64_t> added; // an UPDATE arrives as a delete delta plus an insert delta
  for (const auto &delta : supplier_deltas) {
    if (!std::binary_search(affected.begin(), affected.begin() + numwait_changed,
                            Affected{delta.suppkey}, BySuppkey) &&
        added.insert(delta.suppkey).second) {
      const int64_t *cur = next_core.numwait.Find(delta.suppkey);
      affected.push_back({delta.suppkey, cur ? *cur : 0, cur ? *cur : 0});
    }
    // had_old only records that a row went away; the final directory state is
    // the last delta per supplier (delete then insert = replace).
    auto &slot = next_dir.overlay[delta.suppkey];
    if (delta.has_new) {
      slot.present = true;
      slot.supplier = {std::make_shared<const std::string>(delta.new_name), delta.new_nationkey};
    } else {
      slot.present = false;
      slot.supplier = {};
    }
  }
}

struct PatchedNations {
  std::vector<int32_t> nations;
  std::vector<RowVec> rows; // parallel to nations
};

// Re-prices the affected suppliers' names in their (old and new) nations'
// rows. Nullopt when a nation image is malformed or a change is inconsistent.
std::optional<PatchedNations>
PatchNations(core::AuxEngineServices &services, const core::VisibilityToken &visibility,
             const TpchQ21Dir &parent_dir, const TpchQ21Dir &next_dir,
             const std::vector<Affected> &affected) {
  std::map<int32_t, std::vector<NameChange>> name_changes;
  for (const auto &supplier : affected) {
    const auto *old_rec = parent_dir.Find(supplier.suppkey);
    const auto *new_rec = next_dir.Find(supplier.suppkey);
    const int64_t old_contribution = std::max<int64_t>(supplier.old_nw, 0);
    const int64_t new_contribution = std::max<int64_t>(supplier.new_nw, 0);
    if (old_rec && old_contribution > 0)
      name_changes[old_rec->nationkey].push_back({old_rec->name, -old_contribution, old_contribution});
    if (new_rec && new_contribution > 0)
      name_changes[new_rec->nationkey].push_back({new_rec->name, new_contribution, 0});
  }

  PatchedNations result;
  std::vector<std::vector<NameChange>> nation_changes;
  std::size_t nation_ops = 0;
  for (auto &[nationkey, changes] : name_changes) {
    result.nations.push_back(nationkey);
    nation_ops += changes.size();
    nation_changes.push_back(std::move(changes));
  }
  const auto &nations = result.nations;
  std::vector<std::shared_ptr<const TpchQ21NationRows>> nation_rows(nations.size());
  if (!nations.empty()) {
    std::vector<core::PartitionId> partitions;
    for (const auto nk : nations)
      partitions.push_back(NationPartition(nk));
    const auto snapshot = services.StateStore().ReadPartitions(visibility, partitions);
    for (const auto &image : snapshot.partitions) {
      if (image.partition < TpchQ21Layout::kNationBase)
        continue;
      const auto nk = static_cast<int32_t>(image.partition - TpchQ21Layout::kNationBase);
      const auto *typed = AsQ21Image(image, TpchQ21ImageKind::Nation);
      if (!typed || typed->nationkey != nk)
        return std::nullopt;
      const auto at = std::find(nations.begin(), nations.end(), nk) - nations.begin();
      nation_rows[static_cast<std::size_t>(at)] = typed->rows;
    }
  }
  const RowVec no_rows; // an absent image = no waiters yet
  result.rows.resize(nations.size());
  std::vector<char> patched_ok(nations.size(), 0);
  core::ParallelEachWhen(services, nations.size(), nation_ops, kNationOpThreshold,
                         [&](std::size_t i) {
                           patched_ok[i] = PatchNationRows(
                               nation_rows[i] ? nation_rows[i]->rows : no_rows,
                               std::move(nation_changes[i]), result.rows[i]);
                         });
  if (std::find(patched_ok.begin(), patched_ok.end(), 0) != patched_ok.end())
    return std::nullopt;
  return result;
}

// Publishes only the images that changed: the order summaries, the supplier
// directory, and each patched nation's rows (also under its name partition).
void PublishImages(core::StagedMaintenanceOutcome &outcome, const TpchQ21TxnBuffer &buffer,
                   std::shared_ptr<const TpchQ21Core> next_core,
                   std::shared_ptr<const TpchQ21Dir> next_dir,
                   const TpchQ21NationIndex &nation_index, PatchedNations &patched) {
  outcome.action = core::MaintenanceAction::ApplyDelta;
  if (!buffer.LineitemDeltas().empty() || !buffer.OrderDeltas().empty()) {
    outcome.updates.push_back(core::StateImage::Make(
        TpchQ21Layout::kAccelerator, TpchQ21Layout::kOrderSummaries, 0,
        MakeCoreImage(std::move(next_core))));
  }
  if (!buffer.SupplierDeltas().empty()) {
    outcome.updates.push_back(core::StateImage::Make(
        TpchQ21Layout::kAccelerator, TpchQ21Layout::kSupplierMetadata, 0,
        MakeDirImage(std::move(next_dir))));
  }

  // nationkey -> unique name (duplicate names keep the slow path).
  std::unordered_map<int32_t, std::string> nk_to_name;
  for (const auto &[name, nkeys] : nation_index.nation_name_to_keys)
    if (nkeys.size() == 1)
      nk_to_name.emplace(nkeys.front(), name);
  for (std::size_t i = 0; i < patched.nations.size(); ++i) {
    const int32_t nk = patched.nations[i];
    auto rows = std::make_shared<TpchQ21NationRows>();
    rows->rows = std::move(patched.rows[i]);
    const std::shared_ptr<const TpchQ21NationRows> shared = std::move(rows);
    outcome.updates.push_back(core::StateImage::Make(
        TpchQ21Layout::kAccelerator, NationPartition(nk), 0, MakeNationImage(nk, shared)));
    const auto name_it = nk_to_name.find(nk);
    if (name_it != nk_to_name.end()) {
      outcome.updates.push_back(core::StateImage::Make(
          TpchQ21Layout::kAccelerator, NationNamePartition(name_it->second), 0,
          MakeNationImage(nk, shared, name_it->second)));
    }
  }
}

// Order keys in hash shards: the fulfilled orders, or the per-supplier
// lineitem counts (an OrderSummary) of the orders seen. The shards are the
// coarsest level of the final order map's shard hash, so a task owning one
// build shard writes only its own shards of that map.
void AddSupplier(OrderSummary &summary, int64_t suppkey, int64_t total, int64_t late) {
  if (auto *info = summary.FindSupplier(suppkey)) {
    info->total_count += total;
    info->late_count += late;
  } else {
    summary.suppliers.push_back({suppkey, total, late});
  }
}

// Build rows are dealt into buckets by the hash prefix of their key. A map of
// shard_bits >= kRowBucketBits shards refines the buckets, so whole shards
// belong to one bucket; a coarser one groups buckets.
std::size_t BucketOf(int64_t key) {
  return core::ShardedCowHashMap<int64_t, 7>::ShardOf(key, core::kRowBucketBits);
}

// Runs fn(first_bucket, last_bucket) per group of buckets that a sharded map of
// `shard_bits` writes from one task: no two groups touch the same shard.
template <class Fn>
void ForEachShardGroup(const core::AuxEngineServices &services, unsigned shard_bits, Fn &&fn) {
  const unsigned group_bits = std::min(shard_bits, core::kRowBucketBits);
  const std::size_t per_group = std::size_t{1} << (core::kRowBucketBits - group_bits);
  core::ParallelEach(services, std::size_t{1} << group_bits, 1, [&](std::size_t g) {
    fn(g * per_group, (g + 1) * per_group);
  });
}

struct LineRow {
  int64_t orderkey;
  int64_t suppkey;
  bool late;
};

// The orders of one bucket that contribute to the answer.
struct OrderBucket {
  std::vector<std::pair<int64_t, OrderSummary>> contributing;
  std::vector<std::pair<int64_t, int64_t>> numwait; // (suppkey, late lineitems) per order
};

OrderBucket SummarizeBucket(std::vector<LineRow> lines) {
  // Stable: an order's suppliers stay in table order.
  std::stable_sort(lines.begin(), lines.end(), [](const LineRow &a, const LineRow &b) {
    return a.orderkey < b.orderkey;
  });
  OrderBucket out;
  for (std::size_t begin = 0, end = 0; begin < lines.size(); begin = end) {
    OrderSummary summary;
    summary.orderstatus = 'F';
    for (end = begin; end < lines.size() && lines[end].orderkey == lines[begin].orderkey; ++end)
      AddSupplier(summary, lines[end].suppkey, 1, lines[end].late ? 1 : 0);
    int64_t suppkey = 0, count = 0;
    if (!ComputeOrderContribution(summary, suppkey, count))
      continue;
    if (count != 0)
      out.numwait.emplace_back(suppkey, count);
    out.contributing.emplace_back(lines[begin].orderkey, std::move(summary));
  }
  return out;
}

using Fulfilled = std::vector<std::vector<int64_t>>; // per bucket, sorted

Fulfilled ScanFulfilledOrders(core::AuxEngineServices &services, TpchQ21InitialScan &scan) {
  auto rows = core::PartitionedBuild<core::BucketedRows<int64_t>>(
      services, [] { return core::BucketedRows<int64_t>{}; },
      [&](std::size_t parts, auto &&sink) {
        scan.ScanOrders(services, parts, [&](std::size_t p, const TpchQ21OrderInput &input) {
          sink(p, core::SingleRowBatch<TpchQ21OrderInput>{input});
        });
      },
      [](core::BucketedRows<int64_t> &partial, const TpchQ21OrderInput &input) {
        if (input.orderstatus == 'F')
          partial.Add(BucketOf(input.orderkey), input.orderkey);
      },
      [](core::BucketedRows<int64_t> &into, core::BucketedRows<int64_t> &&from) {
        into.Absorb(std::move(from));
      });
  Fulfilled fulfilled(core::kRowBuckets);
  core::ParallelEach(services, core::kRowBuckets, 1, [&](std::size_t b) {
    auto &keys = fulfilled[b];
    keys = rows.Take(b);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  });
  return fulfilled;
}

// The lineitems of fulfilled orders, dealt by order.
core::BucketedRows<LineRow> ScanFulfilledLineitems(core::AuxEngineServices &services,
                                                   TpchQ21InitialScan &scan,
                                                   const Fulfilled &fulfilled) {
  return core::PartitionedBuild<core::BucketedRows<LineRow>>(
      services, [] { return core::BucketedRows<LineRow>{}; },
      [&](std::size_t parts, auto &&sink) {
        scan.ScanLineitems(services, parts, [&](std::size_t p, const TpchQ21LineitemInput &input) {
          sink(p, core::SingleRowBatch<TpchQ21LineitemInput>{input});
        });
      },
      [&](core::BucketedRows<LineRow> &partial, const TpchQ21LineitemInput &input) {
        const std::size_t bucket = BucketOf(input.orderkey);
        if (std::binary_search(fulfilled[bucket].begin(), fulfilled[bucket].end(), input.orderkey))
          partial.Add(bucket, {input.orderkey, input.suppkey, input.receiptdate > input.commitdate});
      },
      [](core::BucketedRows<LineRow> &into, core::BucketedRows<LineRow> &&from) {
        into.Absorb(std::move(from));
      });
}

// Per-supplier numwait totals: the (suppkey, count) rows of all buckets summed
// per supplier bucket.
struct NumwaitRow {
  int64_t suppkey;
  int64_t count;
};

std::vector<std::vector<NumwaitRow>>
TotalNumwait(const core::AuxEngineServices &services, const std::vector<OrderBucket> &buckets) {
  // Counting scatter of each order bucket's rows by supplier bucket.
  struct Scattered {
    std::vector<NumwaitRow> rows;
    std::vector<std::size_t> start;
  };
  std::vector<Scattered> scattered(buckets.size());
  core::ParallelEach(services, buckets.size(), 1, [&](std::size_t b) {
    auto &out = scattered[b];
    const auto &rows = buckets[b].numwait;
    out.start.assign(core::kRowBuckets + 1, 0);
    for (const auto &row : rows)
      ++out.start[BucketOf(row.first) + 1];
    for (std::size_t g = 0; g < core::kRowBuckets; ++g)
      out.start[g + 1] += out.start[g];
    auto next = std::vector<std::size_t>(out.start.begin(), out.start.end() - 1);
    out.rows.resize(rows.size());
    for (const auto &row : rows)
      out.rows[next[BucketOf(row.first)]++] = {row.first, row.second};
  });
  std::vector<std::vector<NumwaitRow>> totals(core::kRowBuckets);
  core::ParallelEach(services, core::kRowBuckets, 1, [&](std::size_t g) {
    core::FlatHashMap<int64_t> sums;
    for (const auto &from : scattered)
      for (std::size_t i = from.start[g]; i < from.start[g + 1]; ++i)
        sums[from.rows[i].suppkey] += from.rows[i].count;
    for (auto entry : sums)
      totals[g].push_back({entry.first, entry.second});
  });
  return totals;
}

// The answer rows of every nation: its suppliers' numwait merged by name and
// ordered (numwait DESC, name ASC).
std::map<int32_t, std::shared_ptr<const TpchQ21NationRows>>
NationRows(const core::AuxEngineServices &services, const TpchQ21State &state,
           const std::vector<std::vector<NumwaitRow>> &numwait) {
  using ByNation = std::map<int32_t, std::vector<TpchQ21ImageRow>>;
  std::vector<ByNation> per_group(numwait.size());
  core::ParallelEach(services, numwait.size(), 1, [&](std::size_t g) {
    for (const auto &row : numwait[g]) {
      if (row.count <= 0)
        continue;
      if (const auto *supplier = state.dir->Find(row.suppkey))
        per_group[g][supplier->nationkey].push_back({supplier->name, row.count});
    }
  });
  std::set<int32_t> nationkeys;
  for (const auto &by_nation : per_group)
    for (const auto &entry : by_nation)
      nationkeys.insert(entry.first);
  const std::vector<int32_t> keys(nationkeys.begin(), nationkeys.end());
  std::vector<std::shared_ptr<TpchQ21NationRows>> rows(keys.size());
  core::ParallelEach(services, keys.size(), 1, [&](std::size_t i) {
    std::vector<TpchQ21ImageRow> all;
    for (const auto &by_nation : per_group) {
      const auto it = by_nation.find(keys[i]);
      if (it != by_nation.end())
        all.insert(all.end(), it->second.begin(), it->second.end());
    }
    std::stable_sort(all.begin(), all.end(), [](const TpchQ21ImageRow &a, const TpchQ21ImageRow &b) {
      return *a.name < *b.name;
    });
    auto merged = std::make_shared<TpchQ21NationRows>();
    for (const auto &row : all) {
      if (!merged->rows.empty() && *merged->rows.back().name == *row.name)
        merged->rows.back().numwait += row.numwait;
      else
        merged->rows.push_back(row);
    }
    std::sort(merged->rows.begin(), merged->rows.end(), ImageRowBefore);
    rows[i] = std::move(merged);
  });
  std::map<int32_t, std::shared_ptr<const TpchQ21NationRows>> nations;
  for (std::size_t i = 0; i < keys.size(); ++i)
    nations[keys[i]] = std::move(rows[i]);
  return nations;
}

} // namespace

bool TpchQ21TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ21TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  lineitem_deltas.clear();
  order_deltas.clear();
  supplier_deltas.clear();
  nation_deltas.clear();
}
bool TpchQ21TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
const std::vector<TpchQ21LineitemDelta> &TpchQ21TxnBuffer::LineitemDeltas() const {
  return lineitem_deltas;
}
const std::vector<TpchQ21OrderDelta> &TpchQ21TxnBuffer::OrderDeltas() const {
  return order_deltas;
}
const std::vector<TpchQ21SupplierDelta> &TpchQ21TxnBuffer::SupplierDeltas() const {
  return supplier_deltas;
}
const std::vector<TpchQ21NationDelta> &TpchQ21TxnBuffer::NationDeltas() const {
  return nation_deltas;
}
void TpchQ21TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  lineitem_deltas.clear();
  order_deltas.clear();
  supplier_deltas.clear();
  nation_deltas.clear();
}
void TpchQ21TxnBuffer::AddLineitemDelta(TpchQ21LineitemDelta delta) {
  pending = true;
  lineitem_deltas.push_back(delta);
}
void TpchQ21TxnBuffer::AddOrderDelta(TpchQ21OrderDelta delta) {
  pending = true;
  order_deltas.push_back(delta);
}
void TpchQ21TxnBuffer::AddSupplierDelta(TpchQ21SupplierDelta delta) {
  pending = true;
  supplier_deltas.push_back(std::move(delta));
}
void TpchQ21TxnBuffer::AddNationDelta(TpchQ21NationDelta delta) {
  pending = true;
  nation_deltas.push_back(std::move(delta));
}

core::AcceleratorId TpchQ21Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ21Module::Dependencies() const {
  return {{config.supplier_table, {0, 1, 2}, {0, 1, 3}},
          {config.orders_table, {0, 1}, {0, 2}},
          {config.lineitem_table, {0, 1, 2, 3}, {0, 2, 11, 12}},
          {config.nation_table, {0, 1}, {0, 1}}};
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ21Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ21TxnBuffer>();
}

void TpchQ21Module::Build(core::AuxEngineServices &services,
                           core::StateBuilder &builder) {
  const auto state = BuildState(services);
  const std::shared_ptr<const TpchQ21Dir> dir = state.dir;
  const std::shared_ptr<const TpchQ21Core> core_state = state.core;
  const std::shared_ptr<const TpchQ21NationIndex> index = state.nation_index;

  builder.Write(core::StateImage::Make(
      TpchQ21Layout::kAccelerator, TpchQ21Layout::kSupplierMetadata, 0,
      MakeDirImage(dir)));
  builder.Write(core::StateImage::Make(
      TpchQ21Layout::kAccelerator, TpchQ21Layout::kOrderSummaries, 0,
      MakeCoreImage(core_state)));
  builder.Write(core::StateImage::Make(
      TpchQ21Layout::kAccelerator, TpchQ21Layout::kNationNameIndex, 0,
      MakeIndexImage(index)));
  // Persist NationPartition images so StageMaintenance can patch them.
  for (const auto &[nk, rows] : state.nations) {
    if (rows->rows.empty())
      continue;
    builder.Write(core::StateImage::Make(
        TpchQ21Layout::kAccelerator, NationPartition(nk), 0,
        MakeNationImage(nk, rows)));
  }
  // Materialize the result at the query-parameter grain: one FNV-1a-hashed
  // partition read directly from the nation name, collision-checked by name.
  for (const auto &[name, nationkeys] : index->nation_name_to_keys) {
    if (nationkeys.size() != 1)
      continue; // uncommon duplicate names retain the correctness slow path.
    const int32_t nk = nationkeys.front();
    const auto it = state.nations.find(nk);
    auto rows = it != state.nations.end()
                    ? it->second
                    : std::make_shared<const TpchQ21NationRows>();
    builder.Write(core::StateImage::Make(
        TpchQ21Layout::kAccelerator, NationNamePartition(name), 0,
        MakeNationImage(nk, std::move(rows), name)));
  }
  builder.Finish();
}

core::InvalidationPlan
TpchQ21Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.lineitem_table ||
      mutation.table == config.orders_table ||
      mutation.table == config.supplier_table ||
      mutation.table == config.nation_table) {
    core::InvalidationPlan plan;
    plan.scope =
        ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
    return plan;
  }
  return {};
}

void TpchQ21Module::AccumulateMutation(const core::MutationBatch &mutation,
                                        core::AuxEngineServices &,
                                        core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ21TxnBuffer &>(base_buffer);

  if (mutation.table == config.supplier_table) {
    // Supplier changes patch the directory and the affected nations' rows.
    const auto remove = [&](const core::AuxRow &row) {
      int64_t key = 0;
      std::string name;
      int32_t nationkey = 0;
      if (!ReadSupplier(row, key, name, nationkey)) {
        buffer.MarkRebuild();
        return;
      }
      TpchQ21SupplierDelta d;
      d.suppkey = key;
      d.had_old = true;
      buffer.AddSupplierDelta(std::move(d));
    };
    const auto put = [&](const core::AuxRow &row) {
      TpchQ21SupplierDelta d;
      if (!ReadSupplier(row, d.suppkey, d.new_name, d.new_nationkey)) {
        buffer.MarkRebuild();
        return;
      }
      d.has_new = true;
      buffer.AddSupplierDelta(std::move(d));
    };
    switch (mutation.kind) {
    case core::MutationKind::Insert:
      for (const auto &row : mutation.new_rows)
        put(row);
      break;
    case core::MutationKind::Delete:
      for (const auto &row : mutation.old_rows)
        remove(row);
      break;
    case core::MutationKind::Update:
      if (mutation.old_rows.size() != mutation.new_rows.size()) {
        buffer.MarkRebuild();
        return;
      }
      for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
        int64_t ok = 0, nk = 0;
        std::string oname, nname;
        int32_t onation = 0, nnation = 0;
        if (ReadSupplier(mutation.old_rows[i], ok, oname, onation) &&
            ReadSupplier(mutation.new_rows[i], nk, nname, nnation) &&
            ok == nk && oname == nname && onation == nnation)
          continue; // irrelevant columns changed only
        remove(mutation.old_rows[i]);
        put(mutation.new_rows[i]);
      }
      break;
    }
    return;
  }

  if (mutation.table == config.orders_table) {
    auto add = [&](const core::AuxRow &row, int8_t sign) {
      int64_t orderkey = 0;
      char orderstatus = 0;
      if (!ReadOrder(row, orderkey, orderstatus)) {
        buffer.MarkRebuild();
        return;
      }
      buffer.AddOrderDelta({orderkey, static_cast<char>(sign > 0 ? 0 : orderstatus),
                            static_cast<char>(sign > 0 ? orderstatus : 0)});
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
        int64_t old_orderkey = 0, new_orderkey = 0;
        char old_status = 0, new_status = 0;
        if (!ReadOrder(mutation.old_rows[i], old_orderkey, old_status) ||
            !ReadOrder(mutation.new_rows[i], new_orderkey, new_status) ||
            old_orderkey != new_orderkey) {
          buffer.MarkRebuild();
          return;
        }
        if (old_status == new_status)
          continue; // only untracked columns changed
        buffer.AddOrderDelta({old_orderkey, old_status, new_status});
      }
      break;
    }
    return;
  }

  if (mutation.table == config.nation_table) {
    // Nation changes can rename/remove a name whose parameter-keyed
    // NationNamePartition image would otherwise stay Valid and serve stale
    // rows.  Nations are effectively static; rebuild instead of patching.
    buffer.MarkRebuild();
    return;
  }

  if (mutation.table != config.lineitem_table)
    return;

  auto add = [&](const core::AuxRow &row, int8_t sign) {
    int64_t orderkey = 0, suppkey = 0;
    int32_t commitdate = 0, receiptdate = 0;
    if (!ReadLineitem(row, orderkey, suppkey, commitdate, receiptdate)) {
      buffer.MarkRebuild();
      return;
    }
    buffer.AddLineitemDelta({orderkey, suppkey, commitdate, receiptdate, sign});
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
      int64_t ook = 0, osk = 0, nok = 0, nsk = 0;
      int32_t ocd = 0, ord = 0, ncd = 0, nrd = 0;
      if (ReadLineitem(mutation.old_rows[i], ook, osk, ocd, ord) &&
          ReadLineitem(mutation.new_rows[i], nok, nsk, ncd, nrd) &&
          ook == nok && osk == nsk && ocd == ncd && ord == nrd)
        continue; // only untracked columns changed
      add(mutation.old_rows[i], -1);
      add(mutation.new_rows[i], 1);
    }
    break;
  }
}

core::StagedMaintenanceOutcome
TpchQ21Module::StageMaintenance(core::AuxEngineServices &services,
                                 const core::InvalidationPlan &,
                                 core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ21TxnBuffer &>(base_buffer);
  core::StagedMaintenanceOutcome outcome;

  if (buffer.RequiresRebuild()) {
    const auto decision = services.ChooseMaintenance(
        core::MaintenanceRequest::RebuildOnly(services.ExpectedQueriesBeforeNextWrite()));
    core::FinishRebuildAll(outcome, decision);
    return outcome;
  }

  const auto total_deltas = buffer.LineitemDeltas().size() + buffer.OrderDeltas().size() +
                            buffer.SupplierDeltas().size() + buffer.NationDeltas().size();
  if (total_deltas == 0)
    return outcome;
  const auto decision = services.ChooseMaintenance(
      core::MaintenanceRequest::IncrementalSingleState(
          total_deltas, services.ExpectedQueriesBeforeNextWrite()));
  if (decision.action != core::MaintenanceAction::ApplyDelta) {
    core::FinishRebuildAll(outcome, decision);
    return outcome;
  }

  const auto visibility = services.CurrentVisibility();
  const auto parents = LoadParents(services, visibility);
  if (!parents) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }
  // Staged copies share every shard with the parents; a write clones only the
  // shard it touches, so published versions are never mutated.
  auto next_core = std::make_shared<TpchQ21Core>(*parents->order_core);
  auto next_dir = std::make_shared<TpchQ21Dir>(*parents->supplier_dir);

  auto order_updates = ComputeOrderUpdates(services, initial_scan, buffer, *parents->order_core);
  if (!order_updates) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }
  ApplyOrderUpdates(services, *order_updates, *next_core);
  auto affected = ApplyNumwaitDeltas(services, order_updates->numwait_delta, *next_core);
  ApplySupplierDeltas(buffer.SupplierDeltas(), *next_core, *next_dir, affected);
  auto patched = PatchNations(services, visibility, *parents->supplier_dir, *next_dir, affected);
  if (!patched) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }
  PublishImages(outcome, buffer, std::move(next_core), std::move(next_dir),
                *parents->nation_index, *patched);
  return outcome;
}

bool TpchQ21Module::CheckValidity(core::AuxEngineServices &services,
                                   const core::QueryArguments &arguments) const {
  if (arguments.values.empty())
    return false;
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void TpchQ21Module::Execute(core::AuxEngineServices &services,
                             const core::QueryArguments &arguments,
                             core::AuxRowSink &sink) {
  if (arguments.values.empty() || arguments.values[0].is_null) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  std::string nation_name;
  if (std::holds_alternative<std::string>(arguments.values[0].payload)) {
    nation_name = std::get<std::string>(arguments.values[0].payload);
  } else {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  if (!services.StateStore()
           .CheckValidity({Id(), {}}, services.CurrentVisibility())
           .IsValid()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  const auto visibility = services.CurrentVisibility();

  // Fast path: single-partition read via FNV-1a nation-name hash.
  {
    const auto fast_snapshot = services.StateStore().ReadPartitions(
        visibility, {NationNamePartition(nation_name)});
    for (const auto &image : fast_snapshot.partitions) {
      const auto *typed = AsQ21Image(image, TpchQ21ImageKind::Nation);
      if (!typed) continue;
      // Collision guard: verify the stored nation_name matches the query.
      if (typed->nation_name != nation_name) continue;
      // An empty hit falls through to the NationPartition path rather than
      // being treated as authoritative.
      if (!typed->rows->rows.empty()) {
        sink.Begin(core::OutputSchema{});
        // rows are kept in output order (numwait DESC, s_name): TPC-H's LIMIT 100
        const auto n = std::min(typed->rows->rows.size(), kTpchQ21Limit);
        for (std::size_t i = 0; i < n; ++i)
          EmitRow(sink, *typed->rows->rows[i].name, typed->rows->rows[i].numwait);
        sink.End();
        return;
      }
      break;
    }
  }

  // Slow path: resolve the nation name via the tiny NationNameIndex.
  const auto idx_snapshot = services.StateStore().ReadPartitions(
      visibility, {TpchQ21Layout::kNationNameIndex});
  std::shared_ptr<const TpchQ21NationIndex> idx_state;
  for (const auto &image : idx_snapshot.partitions) {
    if (image.partition == TpchQ21Layout::kNationNameIndex) {
      const auto *typed = AsQ21Image(image, TpchQ21ImageKind::NationNameIndex);
      if (typed) idx_state = typed->nation_index;
      break;
    }
  }
  if (!idx_state) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  const auto name_it = idx_state->nation_name_to_keys.find(nation_name);
  if (name_it == idx_state->nation_name_to_keys.end() || name_it->second.empty()) {
    sink.Begin(core::OutputSchema{});
    sink.End();
    return;
  }

  std::vector<std::shared_ptr<const TpchQ21NationRows>> nations;
  if (!LoadNationRows(services, name_it->second, nations)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  EmitMerged(nations, sink);
}

TpchQ21State
TpchQ21Module::BuildState(core::AuxEngineServices &services) const {
  TpchQ21State state;
  if (!initial_scan)
    throw std::logic_error("TPC-H Q21 aux requires a typed initial scan source");

  // Suppliers: the scan's pieces become the directory in one pass.
  using Suppliers = std::unordered_map<int64_t, TpchQ21Supplier>;
  using SupplierPieces = core::PartitionPieces<std::pair<int64_t, TpchQ21Supplier>>;
  auto supplier_pieces = core::PartitionedBuild<SupplierPieces>(
                             services, [] { return SupplierPieces{}; },
                             [&](std::size_t parts, auto &&sink) {
                               initial_scan->ScanSuppliers(
                                   services, parts,
                                   [&](std::size_t p, const TpchQ21SupplierInput &input) {
                                     sink(p, core::SingleRowBatch<TpchQ21SupplierInput>{input});
                                   });
                             },
                             [](SupplierPieces &partial, const TpchQ21SupplierInput &input) {
                               partial.Add({input.suppkey,
                                            {std::make_shared<const std::string>(input.name),
                                             input.nationkey}});
                             },
                             [](SupplierPieces &into, SupplierPieces &&from) {
                               into.Absorb(std::move(from));
                             })
                             .Take();
  auto suppliers = std::make_shared<Suppliers>();
  suppliers->reserve(core::PieceOffsets(supplier_pieces).back());
  for (auto &piece : supplier_pieces)
    for (auto &[suppkey, supplier] : piece)
      (*suppliers)[suppkey] = std::move(supplier); // a later row of a key wins
  state.dir->base = std::move(suppliers);

  initial_scan->ScanNationsSerial([&](const TpchQ21NationInput &input) {
    state.nation_index->nation_name_to_keys[input.name].push_back(
        input.nationkey);
  });

  // The contributing orders and the late lineitems of their suppliers, bucket by bucket.
  const auto fulfilled = ScanFulfilledOrders(services, *initial_scan);
  auto lines = ScanFulfilledLineitems(services, *initial_scan, fulfilled);
  std::vector<OrderBucket> buckets(core::kRowBuckets);
  core::ParallelEach(services, core::kRowBuckets, 1, [&](std::size_t b) {
    buckets[b] = SummarizeBucket(lines.Take(b));
  });

  // Both maps keep ~48 entries per shard; the shards of an order or supplier
  // bucket are written by one task.
  std::size_t contributing = 0;
  for (const auto &bucket : buckets)
    contributing += bucket.contributing.size();
  const auto numwait = TotalNumwait(services, buckets);
  std::size_t suppliers_waiting = 0;
  for (const auto &group : numwait)
    suppliers_waiting += group.size();
  const unsigned order_bits =
      std::max(6u, core::ShardedCowHashMap<OrderSummary, 7>::ShardBitsFor(contributing));
  const unsigned numwait_bits = core::ShardedCowHashMap<int64_t, 7>::ShardBitsFor(suppliers_waiting);
  state.core = std::make_shared<TpchQ21Core>(order_bits, numwait_bits);
  // Fresh, exactly-sized copies: the build's per-order scratch frees without
  // leaving the state scattered through half-used heap pages.
  ForEachShardGroup(services, order_bits, [&](std::size_t first, std::size_t last) {
    for (std::size_t b = first; b < last; ++b)
      for (auto &[orderkey, order] : buckets[b].contributing) {
        auto &kept = state.core->orders[orderkey];
        kept.orderstatus = order.orderstatus;
        kept.suppliers.assign(order.suppliers.begin(), order.suppliers.end());
      }
  });
  ForEachShardGroup(services, numwait_bits, [&](std::size_t first, std::size_t last) {
    for (std::size_t g = first; g < last; ++g)
      for (const auto &row : numwait[g])
        state.core->numwait[row.suppkey] = row.count;
  });

  state.nations = NationRows(services, state, numwait);
  return state;
}

bool TpchQ21Module::LoadNationRows(
    core::AuxEngineServices &services, const std::vector<int32_t> &nationkeys,
    std::vector<std::shared_ptr<const TpchQ21NationRows>> &out) const {
  std::vector<core::PartitionId> partitions;
  for (const auto nk : nationkeys)
    partitions.push_back(NationPartition(nk));
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), partitions);
  for (const auto &image : snapshot.partitions) {
    if (image.partition < TpchQ21Layout::kNationBase)
      continue;
    const auto nk = static_cast<int32_t>(image.partition -
                                         TpchQ21Layout::kNationBase);
    const auto *typed = AsQ21Image(image, TpchQ21ImageKind::Nation);
    if (!typed || typed->nationkey != nk)
      return false;
    out.push_back(typed->rows);
  }
  // A nation without a published image has no waiters.
  return true;
}

void TpchQ21Module::EmitMerged(
    const std::vector<std::shared_ptr<const TpchQ21NationRows>> &nations,
    core::AuxRowSink &sink) const {
  sink.Begin(core::OutputSchema{});
  if (nations.size() == 1) {
    const auto &rows = nations.front()->rows;
    const auto n = std::min(rows.size(), kTpchQ21Limit);
    for (std::size_t i = 0; i < n; ++i)
      EmitRow(sink, *rows[i].name, rows[i].numwait);
    sink.End();
    return;
  }
  // Several nations share the name: merge rows by s_name across them.
  std::map<std::string, int64_t> by_name;
  for (const auto &n : nations)
    for (const auto &row : n->rows)
      by_name[*row.name] += row.numwait;
  std::vector<TpchQ21OutputRow> output_rows;
  output_rows.reserve(by_name.size());
  for (auto &[name, numwait] : by_name)
    output_rows.push_back({name, numwait});
  std::sort(output_rows.begin(), output_rows.end(), BetterOutputRow);
  if (output_rows.size() > kTpchQ21Limit)
    output_rows.resize(kTpchQ21Limit);
  for (const auto &row : output_rows)
    EmitRow(sink, row.name, row.numwait);
  sink.End();
}

std::vector<core::CalibrationWrite> TpchQ21Module::CalibrationWrites() const {
  return TpchQ21CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
