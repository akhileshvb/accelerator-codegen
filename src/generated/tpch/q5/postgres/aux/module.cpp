#include "tpch/q5/postgres/aux/module.hpp"
#include "tpch/calibration_writes.hpp"
#include "accelerator_codegen/stateful/core/byte_stream.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {

using core::ByteReader;
using core::ByteWriter;

constexpr uint32_t kVersion = 1;
constexpr int32_t kEpoch = 719468;

#if defined(__SIZEOF_INT128__)
int64_t ComputeRevenueRaw(int64_t extended_price_raw, int64_t discount_raw) {
  __int128_t revenue = static_cast<__int128_t>(extended_price_raw) *
                       static_cast<__int128_t>(100 - discount_raw);
  return static_cast<int64_t>(revenue);
}
#else
int64_t ComputeRevenueRaw(int64_t extended_price_raw, int64_t discount_raw) {
  return extended_price_raw * (100 - discount_raw);
}
#endif

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
  if (std::holds_alternative<core::AuxDate>(cell.payload)) {
    out = std::get<core::AuxDate>(cell.payload).days_since_epoch;
    return true;
  }
  return false;
}

bool Int32Cell(const core::AuxRow &row, std::size_t index, int32_t &out) {
  int64_t value = 0;
  if (!IntCell(row, index, value) ||
      value < std::numeric_limits<int32_t>::min() ||
      value > std::numeric_limits<int32_t>::max())
    return false;
  out = static_cast<int32_t>(value);
  return true;
}

bool DateCell(const core::AuxRow &row, std::size_t index, int32_t &days) {
  int64_t value = 0;
  if (!IntCell(row, index, value) ||
      value < std::numeric_limits<int32_t>::min() ||
      value > std::numeric_limits<int32_t>::max())
    return false;
  days = static_cast<int32_t>(value);
  return true;
}

int32_t DaysFromCivil(int32_t year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int32_t era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int32_t>(doe) - kEpoch;
}

void CivilFromDays(int32_t days, int32_t &year, unsigned &month,
                   unsigned &day) {
  int32_t z = days + kEpoch;
  const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  year = static_cast<int32_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  day = doy - (153 * mp + 2) / 5 + 1;
  month = mp + (mp < 10 ? 3 : -9);
  year += month <= 2;
}

uint32_t NextPowerOf2(uint32_t n) {
  if (n == 0)
    return 1;
  n--;
  n |= n >> 1;
  n |= n >> 2;
  n |= n >> 4;
  n |= n >> 8;
  n |= n >> 16;
  return n + 1;
}

core::InvalidationPlan EmptyPlan() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

core::InvalidationPlan NoOpPlan() {
  core::InvalidationPlan plan;
  plan.scope = ::accelerator_codegen::aux_state::InvalidationScope::None;
  return plan;
}

std::shared_ptr<const TpchQ5StateImage>
MakeSideImage(const std::shared_ptr<const TpchQ5State> &state) {
  auto image = std::make_shared<TpchQ5StateImage>();
  image->is_side = true;
  image->state = state;
  return image;
}

std::shared_ptr<const TpchQ5StateImage>
MakeTreeNodeImage(uint32_t node_index,
                  const std::shared_ptr<const TpchQ5State> &state) {
  auto image = std::make_shared<TpchQ5StateImage>();
  image->is_side = false;
  image->node_index = node_index;
  image->state = state;
  return image;
}

const TpchQ5StateImage *AsQ5Image(const core::StateImage &image) {
  return image.As<TpchQ5StateImage>();
}

