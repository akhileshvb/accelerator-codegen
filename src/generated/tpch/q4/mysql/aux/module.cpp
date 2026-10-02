#include "tpch/q4/mysql/aux/module.hpp"
#include "tpch/q4/calibration_writes.hpp"
#include "accelerator_codegen/stateful/core/byte_stream.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <variant>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <stdexcept>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {

constexpr int32_t kEpoch = 719468;

struct CustomTimer {
  using Clock = std::chrono::high_resolution_clock;
  Clock::time_point start;
  bool active;
  explicit CustomTimer(bool enabled) : start(Clock::now()), active(enabled) {}
  double us() const {
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - start)
            .count()) /
           1000.0;
  }
};

static bool CustomProfilingEnabled() {
  static const bool enabled = []() {
    const char *v = std::getenv("ACCEL_AUX_CUSTOM_PROFILE");
    return v && v[0] != '\0';
  }();
  return enabled;
}

static void EmitCustomProfile(const char *phase, double us_val, int64_t rows,
                              int64_t keys, int64_t nodes) {
  if (!CustomProfilingEnabled())
    return;
  fprintf(stderr,
          "ACCEL_CUSTOM_PROFILE {"
          "\"phase\":\"%s\","
          "\"us\":%.3f,"
          "\"rows\":%ld,"
          "\"keys\":%ld,"
          "\"nodes\":%ld"
          "}\n",
          phase, us_val, (long)rows, (long)keys, (long)nodes);
}

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

std::shared_ptr<const TpchQ4StateImage>
MakeSideImage(const std::shared_ptr<const TpchQ4State> &state) {
  auto image = std::make_shared<TpchQ4StateImage>();
  image->kind = TpchQ4ImageKind::Side;
  image->state = state;
  return image;
}

std::shared_ptr<const TpchQ4TreeImage>
MakeTreeImage(const std::shared_ptr<const TpchQ4TreePayload> &payload) {
  auto image = std::make_shared<TpchQ4TreeImage>();
  image->kind = TpchQ4ImageKind::Tree;
  image->payload = payload;
  return image;
}

std::shared_ptr<const TpchQ4EligibilityImage>
MakeEligibilityImage(const std::shared_ptr<const TpchQ4EligibilityPayload> &payload) {
  auto image = std::make_shared<TpchQ4EligibilityImage>();
  image->kind = TpchQ4ImageKind::Eligibility;
  image->payload = payload;
  return image;
}

const TpchQ4StateImage *AsQ4Image(const core::StateImage &image) {
  return image.As<TpchQ4StateImage>();
}

const TpchQ4TreeImage *AsQ4TreeImage(const core::StateImage &image) {
  return image.As<TpchQ4TreeImage>();
}

const TpchQ4EligibilityImage *AsQ4EligibilityImage(const core::StateImage &image) {
  return image.As<TpchQ4EligibilityImage>();
}

uint32_t LeafNode(uint32_t tree_size, int32_t ord) {
  return tree_size + static_cast<uint32_t>(ord);
}

bool ReadLineitem(const core::AuxRow &row, TpchQ4LineitemInput &out) {
  int64_t orderkey = 0;
  if (!IntCell(row, 0, orderkey) ||
      !DateCell(row, 1, out.commitdate_days) ||
      !DateCell(row, 2, out.receiptdate_days))
    return false;
  out.orderkey = orderkey;
  return true;
}

bool LineitemQualifies(int32_t commitdate_days, int32_t receiptdate_days) {
  return commitdate_days < receiptdate_days;
}

bool ReadOrder(const core::AuxRow &row, int64_t &orderkey,
               int32_t &orderdate_days, std::string &orderpriority) {
  return IntCell(row, 0, orderkey) && DateCell(row, 1, orderdate_days) &&
         StringCell(row, 2, orderpriority);
}

}  // namespace

int32_t TpchQ4DaysFromYmd(int32_t year, unsigned month, unsigned day) {
  return DaysFromCivil(year, month, day);
}

int32_t TpchQ4AddMonths(int32_t days_since_epoch, int32_t months) {
  int32_t year = 0;
  unsigned month = 0, day = 0;
  CivilFromDays(days_since_epoch, year, month, day);
  const int total = year * 12 + static_cast<int>(month) - 1 + months;
  const int new_year = total / 12;
  const unsigned new_month = static_cast<unsigned>(total % 12 + 1);
  return DaysFromCivil(new_year, new_month, day);
}