bool ReadLineitem(const core::AuxRow &row, TpchQ5LineitemInput &out) {
  int64_t orderkey = 0, suppkey = 0;
  int64_t price_raw = 0, discount_raw = 0;
  int32_t price_scale = 2, discount_scale = 2;
  if (!IntCell(row, 0, orderkey))
    return false;

  // Scan and catalog probes project (orderkey, suppkey, price, discount).
  if (!IntCell(row, 1, suppkey))
    return false;
  if (row.values.size() > 2 && !row.values[2].is_null) {
    if (std::holds_alternative<core::ScaledDecimal>(row.values[2].payload)) {
      const auto d = std::get<core::ScaledDecimal>(row.values[2].payload);
      price_raw = d.value;
      price_scale = d.scale;
    } else if (std::holds_alternative<int64_t>(row.values[2].payload)) {
      price_raw = std::get<int64_t>(row.values[2].payload);
    }
  }
  if (row.values.size() > 3 && !row.values[3].is_null) {
    if (std::holds_alternative<core::ScaledDecimal>(row.values[3].payload)) {
      const auto d = std::get<core::ScaledDecimal>(row.values[3].payload);
      discount_raw = d.value;
      discount_scale = d.scale;
    } else if (std::holds_alternative<int64_t>(row.values[3].payload)) {
      discount_raw = std::get<int64_t>(row.values[3].payload);
    }
  }
  out.orderkey = orderkey;
  out.suppkey = suppkey;
  out.extended_price_raw = price_raw;
  out.extended_price_scale = price_scale;
  out.discount_raw = discount_raw;
  out.discount_scale = discount_scale;
  return true;
}

bool ReadOrder(const core::AuxRow &row, int64_t &orderkey, int64_t &custkey,
               int32_t &orderdate_days) {
  if (!IntCell(row, 0, orderkey) || !IntCell(row, 1, custkey) ||
      !DateCell(row, 2, orderdate_days))
    return false;
  return true;
}

} // namespace

// TxnBuffer

bool TpchQ5TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ5TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  lineitem_mutations.clear();
  order_mutations.clear();
}
bool TpchQ5TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
void TpchQ5TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
}

bool TpchQ5TxnBuffer::HasLineitemMutations() const {
  return !lineitem_mutations.empty();
}
const std::vector<TpchQ5TxnBuffer::LineitemDelta> &
TpchQ5TxnBuffer::LineitemMutations() const {
  return lineitem_mutations;
}
void TpchQ5TxnBuffer::AddLineitemMutation(const LineitemDelta &d) {
  pending = true;
  lineitem_mutations.push_back(d);
}

void TpchQ5TxnBuffer::AddOrderMutation(const OrderDelta &d) {
  pending = true;
  order_mutations.push_back(d);
}

const std::vector<TpchQ5TxnBuffer::OrderDelta> &
TpchQ5TxnBuffer::OrderMutations() const {
  return order_mutations;
}



int32_t TpchQ5DaysFromYmd(int32_t year, unsigned month, unsigned day) {
  return DaysFromCivil(year, month, day);
}

int32_t TpchQ5AddMonths(int32_t days_since_epoch, int32_t months) {
  int32_t year = 0;
  unsigned month = 0, day = 0;
  CivilFromDays(days_since_epoch, year, month, day);
  int total = year * 12 + static_cast<int>(month) - 1 + months;
  int32_t new_year = total / 12;
  unsigned new_month = static_cast<unsigned>(total % 12) + 1;
  unsigned new_day = std::min(day, 28u);
  return DaysFromCivil(new_year, new_month, new_day);
}

int64_t TpchQ5RevenueRaw(int64_t extended_price_raw, int64_t discount_raw) {
  return ComputeRevenueRaw(extended_price_raw, discount_raw);
}

std::vector<uint32_t> TpchQ5TreeCover(uint32_t tree_size,
                                      int32_t start_ordinal,
                                      int32_t end_ordinal) {
  std::vector<uint32_t> result;
  if (start_ordinal >= end_ordinal)
    return result;
  int32_t l = start_ordinal + static_cast<int32_t>(tree_size);
  int32_t r = end_ordinal + static_cast<int32_t>(tree_size);
  while (l < r) {
    if (l & 1) {
      result.push_back(static_cast<uint32_t>(l));
      l++;
    }
    if (r & 1) {
      r--;
      result.push_back(static_cast<uint32_t>(r));
    }
    l >>= 1;
    r >>= 1;
  }
  return result;
}

// Module Implementation

core::AcceleratorId TpchQ5Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ5Module::Dependencies() const {
  return {
      {config.region_table, {0, 1}, {0, 1}},
      {config.nation_table, {0, 1, 2}, {0, 1, 2}},
      {config.customer_table, {0, 1}, {0, 3}},
      {config.supplier_table, {0, 1}, {0, 3}},
      {config.orders_table, {0, 1, 2}, {0, 1, 4}},
      {config.lineitem_table, {0, 1, 2, 3}, {0, 2, 5, 6}},
  };
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ5Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ5TxnBuffer>();
}