int32_t TpchQ4PriorityIndex(const std::string &priority,
                            std::vector<std::string> &names,
                            std::unordered_map<std::string, int32_t> &pmap) {
  auto it = pmap.find(priority);
  if (it != pmap.end())
    return it->second;
  int32_t idx = static_cast<int32_t>(names.size());
  names.push_back(priority);
  pmap[priority] = idx;
  return idx;
}

bool TpchQ4TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ4TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  deltas.clear();
  order_deltas.clear();
}
bool TpchQ4TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
const std::vector<TpchQ4LineitemDelta> &TpchQ4TxnBuffer::Deltas() const {
  return deltas;
}
const std::vector<TpchQ4OrderDelta> &TpchQ4TxnBuffer::OrderDeltas() const {
  return order_deltas;
}
void TpchQ4TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  deltas.clear();
  order_deltas.clear();
}
void TpchQ4TxnBuffer::AddDelta(TpchQ4LineitemDelta delta) {
  pending = true;
  deltas.push_back(delta);
}
void TpchQ4TxnBuffer::AddOrderDelta(TpchQ4OrderDelta delta) {
  pending = true;
  order_deltas.push_back(delta);
}

core::AcceleratorId TpchQ4Module::Id() const { return kId; }
std::vector<core::TableDependency> TpchQ4Module::Dependencies() const {
  return {{config.orders_table, {0, 1, 2}, {0, 4, 5}},
          {config.lineitem_table, {0, 1, 2}, {0, 11, 12}}};
}
std::unique_ptr<core::AuxTxnBuffer> TpchQ4Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ4TxnBuffer>();
}

void TpchQ4Module::Build(core::AuxEngineServices &services,
                         core::StateBuilder &builder) {
  const auto state = BuildState(services);
  auto side_state = std::make_shared<TpchQ4State>(state);
  side_state->order_eligibility.clear();
  side_state->tree.clear();
  builder.Write(core::StateImage::Make(
      TpchQ4Layout::kAccelerator, TpchQ4Layout::kSide, 0,
      MakeSideImage(side_state)));
  auto eligibility_payload = std::make_shared<TpchQ4EligibilityPayload>();
  eligibility_payload->order_eligibility = state.order_eligibility;
  builder.Write(core::StateImage::Make(
      TpchQ4Layout::kAccelerator, TpchQ4Layout::kEligibility, 0,
      MakeEligibilityImage(eligibility_payload)));
  auto tree_payload = std::make_shared<TpchQ4TreePayload>();
  tree_payload->tree_size = state.tree_size;
  tree_payload->tree = state.tree;
  builder.Write(core::StateImage::Make(
      TpchQ4Layout::kAccelerator, TpchQ4Layout::kTreeBase, 0,
      MakeTreeImage(tree_payload)));
  builder.Finish();
}

core::InvalidationPlan
TpchQ4Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.lineitem_table ||
      mutation.table == config.orders_table) {
    core::InvalidationPlan plan;
    plan.scope =
        ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
    return plan;
  }
  return {};
}