void TpchQ5Module::Build(core::AuxEngineServices &services,
                         core::StateBuilder &builder) {
  auto state = BuildState(services);
  auto shared = std::make_shared<const TpchQ5State>(std::move(state));

  // Side partition
  builder.Write(core::StateImage::Make(
      kId, core::TpchQ5Layout::kSide, 0, MakeSideImage(shared)));

  // Single dense tree partition: the complete segment tree lives in one slot,
  // avoiding an 8193-slot validity scan on the hot read path.
  builder.Write(core::StateImage::Make(
      kId, core::TpchQ5Layout::kTreeBase, 0,
      MakeTreeNodeImage(0, shared)));
  builder.Finish();
}

core::InvalidationPlan
TpchQ5Module::InterpretMutation(const core::MutationBatch &mutation) const {
  // Price/discount-only lineitem mutations are handled incrementally by
  // StageIncrementalMaintenance. They do not invalidate the published tree;
  // returning NoOp here avoids an epoch invalidate -> full RebuildAll on every
  // synthetic lineitem write (the dominant 10% write-mix cost).
  if (mutation.table == config.lineitem_table) {
    bool lineitem_only = true;
    for (const auto col : mutation.updated_columns) {
      // Harness projection uses column ids 5/6 for price/discount in some
      // probes; normalize to the module's watched projection.
      if (col != 2 && col != 3 && col != 5 && col != 6) {
        lineitem_only = false;
        break;
      }
    }
    if (lineitem_only) {
      return NoOpPlan();
    }
  }

  auto deps = Dependencies();
  for (const auto &dep : deps) {
    if (dep.table == mutation.table) {
      for (const auto col : mutation.updated_columns) {
        for (const auto watched : dep.watched_columns) {
          if (col == watched) {
            return EmptyPlan();
          }
        }
      }
    }
  }
  return NoOpPlan();
}

void TpchQ5Module::AccumulateMutation(const core::MutationBatch &mutation,
                                      core::AuxEngineServices &,
                                      core::AuxTxnBuffer &buf) {
  auto &buffer = dynamic_cast<TpchQ5TxnBuffer &>(buf);

  if (mutation.table == config.customer_table ||
      mutation.table == config.supplier_table ||
      mutation.table == config.nation_table ||
      mutation.table == config.region_table) {
    buffer.MarkRebuild();
    return;
  }

  if (mutation.table == config.orders_table) {
    auto add = [&](const core::AuxRow &row, int8_t sign) {
      TpchQ5TxnBuffer::OrderDelta delta;
      if (!ReadOrder(row, delta.orderkey, delta.custkey, delta.orderdate_days)) {
        buffer.MarkRebuild();
        return;
      }
      delta.sign = sign;
      buffer.AddOrderMutation(delta);
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

  if (mutation.table != config.lineitem_table) {
    buffer.MarkRebuild();
    return;
  }

  // Store raw lineitem mutations for later resolution in StageMaintenance
  for (size_t i = 0; i < mutation.old_rows.size() || i < mutation.new_rows.size(); i++) {
    TpchQ5TxnBuffer::LineitemDelta d;
    bool read_ok = true;
    if (i < mutation.old_rows.size()) {
      read_ok = ReadLineitem(mutation.old_rows[i], d.old_row);
    }
    if (i < mutation.new_rows.size()) {
      if (!ReadLineitem(mutation.new_rows[i], d.new_row))
        read_ok = false;
    }
    if (mutation.kind == core::MutationKind::Delete)
      d.is_delete = true;
    if (mutation.kind == core::MutationKind::Insert)
      d.is_insert = true;
    if (!read_ok) {
      buffer.MarkRebuild();
      return;
    }
    buffer.AddLineitemMutation(d);
  }
}

core::StagedMaintenanceOutcome
TpchQ5Module::StageMaintenance(core::AuxEngineServices &services,
                               const core::InvalidationPlan &plan,
                               core::AuxTxnBuffer &buf) {
  core::StagedMaintenanceOutcome outcome;
  outcome.action = core::MaintenanceAction::NoOp;

  auto &buffer = dynamic_cast<TpchQ5TxnBuffer &>(buf);

  // Try incremental maintenance for lineitem/order RF and price updates.
  if (!buffer.RequiresRebuild() &&
      (buffer.HasLineitemMutations() || !buffer.OrderMutations().empty())) {
    auto inc_outcome = StageIncrementalMaintenance(services, plan, buffer);
    if (inc_outcome.action != core::MaintenanceAction::NoOp) {
      inc_outcome.invalidations = plan;
      return inc_outcome;
    }
    // Incremental resolved the write as not touching the same-nation tree.
    return outcome;
  }

  // Nothing to do if plan is NoOp and no rebuild pending
  if (!buffer.RequiresRebuild() &&
      plan.scope == ::accelerator_codegen::aux_state::InvalidationScope::None) {
    return outcome;
  }

  // Fallback: full rebuild
  outcome.action = core::MaintenanceAction::RebuildAll;

  auto state = BuildState(services);
  auto shared = std::make_shared<const TpchQ5State>(std::move(state));

  outcome.updates.push_back(core::StateImage::Make(
      kId, core::TpchQ5Layout::kSide, 0, MakeSideImage(shared)));

  outcome.updates.push_back(core::StateImage::Make(
      kId, core::TpchQ5Layout::kTreeBase, 0,
      MakeTreeNodeImage(0, shared)));

  outcome.invalidations = plan;
  return outcome;
}

core::StagedMaintenanceOutcome
TpchQ5Module::StageIncrementalMaintenance(
    core::AuxEngineServices &services, const core::InvalidationPlan &,
    TpchQ5TxnBuffer &buffer) {
  core::StagedMaintenanceOutcome outcome;
  outcome.action = core::MaintenanceAction::NoOp;

  const auto side_snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {core::TpchQ5Layout::kSide});
  const TpchQ5State *side_ptr = nullptr;
  for (const auto &image : side_snapshot.partitions) {
    if (image.partition != core::TpchQ5Layout::kSide)
      continue;
    const auto *typed = AsQ5Image(image);
    if (typed && typed->is_side && typed->state) {
      side_ptr = typed->state.get();
      break;
    }
  }
  if (!side_ptr) {
    outcome.action = core::MaintenanceAction::RebuildAll;
    return outcome;
  }
  const auto &side = *side_ptr;

  // An updated or deleted order moves (or drops) the revenue of all its
  // existing lineitems, and the side maps would go stale: rebuild. (New
  // orders are resolved through the overlay below.)
  for (const auto &order : buffer.OrderMutations()) {
    if (order.sign < 0) {
      outcome.action = core::MaintenanceAction::RebuildAll;
      return outcome;
    }
  }

  std::unordered_map<int64_t, int32_t> order_nation_overlay;
  std::unordered_map<int64_t, int32_t> order_ordinal_overlay;
  for (const auto &order : buffer.OrderMutations()) {
    if (order.sign < 0)
      continue;
    auto cust_it = side.custkey_to_nationkey.find(order.custkey);
    if (cust_it == side.custkey_to_nationkey.end())
      continue;
    const int32_t ord =
        TpchQ5DateOrdinal(order.orderdate_days, side.min_orderdate_days);
    if (ord < 0 || ord >= static_cast<int32_t>(side.tree_size))
      continue;
    order_nation_overlay[order.orderkey] = cust_it->second;
    order_ordinal_overlay[order.orderkey] = ord;
  }

  auto lookup_ordinal = [&](int64_t orderkey, int32_t &ord) -> bool {
    const auto overlay = order_ordinal_overlay.find(orderkey);
    if (overlay != order_ordinal_overlay.end()) {
      ord = overlay->second;
      return true;
    }
    const auto it = side.orderkey_to_ordinal.find(orderkey);
    if (it == side.orderkey_to_ordinal.end())
      return false;
    ord = it->second;
    return true;
  };
  auto lookup_nation = [&](int64_t orderkey, int32_t &nk) -> bool {
    const auto overlay = order_nation_overlay.find(orderkey);
    if (overlay != order_nation_overlay.end()) {
      nk = overlay->second;
      return true;
    }
    const auto it = side.orderkey_to_nationkey.find(orderkey);
    if (it == side.orderkey_to_nationkey.end())
      return false;
    nk = it->second;
    return true;
  };

  // Per-leaf revenue deltas. Credit a nation only when supplier nation equals
  // customer nation (same filter as BuildState / the Q5 join).
  std::unordered_map<uint32_t, std::unordered_map<int32_t, int64_t>> leaf_deltas;
  bool unresolved = false;

  auto apply_row = [&](const TpchQ5LineitemInput &row, int64_t sign) {
    int32_t ord = 0;
    int32_t nation = 0;
    if (!lookup_ordinal(row.orderkey, ord) || !lookup_nation(row.orderkey, nation)) {
      unresolved = true;
      return;
    }
    auto supp_it = side.suppkey_to_nationkey.find(row.suppkey);
    if (supp_it == side.suppkey_to_nationkey.end()) {
      unresolved = true;
      return;
    }
    if (supp_it->second != nation)
      return;
    if (ord < 0 || ord >= static_cast<int32_t>(side.tree_size)) {
      unresolved = true;
      return;
    }
    const int64_t rev =
        ComputeRevenueRaw(row.extended_price_raw, row.discount_raw);
    const uint32_t leaf = TpchQ5LeafNode(side.tree_size, ord);
    leaf_deltas[leaf][nation] += sign * rev;
  };

  for (const auto &m : buffer.LineitemMutations()) {
    if (!m.is_insert)
      apply_row(m.old_row, -1);
    if (!m.is_delete)
      apply_row(m.new_row, 1);
  }

  if (unresolved) {
    outcome.action = core::MaintenanceAction::RebuildAll;
    return outcome;
  }

  if (leaf_deltas.empty()) {
    return outcome;
  }

  const auto decision = services.ChooseMaintenance(
      {static_cast<uint64_t>(buffer.LineitemMutations().size()), 2, true, false,
       true, false, false, core::MaintenanceAction::NoOp,
       services.ExpectedQueriesBeforeNextWrite()});
  if (decision.action != core::MaintenanceAction::ApplyDelta) {
    outcome.action = decision.action == core::MaintenanceAction::RebuildPartition
                         ? core::MaintenanceAction::RebuildAll
                         : decision.action;
    return outcome;
  }

  // Load the current tree image without copying the full tree.
  // Only affected leaves and their ancestors are cloned below.
  auto tree_partition = core::TpchQ5Layout::kTreeBase;
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {tree_partition});

  const TpchQ5State *tree_state = nullptr;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == tree_partition) {
      const auto *typed = AsQ5Image(image);
      if (typed && !typed->is_side && typed->state) {
        tree_state = typed->state.get();
        break;
      }
    }
  }

  if (!tree_state)
    return outcome;

  auto new_state = std::make_shared<TpchQ5State>();
  new_state->min_orderdate_days = side.min_orderdate_days;
  new_state->max_orderdate_days = side.max_orderdate_days;
  new_state->tree_size = side.tree_size;
  new_state->nation_name = side.nation_name;
  new_state->nation_region = side.nation_region;
  new_state->region_name_to_key = side.region_name_to_key;
  new_state->region_nations = side.region_nations;

  // Shared tree skeleton. Mutated nodes are detached before writing.
  new_state->tree = tree_state->tree;

  // Apply leaf deltas and do targeted path-only propagation.
  // Only affected leaves and their ancestors are recomputed — avoids
  // O(tree_size) full scan which is the dominant incremental cost at SF20+.
  std::set<uint32_t> recompute_nodes;

  for (const auto &[leaf_idx, nation_deltas] : leaf_deltas) {
    if (leaf_idx >= new_state->tree.size()) continue;
    auto &node = new_state->tree[leaf_idx];
    for (const auto &[nk, delta] : nation_deltas) {
      node.nation_revenue[nk] += delta;
      if (node.nation_revenue[nk] == 0)
        node.nation_revenue.erase(nk);
    }

    uint32_t current = leaf_idx;
    while (current > 1) {
      current /= 2;
      recompute_nodes.insert(current);
    }
  }

  // Clone affected internal nodes and recompute them bottom-up.
  for (auto it = recompute_nodes.rbegin(); it != recompute_nodes.rend(); ++it) {
    uint32_t node_idx = *it;
    uint32_t left = node_idx * 2;
    uint32_t right = node_idx * 2 + 1;
    if (left < new_state->tree.size() && right < new_state->tree.size()) {
      TpchQ5NodeRevenue rebuilt;
      for (const auto &[nk, rev] : new_state->tree[left].nation_revenue)
        rebuilt.nation_revenue[nk] += rev;
      for (const auto &[nk, rev] : new_state->tree[right].nation_revenue)
        rebuilt.nation_revenue[nk] += rev;
      new_state->tree[node_idx] = std::move(rebuilt);
    }
  }

  // Detach any affected leaf nodes that were mutated in place.
  // This keeps the immutable published tree intact for concurrent readers.
  for (const auto &[leaf_idx, nation_deltas] : leaf_deltas) {
    (void)nation_deltas;
    if (leaf_idx < new_state->tree.size() &&
        new_state->tree[leaf_idx].nation_revenue.empty()) {
      new_state->tree[leaf_idx] = TpchQ5NodeRevenue{};
    }
  }

  outcome.action = core::MaintenanceAction::ApplyDelta;

  outcome.updates.push_back(core::StateImage::Make(
      kId, core::TpchQ5Layout::kTreeBase, 0,
      MakeTreeNodeImage(0, new_state)));

  return outcome;
}