void TpchQ4Module::AccumulateMutation(const core::MutationBatch &mutation,
                                      core::AuxEngineServices &,
                                      core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ4TxnBuffer &>(base_buffer);
  if (mutation.table == config.orders_table) {
    auto add = [&](const core::AuxRow &row, int8_t sign) {
      int64_t orderkey = 0;
      int32_t orderdate_days = 0;
      std::string priority;
      if (!ReadOrder(row, orderkey, orderdate_days, priority)) {
        buffer.MarkRebuild();
        return;
      }
      buffer.AddOrderDelta({orderkey, orderdate_days, priority, sign});
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
    TpchQ4LineitemInput input;
    if (!ReadLineitem(row, input)) {
      buffer.MarkRebuild();
      return;
    }
    if (!LineitemQualifies(input.commitdate_days, input.receiptdate_days))
      return;
    buffer.AddDelta({input.orderkey, sign});
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
TpchQ4Module::StageMaintenance(core::AuxEngineServices &services,
                               const core::InvalidationPlan &,
                               core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ4TxnBuffer &>(base_buffer);
  core::StagedMaintenanceOutcome outcome;

  if (buffer.RequiresRebuild()) {
    const auto decision = services.ChooseMaintenance(
        {0, 0, false, false, true, true, false,
         core::MaintenanceAction::NoOp,
         services.ExpectedQueriesBeforeNextWrite()});
    outcome.action = decision.action == core::MaintenanceAction::ApplyDelta
                         ? core::MaintenanceAction::RebuildAll
                         : decision.action;
    outcome.invalidations = EmptyPlan();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  if (buffer.Deltas().empty() && buffer.OrderDeltas().empty())
    return outcome;

  const auto total_deltas =
      buffer.Deltas().size() + buffer.OrderDeltas().size();
  const auto decision = services.ChooseMaintenance(
      {static_cast<uint64_t>(total_deltas), 1, true, false, true, false, false,
       core::MaintenanceAction::NoOp,
       services.ExpectedQueriesBeforeNextWrite()});
  if (decision.action == core::MaintenanceAction::RebuildPartition) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }
  if (decision.action != core::MaintenanceAction::ApplyDelta) {
    outcome.action = decision.action;
    outcome.invalidations = EmptyPlan();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  const auto visibility = services.CurrentVisibility();

  // Load side state (eligibility counts and priority dictionary)
  TpchQ4State side;
  if (!LoadSide(services, side)) {
    outcome.action = core::MaintenanceAction::RebuildAll;
    outcome.invalidations = EmptyPlan();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  // Load current tree image
  const auto tree_snapshot = services.StateStore().ReadPartitions(
      visibility, {TpchQ4Layout::kTreeBase});
  TpchQ4State tree_state;
  tree_state.tree_size = side.tree_size;
  tree_state.tree.resize(side.tree_size * 2);
  bool have_tree = false;
  for (const auto &image : tree_snapshot.partitions) {
    if (image.partition == TpchQ4Layout::kTreeBase) {
      const auto *typed = AsQ4TreeImage(image);
      if (!typed || typed->kind != TpchQ4ImageKind::Tree || !typed->payload) {
        outcome.action = core::MaintenanceAction::RebuildAll;
        outcome.invalidations = EmptyPlan();
        outcome.pending_rebuild = outcome.invalidations;
        return outcome;
      }
      tree_state.tree = typed->payload->tree;
      have_tree = true;
      break;
    }
  }
  if (!have_tree) {
    outcome.action = core::MaintenanceAction::RebuildAll;
    outcome.invalidations = EmptyPlan();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  const int32_t num_p = static_cast<int32_t>(side.priority_names.size());

  auto ensure_vectors = [&](TpchQ4State &state) {
    for (auto &node : state.tree) {
      if (static_cast<int32_t>(node.counts.size()) < num_p)
        node.counts.resize(num_p, 0);
    }
  };
  ensure_vectors(side);
  ensure_vectors(tree_state);

  // Per order key: remove the order's old contribution (if it counted), then
  // replay its order-row changes in sequence (delete, then insert for an
  // update) keeping its count of qualifying lineitems, add the lineitem
  // delta, and add the new contribution (if it counts now).
  std::map<int64_t, int64_t> eligibility_delta;
  for (const auto &delta : buffer.Deltas())
    eligibility_delta[delta.orderkey] += delta.sign;
  std::map<int64_t, std::vector<const TpchQ4OrderDelta *>> order_changes;
  for (const auto &delta : buffer.OrderDeltas())
    order_changes[delta.orderkey].push_back(&delta);
  std::set<int64_t> touched_orders;
  for (const auto &entry : eligibility_delta)
    touched_orders.insert(entry.first);
  for (const auto &entry : order_changes)
    touched_orders.insert(entry.first);

  // Tree adjustments grouped by leaf (a leaf may be touched via multiple
  // orders or an order that changes date/priority as well as lineitems).
  std::map<uint32_t, std::vector<int64_t>> leaf_adjustments;
  std::set<uint32_t> touched_leaf_ordinals;
  auto adjust = [&](const TpchQ4OrderInfo &info, int64_t by) {
    const int32_t ord = info.orderdate_days - side.min_orderdate_days;
    if (ord < 0 || ord >= static_cast<int32_t>(side.tree_size))
      return;
    const uint32_t leaf = LeafNode(side.tree_size, ord);
    touched_leaf_ordinals.insert(leaf);
    auto &adj = leaf_adjustments[leaf];
    if (adj.size() < side.priority_names.size())
      adj.resize(side.priority_names.size(), 0);
    if (info.priority_idx >= 0 && info.priority_idx < static_cast<int32_t>(adj.size()))
      adj[info.priority_idx] += by;
  };

  for (const auto orderkey : touched_orders) {
    auto it = side.order_eligibility.find(orderkey);
    bool exists = it != side.order_eligibility.end();
    TpchQ4OrderInfo info;
    if (exists) {
      info = it->second;
      if (info.qualifying_count > 0)
        adjust(info, -1);
    }
    auto changes = order_changes.find(orderkey);
    if (changes != order_changes.end()) {
      for (const auto *delta : changes->second) {
        if (delta->sign < 0) {
          exists = false;
        } else if (delta->sign > 0) {
          info.orderdate_days = delta->orderdate_days;
          info.priority_idx = TpchQ4PriorityIndex(delta->orderpriority, side.priority_names,
                                                  side.priority_map);
          exists = true;
        }
      }
    }
    auto lines = eligibility_delta.find(orderkey);
    if (lines != eligibility_delta.end())
      info.qualifying_count += lines->second;
    if (info.qualifying_count < 0)
      info.qualifying_count = 0;
    if (!exists) {
      if (it != side.order_eligibility.end())
        side.order_eligibility.erase(it);
      continue;
    }
    side.order_eligibility[orderkey] = info;
    if (info.qualifying_count > 0)
      adjust(info, +1);
  }

  // Apply leaf adjustments to tree
  for (const auto &[leaf, adj] : leaf_adjustments) {
    if (leaf >= side.tree_size * 2)
      continue;
    auto &node = tree_state.tree[leaf];
    if (node.counts.size() < adj.size())
      node.counts.resize(adj.size(), 0);
    for (size_t pi = 0; pi < adj.size(); ++pi)
      node.counts[pi] += adj[pi];
  }

  // Recompute ancestor paths from touched leaves
  std::set<uint32_t> ancestor_set;
  for (uint32_t leaf : touched_leaf_ordinals) {
    uint32_t node = leaf / 2;
    while (node > 0) {
      ancestor_set.insert(node);
      node /= 2;
    }
  }

  // Process deeper nodes first, then shallower.
  std::vector<uint32_t> ancestors(ancestor_set.begin(), ancestor_set.end());
  std::sort(ancestors.begin(), ancestors.end(),
            [](uint32_t a, uint32_t b) { return a > b; });

  for (uint32_t node_idx : ancestors) {
    if (node_idx >= tree_state.tree.size())
      continue;
    auto &node = tree_state.tree[node_idx];
    node.counts.assign(num_p, 0);
    uint32_t left = node_idx * 2;
    uint32_t right = node_idx * 2 + 1;
    if (left < tree_state.tree.size()) {
      const auto &l = tree_state.tree[left];
      for (int32_t pi = 0; pi < num_p; ++pi)
        if (pi < static_cast<int32_t>(l.counts.size()))
          node.counts[pi] += l.counts[pi];
    }
    if (right < tree_state.tree.size()) {
      const auto &r = tree_state.tree[right];
      for (int32_t pi = 0; pi < num_p; ++pi)
        if (pi < static_cast<int32_t>(r.counts.size()))
          node.counts[pi] += r.counts[pi];
    }
  }

  outcome.action = core::MaintenanceAction::ApplyDelta;
  {
    auto side_payload = std::make_shared<TpchQ4State>();
    side_payload->priority_names = std::move(side.priority_names);
    side_payload->priority_map = std::move(side.priority_map);
    side_payload->min_orderdate_days = side.min_orderdate_days;
    side_payload->max_orderdate_days = side.max_orderdate_days;
    side_payload->tree_size = side.tree_size;
    outcome.updates.push_back(core::StateImage::Make(
        TpchQ4Layout::kAccelerator, TpchQ4Layout::kSide, 0,
        MakeSideImage(side_payload)));
  }
  {
    auto ep = std::make_shared<TpchQ4EligibilityPayload>();
    ep->order_eligibility = std::move(side.order_eligibility);
    outcome.updates.push_back(core::StateImage::Make(
        TpchQ4Layout::kAccelerator, TpchQ4Layout::kEligibility, 0,
        MakeEligibilityImage(ep)));
  }
  {
    auto tp = std::make_shared<TpchQ4TreePayload>();
    tp->tree_size = tree_state.tree_size;
    tp->tree = std::move(tree_state.tree);
    outcome.updates.push_back(core::StateImage::Make(
        TpchQ4Layout::kAccelerator, TpchQ4Layout::kTreeBase, 0,
        MakeTreeImage(tp)));
  }
  return outcome;
}

bool TpchQ4Module::CheckValidity(core::AuxEngineServices &services,
                                 const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void TpchQ4Module::Execute(core::AuxEngineServices &services,
                           const core::QueryArguments &arguments,
                           core::AuxRowSink &sink) {
  CustomTimer execute_timer(CustomProfilingEnabled());
  if (arguments.values.size() < 1)
    return ExecuteFallback(services, arguments, sink);

  int32_t start = 0;
  if (!arguments.values[0].is_null) {
    if (std::holds_alternative<core::AuxDate>(arguments.values[0].payload))
      start = std::get<core::AuxDate>(arguments.values[0].payload).days_since_epoch;
    else if (std::holds_alternative<int64_t>(arguments.values[0].payload))
      start = static_cast<int32_t>(std::get<int64_t>(arguments.values[0].payload));
  }

  const auto end = TpchQ4AddMonths(start, 3);

  // Single ReadPartitions for both side metadata and tree -- eliminates
  // separate CheckValidity + LoadMeta + LoadTree StateStore calls.
  CustomTimer load_timer(CustomProfilingEnabled());
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {TpchQ4Layout::kSide, TpchQ4Layout::kTreeBase});

  const TpchQ4StateImage *side_image = nullptr;
  const TpchQ4TreeImage *tree_image = nullptr;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == TpchQ4Layout::kSide)
      side_image = AsQ4Image(image);
    else if (image.partition == TpchQ4Layout::kTreeBase)
      tree_image = AsQ4TreeImage(image);
  }

  if (!side_image || !side_image->state || !tree_image || !tree_image->payload) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  const auto &side = *side_image->state;
  const auto &tree_payload = *tree_image->payload;

  if (side.tree_size == 0) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  // Compute canonical segment-tree node cover for [start, end)
  int32_t left_ord = start - side.min_orderdate_days;
  int32_t right_ord = end - side.min_orderdate_days;
  if (left_ord < 0)
    left_ord = 0;
  if (right_ord > static_cast<int32_t>(side.tree_size))
    right_ord = static_cast<int32_t>(side.tree_size);
  if (right_ord <= left_ord) {
    // Empty range -- emit zero counts
    std::vector<int64_t> counts(side.priority_names.size(), 0);
    EmitResult(side, counts, sink);
    return;
  }

  std::vector<uint32_t> node_indices;
  uint32_t l = LeafNode(side.tree_size, left_ord);
  uint32_t r = LeafNode(side.tree_size, right_ord);
  while (l < r) {
    if (l & 1) {
      node_indices.push_back(l);
      l++;
    }
    if (r & 1) {
      r--;
      node_indices.push_back(r);
    }
    l /= 2;
    r /= 2;
  }

  std::vector<int64_t> counts(side.priority_names.size(), 0);
  for (auto idx : node_indices) {
    if (idx >= tree_payload.tree.size())
      continue;
    const auto &node = tree_payload.tree[idx];
    for (size_t pi = 0; pi < node.counts.size() && pi < counts.size(); ++pi)
      counts[pi] += node.counts[pi];
  }

  EmitCustomProfile("state_load", load_timer.us(), side.priority_names.size(),
                    node_indices.size(), node_indices.size());

  EmitResult(side, counts, sink);
}

void TpchQ4Module::ExecuteFallback(core::AuxEngineServices &services,
                                   const core::QueryArguments &arguments,
                                   core::AuxRowSink &sink) {
  int32_t start = 0;
  if (arguments.values.size() >= 1 && !arguments.values[0].is_null) {
    if (std::holds_alternative<core::AuxDate>(arguments.values[0].payload))
      start = std::get<core::AuxDate>(arguments.values[0].payload).days_since_epoch;
    else if (std::holds_alternative<int64_t>(arguments.values[0].payload))
      start = static_cast<int32_t>(std::get<int64_t>(arguments.values[0].payload));
  }

  const auto state = BuildState(services);
  const auto end = TpchQ4AddMonths(start, 3);

  // Compute from built state directly for the requested range
  int32_t left_ord = start - state.min_orderdate_days;
  int32_t right_ord = end - state.min_orderdate_days;
  if (left_ord < 0)
    left_ord = 0;
  if (right_ord > static_cast<int32_t>(state.tree_size))
    right_ord = static_cast<int32_t>(state.tree_size);

  std::vector<int64_t> counts(state.priority_names.size(), 0);

  if (right_ord > left_ord && state.tree_size > 0) {
    std::vector<uint32_t> node_indices;
    uint32_t l = LeafNode(state.tree_size, left_ord);
    uint32_t r = LeafNode(state.tree_size, right_ord);
    while (l < r) {
      if (l & 1) {
        node_indices.push_back(l);
        l++;
      }
      if (r & 1) {
        r--;
        node_indices.push_back(r);
      }
      l /= 2;
      r /= 2;
    }

    for (auto idx : node_indices) {
      if (idx < state.tree.size()) {
        const auto &node = state.tree[idx];
        for (size_t pi = 0; pi < node.counts.size() && pi < counts.size(); ++pi)
          counts[pi] += node.counts[pi];
      }
    }
  }

  EmitResult(state, counts, sink);
}

TpchQ4State TpchQ4Module::BuildState(core::AuxEngineServices &services) const {
  (void)services;
  TpchQ4State state;
  if (!initial_scan)
    throw std::logic_error("TPC-H Q4 requires a typed initial scan source");

  // First scan: collect orders and their priorities
  struct OrderAccum {
    int32_t orderdate_days = 0;
    int32_t priority_idx = 0;
  };
  std::unordered_map<int64_t, OrderAccum> orders;
  bool have_date_bounds = false;

  initial_scan->ScanOrders([&](const TpchQ4OrderInput &input) {
    int32_t pidx = TpchQ4PriorityIndex(input.orderpriority, state.priority_names,
                                        state.priority_map);
    orders[input.key] = {input.orderdate_days, pidx};
    if (!have_date_bounds) {
      state.min_orderdate_days = input.orderdate_days;
      state.max_orderdate_days = input.orderdate_days;
      have_date_bounds = true;
    } else {
      state.min_orderdate_days =
          std::min(state.min_orderdate_days, input.orderdate_days);
      state.max_orderdate_days =
          std::max(state.max_orderdate_days, input.orderdate_days);
    }
  });

  if (state.min_orderdate_days > state.max_orderdate_days) {
    state.min_orderdate_days = 0;
    state.max_orderdate_days = 0;
  }

  // Second scan: count qualifying lineitems per order
  initial_scan->ScanLineitems([&](const TpchQ4LineitemInput &input) {
    auto it = orders.find(input.orderkey);
    if (it == orders.end())
      return;
    if (LineitemQualifies(input.commitdate_days, input.receiptdate_days)) {
      TpchQ4OrderInfo info;
      info.orderdate_days = it->second.orderdate_days;
      info.priority_idx = it->second.priority_idx;
      info.qualifying_count = 1;
      auto res = state.order_eligibility.emplace(input.orderkey, info);
      if (!res.second)
        res.first->second.qualifying_count += 1;
    }
  });

  // Add orders with no qualifying lineitems (qualifying_count stays 0,
  // they won't contribute to tree counts)
  for (const auto &[orderkey, order_info] : orders) {
    auto it = state.order_eligibility.find(orderkey);
    if (it == state.order_eligibility.end()) {
      TpchQ4OrderInfo info;
      info.orderdate_days = order_info.orderdate_days;
      info.priority_idx = order_info.priority_idx;
      info.qualifying_count = 0;
      state.order_eligibility.emplace(orderkey, info);
    }
  }

  // Build segment tree
  int32_t num_days = state.max_orderdate_days - state.min_orderdate_days + 1;
  if (num_days < 1)
    num_days = 1;
  state.tree_size = NextPowerOf2(static_cast<uint32_t>(num_days));
  state.tree.resize(state.tree_size * 2);
  int32_t num_p = static_cast<int32_t>(state.priority_names.size());

  for (auto &node : state.tree)
    node.counts.assign(num_p, 0);

  // Populate leaves
  for (const auto &[orderkey, info] : state.order_eligibility) {
    if (info.qualifying_count <= 0)
      continue;
    int32_t ord = info.orderdate_days - state.min_orderdate_days;
    if (ord < 0 || ord >= static_cast<int32_t>(state.tree_size))
      continue;
    uint32_t leaf = LeafNode(state.tree_size, ord);
    auto &node = state.tree[leaf];
    if (info.priority_idx < static_cast<int32_t>(node.counts.size()))
      node.counts[info.priority_idx] += 1;
  }

  // Populate internal nodes
  for (uint32_t i = state.tree_size - 1; i > 0; i--) {
    const auto &left = state.tree[i * 2];
    const auto &right = state.tree[i * 2 + 1];
    auto &parent = state.tree[i];
    for (int32_t pi = 0; pi < num_p; ++pi) {
      if (pi < static_cast<int32_t>(left.counts.size()))
        parent.counts[pi] += left.counts[pi];
      if (pi < static_cast<int32_t>(right.counts.size()))
        parent.counts[pi] += right.counts[pi];
    }
  }

  return state;
}

bool TpchQ4Module::LoadSide(core::AuxEngineServices &services,
                            TpchQ4State &state) const {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(),
      {TpchQ4Layout::kSide, TpchQ4Layout::kEligibility});
  bool have_meta = false;
  bool have_eligibility = false;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == TpchQ4Layout::kSide) {
      const auto *typed = AsQ4Image(image);
      if (!typed || typed->kind != TpchQ4ImageKind::Side || !typed->state)
        return false;
      state.priority_names = typed->state->priority_names;
      state.priority_map = typed->state->priority_map;
      state.min_orderdate_days = typed->state->min_orderdate_days;
      state.max_orderdate_days = typed->state->max_orderdate_days;
      state.tree_size = typed->state->tree_size;
      have_meta = true;
    } else if (image.partition == TpchQ4Layout::kEligibility) {
      const auto *typed = AsQ4EligibilityImage(image);
      if (!typed || typed->kind != TpchQ4ImageKind::Eligibility || !typed->payload)
        return false;
      state.order_eligibility = typed->payload->order_eligibility;
      have_eligibility = true;
    }
  }
  return have_meta && have_eligibility;
}

bool TpchQ4Module::LoadMeta(core::AuxEngineServices &services,
                            TpchQ4State &state) const {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {TpchQ4Layout::kSide});
  for (const auto &image : snapshot.partitions) {
    if (image.partition == TpchQ4Layout::kSide) {
      const auto *typed = AsQ4Image(image);
      if (!typed || typed->kind != TpchQ4ImageKind::Side || !typed->state)
        return false;
      state.priority_names = typed->state->priority_names;
      state.min_orderdate_days = typed->state->min_orderdate_days;
      state.max_orderdate_days = typed->state->max_orderdate_days;
      state.tree_size = typed->state->tree_size;
      return true;
    }
  }
  return false;
}