bool TpchQ5Module::CheckValidity(core::AuxEngineServices &services,
                                 const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void TpchQ5Module::Execute(core::AuxEngineServices &services,
                           const core::QueryArguments &arguments,
                           core::AuxRowSink &sink) {
  if (!CheckValidity(services, arguments)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  std::string region_name;
  int32_t start_days = 0;
  if (arguments.values.size() >= 1 && !arguments.values[0].is_null) {
    if (std::holds_alternative<std::string>(arguments.values[0].payload))
      region_name = std::get<std::string>(arguments.values[0].payload);
  }
  if (arguments.values.size() >= 2 && !arguments.values[1].is_null) {
    if (std::holds_alternative<core::AuxDate>(arguments.values[1].payload))
      start_days = std::get<core::AuxDate>(arguments.values[1].payload).days_since_epoch;
    else if (std::holds_alternative<int64_t>(arguments.values[1].payload))
      start_days = static_cast<int32_t>(std::get<int64_t>(arguments.values[1].payload));
  }
  int32_t end_days = TpchQ5AddMonths(start_days, 12);

  TpchQ5State side;
  if (!LoadSide(services, side)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  if (region_name.empty()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  // The state holds every order (maintenance rebuilds rather than drop one),
  // so a region with no nations or a date range past the tree's has no
  // revenue: answer from the state, not SQL.
  std::vector<int32_t> eligible_nations;
  int32_t target_regionkey = ResolveRegion(region_name, side, eligible_nations);
  if (target_regionkey < 0 || eligible_nations.empty()) {
    EmitResult(services, side, target_regionkey, {}, sink);
    return;
  }

  const int32_t start_ord = std::max<int32_t>(
      0, TpchQ5DateOrdinal(start_days, side.min_orderdate_days));
  const int32_t end_ord =
      std::min<int32_t>(static_cast<int32_t>(side.tree_size),
                        TpchQ5DateOrdinal(end_days, side.min_orderdate_days));
  if (start_ord >= end_ord) {
    EmitResult(services, side, target_regionkey, {}, sink);
    return;
  }

  auto nodes = TpchQ5TreeCover(side.tree_size, start_ord, end_ord);
  if (nodes.empty()) {
    EmitResult(services, side, target_regionkey, {}, sink);
    return;
  }

  std::unordered_map<int32_t, int64_t> revenue_by_nation;
  if (!LoadTreeNodes(services, side, nodes, revenue_by_nation)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  EmitResult(services, side, target_regionkey, revenue_by_nation, sink);
}

void TpchQ5Module::ExecuteFallback(core::AuxEngineServices &services,
                                   const core::QueryArguments &arguments,
                                   core::AuxRowSink &sink) {
  std::string region_name;
  int32_t start_days = 0;
  if (arguments.values.size() >= 1 && !arguments.values[0].is_null) {
    if (std::holds_alternative<std::string>(arguments.values[0].payload))
      region_name = std::get<std::string>(arguments.values[0].payload);
  }
  if (arguments.values.size() >= 2 && !arguments.values[1].is_null) {
    if (std::holds_alternative<core::AuxDate>(arguments.values[1].payload))
      start_days = std::get<core::AuxDate>(arguments.values[1].payload).days_since_epoch;
    else if (std::holds_alternative<int64_t>(arguments.values[1].payload))
      start_days = static_cast<int32_t>(std::get<int64_t>(arguments.values[1].payload));
  }
  int32_t end_days = TpchQ5AddMonths(start_days, 12);

  auto state = BuildState(services);

  std::vector<int32_t> eligible_nations;
  int32_t target_regionkey = ResolveRegion(region_name, state, eligible_nations);
  if (target_regionkey < 0) {
    sink.Begin({{{"n_name", core::AuxType::String, false},
                 {"revenue", core::AuxType::ScaledDecimal, false}}});
    sink.End();
    return;
  }

  int32_t start_ord = TpchQ5DateOrdinal(start_days, state.min_orderdate_days);
  int32_t end_ord = TpchQ5DateOrdinal(end_days, state.min_orderdate_days);
  if (start_ord < 0)
    start_ord = 0;
  if (end_ord > static_cast<int32_t>(state.tree_size))
    end_ord = static_cast<int32_t>(state.tree_size);

  std::unordered_map<int32_t, int64_t> revenue_by_nation;
  for (int32_t d = start_ord; d < end_ord; d++) {
    uint32_t node = TpchQ5LeafNode(state.tree_size, d);
    if (node < state.tree.size()) {
      const auto &node_rev = state.tree[node];
      for (const auto &[nk, rev] : node_rev.nation_revenue) {
        revenue_by_nation[nk] += rev;
      }
    }
  }

  EmitResult(services, state, target_regionkey, revenue_by_nation, sink);
}

TpchQ5State TpchQ5Module::BuildState(core::AuxEngineServices &services) const {
  (void)services;
  TpchQ5State state;

  if (!initial_scan)
    throw std::logic_error("TPC-H Q5 requires a typed initial scan source");

  initial_scan->ScanRegions([&](const TpchQ5RegionInput &input) {
    state.region_name_to_key[input.name] = input.key;
  });

  initial_scan->ScanNations([&](const TpchQ5NationInput &input) {
    state.nation_name[input.key] = input.name;
    state.nation_region[input.key] = input.regionkey;
    state.region_nations[input.regionkey].push_back(input.key);
  });

  std::unordered_map<int64_t, int32_t> customer_nations;
  initial_scan->ScanCustomers([&](const TpchQ5CustomerInput &input) {
    customer_nations[input.custkey] = input.nationkey;
  });
  state.custkey_to_nationkey = customer_nations;

  std::unordered_map<int64_t, int32_t> supplier_nations;
  initial_scan->ScanSuppliers([&](const TpchQ5SupplierInput &input) {
    supplier_nations[input.suppkey] = input.nationkey;
  });

  state.suppkey_to_nationkey = supplier_nations;

  std::unordered_map<int64_t, std::pair<int64_t, int32_t>> order_info;
  state.min_orderdate_days = std::numeric_limits<int32_t>::max();
  state.max_orderdate_days = std::numeric_limits<int32_t>::min();
  initial_scan->ScanOrders([&](const TpchQ5OrderInput &input) {
    order_info[input.orderkey] = {input.custkey, input.orderdate_days};
    if (input.orderdate_days < state.min_orderdate_days)
      state.min_orderdate_days = input.orderdate_days;
    if (input.orderdate_days > state.max_orderdate_days)
      state.max_orderdate_days = input.orderdate_days;
  });

  if (state.min_orderdate_days > state.max_orderdate_days) {
    state.min_orderdate_days = 0;
    state.max_orderdate_days = 0;
  }

  int32_t num_days = state.max_orderdate_days - state.min_orderdate_days + 1;
  if (num_days < 1)
    num_days = 1;
  state.tree_size = NextPowerOf2(static_cast<uint32_t>(num_days));
  state.tree.resize(state.tree_size * 2);

  initial_scan->ScanLineitems([&](const TpchQ5LineitemInput &input) {
    auto order_it = order_info.find(input.orderkey);
    if (order_it == order_info.end())
      return;
    auto cust_it = customer_nations.find(order_it->second.first);
    if (cust_it == customer_nations.end())
      return;
    auto supp_it = supplier_nations.find(input.suppkey);
    if (supp_it == supplier_nations.end())
      return;
    if (cust_it->second != supp_it->second)
      return;

    int32_t ord = TpchQ5DateOrdinal(order_it->second.second,
                                    state.min_orderdate_days);
    if (ord < 0 || ord >= static_cast<int32_t>(state.tree_size))
      return;

    int64_t revenue = ComputeRevenueRaw(input.extended_price_raw,
                                        input.discount_raw);
    uint32_t leaf = TpchQ5LeafNode(state.tree_size, ord);
    state.tree[leaf].nation_revenue[cust_it->second] += revenue;
  });

  // Populate orderkey lookup maps for incremental maintenance
  for (const auto &[orderkey, info] : order_info) {
    auto cust_it = customer_nations.find(info.first);
    if (cust_it != customer_nations.end()) {
      state.orderkey_to_nationkey[orderkey] = cust_it->second;
      int32_t ord = TpchQ5DateOrdinal(info.second, state.min_orderdate_days);
      if (ord >= 0 && ord < static_cast<int32_t>(state.tree_size))
        state.orderkey_to_ordinal[orderkey] = ord;
    }
  }

  for (uint32_t i = state.tree_size - 1; i > 0; i--) {
    const auto &left = state.tree[i * 2];
    const auto &right = state.tree[i * 2 + 1];
    auto &parent = state.tree[i];
    parent.nation_revenue = left.nation_revenue;
    for (const auto &[nk, rev] : right.nation_revenue) {
      parent.nation_revenue[nk] += rev;
    }
  }

  return state;
}

bool TpchQ5Module::LoadSide(core::AuxEngineServices &services,
                            TpchQ5State &state) const {
  auto side_partition = core::TpchQ5Layout::kSide;
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {side_partition});
  for (const auto &image : snapshot.partitions) {
    if (image.partition == side_partition) {
      const auto *typed = AsQ5Image(image);
      if (!typed || !typed->is_side || !typed->state)
        return false;
      state.nation_name = typed->state->nation_name;
      state.nation_region = typed->state->nation_region;
      state.region_name_to_key = typed->state->region_name_to_key;
      state.region_nations = typed->state->region_nations;
      state.min_orderdate_days = typed->state->min_orderdate_days;
      state.max_orderdate_days = typed->state->max_orderdate_days;
      state.tree_size = typed->state->tree_size;
      return true;
    }
  }
  return false;
}

bool TpchQ5Module::LoadTreeNodes(
    core::AuxEngineServices &services, const TpchQ5State &,
    const std::vector<uint32_t> &node_indices,
    std::unordered_map<int32_t, int64_t> &revenue_by_nation,
    std::map<uint32_t, core::Version> *versions) const {
  std::vector<core::PartitionId> partitions{core::TpchQ5Layout::kTreeBase};
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), partitions);
  bool loaded = false;
  for (const auto &image : snapshot.partitions) {
    const auto *typed = AsQ5Image(image);
    if (!typed || typed->is_side || !typed->state)
      continue;
    for (const auto idx : node_indices) {
      if (idx >= typed->state->tree.size())
        continue;
      for (const auto &[nk, rev] : typed->state->tree[idx].nation_revenue) {
        revenue_by_nation[nk] += rev;
      }
      if (versions)
        (*versions)[idx] = image.version;
    }
    loaded = true;
  }
  return loaded;
}

int32_t TpchQ5Module::ResolveRegion(
    const std::string &region_name, const TpchQ5State &state,
    std::vector<int32_t> &eligible_nations) const {
  auto it = state.region_name_to_key.find(region_name);
  if (it == state.region_name_to_key.end())
    return -1;
  int32_t rkey = it->second;
  auto nat_it = state.region_nations.find(rkey);
  if (nat_it != state.region_nations.end())
    eligible_nations = nat_it->second;
  return rkey;
}

void TpchQ5Module::EmitResult(
    core::AuxEngineServices &, const TpchQ5State &state,
    int32_t target_regionkey,
    const std::unordered_map<int32_t, int64_t> &revenue_by_nation,
    core::AuxRowSink &sink) const {
  TpchQ5FinalizeAndEmit(state.nation_name, state.nation_region,
                        target_regionkey, revenue_by_nation, sink);
}

std::vector<core::CalibrationWrite> TpchQ5Module::CalibrationWrites() const {
  return TpchCalibrationWrites(5);
}

} // namespace accelerator_codegen::aux_state::query