bool TpchQ4Module::LoadTree(
    core::AuxEngineServices &services, const TpchQ4State &side,
    const std::vector<uint32_t> &node_indices,
    std::vector<int64_t> &counts) const {
  counts.assign(side.priority_names.size(), 0);

  if (node_indices.empty())
    return true;

  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {TpchQ4Layout::kTreeBase});
  for (const auto &image : snapshot.partitions) {
    if (image.partition == TpchQ4Layout::kTreeBase) {
      const auto *typed = AsQ4TreeImage(image);
      if (!typed || typed->kind != TpchQ4ImageKind::Tree || !typed->payload)
        return false;
      const auto &tree = typed->payload->tree;
      for (auto idx : node_indices) {
        if (idx >= tree.size())
          continue;
        const auto &node = tree[idx];
        for (size_t pi = 0;
             pi < node.counts.size() && pi < counts.size(); ++pi)
          counts[pi] += node.counts[pi];
      }
      return true;
    }
  }
  return false;
}

void TpchQ4Module::EmitResult(const TpchQ4State &state,
                              const std::vector<int64_t> &counts,
                              core::AuxRowSink &sink) const {
  FinalizeTpchQ4(counts, state.priority_names, sink);
}

std::vector<core::CalibrationWrite> TpchQ4Module::CalibrationWrites() const {
  return TpchQ4CalibrationWrites();
}

}  // namespace accelerator_codegen::aux_state::query
