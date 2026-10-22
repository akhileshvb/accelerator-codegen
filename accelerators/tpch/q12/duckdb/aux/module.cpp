#include "tpch/q12/duckdb/aux/module.hpp"
#include "tpch/q12/calibration_writes.hpp"
#include "prims/sql_date.hpp"
#include "substrate/aux/core/aux_cells.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/partitioned_build.hpp"
#include "substrate/aux/core/query_module_helpers.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>
#include "tpch/q12/layout.hpp"

namespace accelerator_codegen::aux_state::query {
namespace {

namespace sql_date = core::sql_date;

// Tree contributions below which a commit applies its levels inline (untimed).
constexpr std::size_t kLevelOpThreshold = 2048;

std::shared_ptr<const TpchQ12StateImage>
MakeStateImage(TpchQ12ImageKind kind, int32_t receipt_day,
               const std::shared_ptr<const TpchQ12State> &state) {
  auto image = std::make_shared<TpchQ12StateImage>();
  image->kind = kind;
  image->receipt_day = receipt_day;
  image->state = state;
  return image;
}

const TpchQ12StateImage *AsQ12Image(const core::StateImage &image,
                                    TpchQ12ImageKind kind) {
  const auto *payload = image.As<TpchQ12StateImage>();
  return payload && payload->kind == kind && payload->state ? payload : nullptr;
}


// Dyadic tree over receipt days, implicit heap numbering: node 1 is the root,
// leaves are kTreeN + (day - kTreeBase). Days outside [kTreeBase,
// kTreeBase + kTreeN) live in one overflow partition. All partition ids stay
// well below 2^31 and never collide with kOrderPriorities (0).
constexpr int kTreeBits = 18;
constexpr int64_t kTreeN = int64_t(1) << kTreeBits;
constexpr int32_t kTreeBase = -(1 << (kTreeBits - 1));
constexpr core::PartitionId kOverflowPartition =
    static_cast<core::PartitionId>(1) << (kTreeBits + 2);
static_assert(kOverflowPartition < (int64_t(1) << 31), "int32 partition keys");

bool InTree(int32_t day) {
  return day >= kTreeBase && static_cast<int64_t>(day) - kTreeBase < kTreeN;
}
int64_t LeafNode(int32_t day) {
  return kTreeN + (static_cast<int64_t>(day) - kTreeBase);
}

void MergeModes(TpchQ12Modes &dst, const TpchQ12Modes &src);

// Adds the ship-mode counts of `day` to the leaf-to-root path (or overflow)
// of a map of nodes (Build only; maintenance uses the flat level walk).
void AddToTree(std::map<core::PartitionId, TpchQ12Node> &nodes, int32_t day,
               const TpchQ12Modes &modes) {
  if (!InTree(day)) {
    MergeModes(nodes[kOverflowPartition].overflow_days[day], modes);
    return;
  }
  for (int64_t n = LeafNode(day); n >= 1; n >>= 1)
    MergeModes(nodes[static_cast<core::PartitionId>(n)].modes, modes);
}

// A fresh node image (the node is stored inline in the image).
std::shared_ptr<TpchQ12StateImage> NewNodeImage() {
  auto image = std::make_shared<TpchQ12StateImage>();
  image->kind = TpchQ12ImageKind::Node;
  return image;
}

const TpchQ12Node *AsNodeImage(const core::StateImage &image) {
  const auto *payload = image.As<TpchQ12StateImage>();
  return payload && payload->kind == TpchQ12ImageKind::Node ? &payload->node
                                                            : nullptr;
}

// dst += src (dense slots, plus extra names).
void MergeModes(TpchQ12Modes &dst, const TpchQ12Modes &src) {
  for (int i = 0; i < kQ12ModeSlots; ++i) {
    dst.high[i] += src.high[i];
    dst.low[i] += src.low[i];
  }
  for (const auto &entry : src.extra)
    dst.Add(-1, entry.shipmode, entry.high_line_count, entry.low_line_count);
}

bool ReadLineitem(const core::AuxRow &row, TpchQ12LineitemInput &out) {
  int64_t orderkey = 0;
  if (!core::IntCell(row, 0, orderkey) || !core::StringCell(row, 1, out.shipmode) ||
      !core::DateCell(row, 2, out.commitdate_days) ||
      !core::DateCell(row, 3, out.receiptdate_days) ||
      !core::DateCell(row, 4, out.shipdate_days))
    return false;
  out.orderkey = orderkey;
  return true;
}

// Matches ScanOrders: o_orderkey, o_orderpriority
bool ReadOrder(const core::AuxRow &row, int64_t &orderkey,
               std::string &orderpriority) {
  return core::IntCell(row, 0, orderkey) && core::StringCell(row, 1, orderpriority);
}


void AccumulateOrders(const core::MutationBatch &mutation, TpchQ12TxnBuffer &buffer) {
  auto add = [&](const core::AuxRow &row, int8_t sign) {
    int64_t orderkey = 0;
    std::string priority;
    if (!ReadOrder(row, orderkey, priority)) {
      buffer.MarkRebuild();
      return;
    }
    buffer.AddOrderDelta({orderkey, TpchQ12ClassifyPriority(priority), sign});
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

void AccumulateLineitems(const core::MutationBatch &mutation, TpchQ12TxnBuffer &buffer) {
  auto add = [&](const core::AuxRow &row, int8_t sign) {
    TpchQ12LineitemInput input;
    if (!ReadLineitem(row, input)) {
      buffer.MarkRebuild();
      return;
    }
    if (!TpchQ12LineitemQualifies(input.commitdate_days, input.receiptdate_days,
                                  input.shipdate_days))
      return;
    TpchQ12LineitemDelta delta;
    delta.orderkey = input.orderkey;
    delta.receipt_day = input.receiptdate_days;
    delta.shipmode = input.shipmode;
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

// The aligned tree nodes covering [start, end) (bottom-up segment tree), plus
// the overflow partition when the window leaves the tree domain.
void WindowPartitions(int32_t start, int32_t end,
                      std::vector<core::PartitionId> &partitions) {
  const int64_t lo = std::max<int64_t>(start, kTreeBase);
  const int64_t hi = std::min<int64_t>(end, int64_t(kTreeBase) + kTreeN);
  if (lo < hi) {
    int64_t l = lo - kTreeBase + kTreeN, r = hi - kTreeBase + kTreeN;
    while (l < r) {
      if (l & 1)
        partitions.push_back(static_cast<core::PartitionId>(l++));
      if (r & 1)
        partitions.push_back(static_cast<core::PartitionId>(--r));
      l >>= 1;
      r >>= 1;
    }
  }
  if (start < kTreeBase || end > int64_t(kTreeBase) + kTreeN)
    partitions.push_back(kOverflowPartition);
}

// A line's net contribution to the tree. Lines in the tree domain are keyed by
// their leaf node; the rest go to the overflow partition.
struct Contribution {
  int64_t leaf;
  int32_t day;
  int slot; // dense mode slot, -1: look up by name
  const std::string *mode;
  int64_t high;
  int64_t low;
};

// Delta maintenance of the order priorities and the dyadic tree for one
// commit. Run() is false when the delta cannot be applied exactly and the
// state must rebuild; otherwise `outcome` is complete.
class TreeMaintainer {
public:
  TreeMaintainer(core::AuxEngineServices &services, const TpchQ12TxnBuffer &buffer,
                 TpchQ12InitialScan *initial_scan,
                 core::StagedMaintenanceOutcome &outcome)
      : services(services), buffer(buffer), initial_scan(initial_scan),
        outcome(outcome), visibility(services.CurrentVisibility()) {}

  bool Run() {
    if (!LoadPriorities() || !ClassifyOrderChanges() || !LookupChangedLines() ||
        !CountLineitems())
      return false;
    for (const auto key : removed_orders)
      if (priority_state->line_counts.Get(key) != 0)
        return false;

    // The lineitem deltas move leaf-to-root paths of the dyadic tree, so a
    // partition rebuild is not offered: it would need a diff against the old
    // leaf to fix the ancestors.
    const auto request = core::MaintenanceRequest::IncrementalPartitionedNoPartitionRebuild(
        buffer.Deltas().size() + buffer.OrderDeltas().size() + changed_lines.size(),
        std::max<std::size_t>(1, CountTouchedDays() + (priorities_changed ? 1 : 0)),
        services.ExpectedQueriesBeforeNextWrite());
    const auto decision = services.ChooseMaintenance(request);
    if (decision.action != core::MaintenanceAction::ApplyDelta &&
        decision.action != core::MaintenanceAction::RebuildPartition) {
      core::FinishRebuildAll(outcome, decision);
      return true;
    }

    if ((!buffer.Deltas().empty() || !changed_lines.empty()) && !StageTree())
      return false;
    for (const auto key : removed_orders) {
      priority_state->SetPriority(key, TpchQ12PriorityClass::Absent);
      priorities_changed = true;
    }
    if (priorities_changed)
      PublishPriorities();
    return true;
  }

private:
  struct Changed {
    int64_t key;
    TpchQ12PriorityClass from;
    TpchQ12PriorityClass to;
  };

  struct ChangedLine {
    int64_t key;
    int32_t day;
    std::string mode;
  };

  // Working copy: shares all pages with the published image (cheap page-table
  // copy); only pages actually written are cloned. day_partitions is left
  // empty so the new priority image does not carry any day data.
  bool LoadPriorities() {
    const auto snapshot = services.StateStore().ReadPartitions(
        visibility, {core::TpchQ12Layout::kOrderPriorities});
    for (const auto &image : snapshot.partitions) {
      if (image.partition != core::TpchQ12Layout::kOrderPriorities)
        continue;
      const auto *typed = AsQ12Image(image, TpchQ12ImageKind::OrderPriorities);
      if (!typed)
        return false;
      priority_state = std::make_shared<TpchQ12State>();
      priority_state->order_priorities = typed->state->order_priorities;
      priority_state->line_counts = typed->state->line_counts;
      priority_version = image.version;
      break;
    }
    return priority_state != nullptr;
  }

  const Changed *FindChanged(int64_t key) const {
    if (changed_orders.empty())
      return nullptr;
    const auto it = std::lower_bound(
        changed_orders.begin(), changed_orders.end(), key,
        [](const Changed &c, int64_t k) { return c.key < k; });
    return it != changed_orders.end() && it->key == key ? &*it : nullptr;
  }

  // A key with exactly one removal and one insertion is an in-place update:
  // same class (or untracked columns only) is a no-op; a class change moves
  // that order's lineitems between classes (looked up in the base table
  // below). Anything more tangled is a rebuild. Deltas are grouped by key with
  // one stable sort (per-key order is preserved).
  bool ClassifyOrderChanges() {
    const auto &order_deltas = buffer.OrderDeltas();
    std::vector<uint32_t> by_key(order_deltas.size());
    for (uint32_t i = 0; i < by_key.size(); ++i)
      by_key[i] = i;
    std::stable_sort(by_key.begin(), by_key.end(), [&](uint32_t l, uint32_t r) {
      return order_deltas[l].orderkey < order_deltas[r].orderkey;
    });
    for (std::size_t i = 0; i < by_key.size();) {
      std::size_t j = i;
      const int64_t key = order_deltas[by_key[i]].orderkey;
      const TpchQ12OrderDelta *ins = nullptr, *rem = nullptr;
      std::size_t ins_n = 0, rem_n = 0;
      for (; j < by_key.size() && order_deltas[by_key[j]].orderkey == key; ++j) {
        const auto &d = order_deltas[by_key[j]];
        if (d.sign > 0) {
          ins = &d;
          ++ins_n;
        } else {
          rem = &d;
          ++rem_n;
        }
      }
      if (ins_n == 0) {
        removed_orders.push_back(key);
      } else if (rem_n == 0) {
        // Inserted order: the class of its last inserted row wins.
        priority_state->SetPriority(key, ins->priority);
        priorities_changed = true;
      } else {
        if (ins_n != 1 || rem_n != 1)
          return false;
        if (ins->priority != rem->priority) {
          if (priority_state->PriorityOf(key) != rem->priority)
            return false;
          changed_orders.push_back({key, rem->priority, ins->priority});
        }
      }
      i = j;
    }
    return true;
  }

  // Class-changing updates: the order's current qualifying lineitems S.
  // Published state holds (S - inserted + deleted by this txn) under the old
  // class; the final state needs S under the new class. The per-delta term
  // (+-1 under the old class) is added with the tree contributions.
  bool LookupChangedLines() {
    if (changed_orders.empty())
      return true;
    std::vector<int64_t> keys;
    for (const auto &entry : changed_orders)
      keys.push_back(entry.key);
    if (!initial_scan ||
        !initial_scan->LookupQualifyingLineitems(
            keys, [&](int64_t key, int32_t day, const std::string &mode) {
              changed_lines.push_back({key, day, mode});
            }))
      return false;
    std::map<int64_t, std::size_t> line_n;
    for (const auto &line : changed_lines)
      ++line_n[line.key];
    for (const auto &entry : changed_orders) {
      const auto n = line_n[entry.key];
      if (n > std::numeric_limits<uint16_t>::max())
        return false;
      priority_state->SetPriority(entry.key, entry.to);
      priority_state->line_counts.Set(entry.key, static_cast<uint16_t>(n));
    }
    priorities_changed = true;
    return true;
  }

  // Tracks per-order qualifying-lineitem counts so an order removal can be
  // applied as a delta when none of its lineitems remain counted.
  bool CountLineitems() {
    for (const auto &delta : buffer.Deltas()) {
      if (FindChanged(delta.orderkey))
        continue; // count set from the base-table lookup
      const auto priority = priority_state->PriorityOf(delta.orderkey);
      if (priority == TpchQ12PriorityClass::Absent)
        return false;
      if (priority == TpchQ12PriorityClass::Neither)
        continue;
      const int64_t count =
          static_cast<int64_t>(priority_state->line_counts.Get(delta.orderkey)) +
          delta.sign;
      if (count < 0 || count > std::numeric_limits<uint16_t>::max())
        return false;
      priority_state->line_counts.Set(delta.orderkey,
                                      static_cast<uint16_t>(count));
      priorities_changed = true;
    }
    return true;
  }

  std::size_t CountTouchedDays() const {
    std::vector<int32_t> days;
    days.reserve(buffer.Deltas().size());
    for (const auto &delta : buffer.Deltas())
      days.push_back(delta.receipt_day);
    std::sort(days.begin(), days.end());
    return static_cast<std::size_t>(std::unique(days.begin(), days.end()) -
                                    days.begin());
  }

  void AddContribution(std::vector<Contribution> &tree,
                       std::vector<Contribution> &overflow, int32_t day,
                       const std::string &mode, TpchQ12PriorityClass cls,
                       int64_t amount) const {
    if (cls != TpchQ12PriorityClass::High && cls != TpchQ12PriorityClass::Low)
      return;
    Contribution c{InTree(day) ? LeafNode(day) : 0, day, TpchQ12ModeSlot(mode),
                   &mode, cls == TpchQ12PriorityClass::High ? amount : 0,
                   cls == TpchQ12PriorityClass::Low ? amount : 0};
    (InTree(day) ? tree : overflow).push_back(c);
  }

  // Net contribution of every line to the tree; `tree` ends sorted by leaf.
  void CollectContributions(std::vector<Contribution> &tree,
                            std::vector<Contribution> &overflow) const {
    tree.reserve(buffer.Deltas().size() + changed_lines.size());
    for (const auto &line : changed_lines) {
      const auto *cls = FindChanged(line.key);
      AddContribution(tree, overflow, line.day, line.mode, cls->from, -1);
      AddContribution(tree, overflow, line.day, line.mode, cls->to, +1);
    }
    for (const auto &delta : buffer.Deltas()) {
      if (const auto *changed = FindChanged(delta.orderkey)) {
        AddContribution(tree, overflow, delta.receipt_day, delta.shipmode,
                        changed->from, delta.sign);
        continue;
      }
      AddContribution(tree, overflow, delta.receipt_day, delta.shipmode,
                      priority_state->PriorityOf(delta.orderkey), delta.sign);
    }
    std::sort(tree.begin(), tree.end(),
              [](const Contribution &l, const Contribution &r) {
                return l.leaf < r.leaf;
              });
  }

  bool StageTree() {
    std::vector<Contribution> tree, overflow;
    CollectContributions(tree, overflow);

    // Ancestors per level (leaf level 0 .. root level kTreeBits): because the
    // contributions are visited in leaf order, each level is a sorted run and
    // each contribution finds its node with a forward-only cursor.
    constexpr int kLevels = kTreeBits + 1;
    std::vector<core::PartitionId> partitions;
    partitions.reserve(tree.size() * 4 + kLevels + 1);
    uint32_t level_begin[kLevels];
    for (int level = 0; level < kLevels; ++level) {
      level_begin[level] = static_cast<uint32_t>(partitions.size());
      int64_t last = 0;
      for (const auto &c : tree) {
        const int64_t node = c.leaf >> level;
        if (node != last) {
          partitions.push_back(static_cast<core::PartitionId>(node));
          last = node;
        }
      }
    }
    const std::size_t tree_slots = partitions.size();
    if (!overflow.empty())
      partitions.push_back(kOverflowPartition);

    std::vector<std::shared_ptr<TpchQ12StateImage>> work(partitions.size());
    std::vector<core::Version> versions(partitions.size(), 0);
    if (!partitions.empty()) {
      const auto snapshot =
          services.StateStore().ReadPartitions(visibility, partitions);
      std::vector<const core::StateImage *> matched;
      core::MatchSnapshot(partitions, snapshot.partitions, matched);
      for (std::size_t slot = 0; slot < partitions.size(); ++slot) {
        auto image = NewNodeImage();
        if (matched[slot]) {
          const auto *node = AsNodeImage(*matched[slot]);
          if (!node)
            return false;
          image->node = *node;
          versions[slot] = matched[slot]->version;
        }
        work[slot] = std::move(image);
      }
    }
    // Levels own disjoint partitions, so each is applied by one slice.
    core::ParallelEachWhen(
        services, kLevels, tree.size(), kLevelOpThreshold, [&](std::size_t level) {
          uint32_t cursor = level_begin[level];
          for (const auto &c : tree) {
            if (c.leaf >> level != static_cast<int64_t>(partitions[cursor]))
              ++cursor;
            work[cursor]->node.modes.Add(c.slot, *c.mode, c.high, c.low);
          }
        });
    if (!overflow.empty()) {
      auto &days = work[tree_slots]->node.overflow_days;
      for (const auto &c : overflow)
        days[c.day].Add(c.slot, *c.mode, c.high, c.low);
      for (auto it = days.begin(); it != days.end();)
        it = it->second.Empty() ? days.erase(it) : std::next(it);
    }
    outcome.action = core::MaintenanceAction::ApplyDelta;
    outcome.updates.reserve(partitions.size() + 1);
    for (std::size_t slot = 0; slot < partitions.size(); ++slot)
      outcome.updates.push_back(core::StateImage::Make(
          core::TpchQ12Layout::kAccelerator, partitions[slot], versions[slot],
          std::shared_ptr<const TpchQ12StateImage>(std::move(work[slot]))));
    return true;
  }

  void PublishPriorities() {
    outcome.action = core::MaintenanceAction::ApplyDelta;
    const auto typed_priorities =
        std::static_pointer_cast<const TpchQ12State>(std::move(priority_state));
    outcome.updates.push_back(core::StateImage::Make(
        core::TpchQ12Layout::kAccelerator, core::TpchQ12Layout::kOrderPriorities,
        priority_version,
        MakeStateImage(TpchQ12ImageKind::OrderPriorities, 0, typed_priorities)));
  }

  core::AuxEngineServices &services;
  const TpchQ12TxnBuffer &buffer;
  TpchQ12InitialScan *initial_scan;
  core::StagedMaintenanceOutcome &outcome;
  const core::VisibilityToken visibility;

  std::shared_ptr<TpchQ12State> priority_state;
  core::Version priority_version = 0;
  bool priorities_changed = false;
  std::vector<int64_t> removed_orders;      // ascending
  std::vector<Changed> changed_orders;      // ascending by key
  std::vector<ChangedLine> changed_lines;
};

} // namespace

TpchQ12PriorityClass TpchQ12ClassifyPriority(const std::string &priority) {
  if (priority == "1-URGENT" || priority == "2-HIGH")
    return TpchQ12PriorityClass::High;
  return TpchQ12PriorityClass::Low;
}

bool TpchQ12LineitemQualifies(int32_t commitdate_days, int32_t receiptdate_days,
                              int32_t shipdate_days) {
  return commitdate_days < receiptdate_days && shipdate_days < commitdate_days;
}

bool TpchQ12TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ12TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  deltas.clear();
  order_deltas.clear();
}
bool TpchQ12TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
const std::vector<TpchQ12LineitemDelta> &TpchQ12TxnBuffer::Deltas() const {
  return deltas;
}
const std::vector<TpchQ12OrderDelta> &TpchQ12TxnBuffer::OrderDeltas() const {
  return order_deltas;
}
void TpchQ12TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  deltas.clear();
  order_deltas.clear();
}
void TpchQ12TxnBuffer::AddDelta(TpchQ12LineitemDelta delta) {
  pending = true;
  deltas.push_back(delta);
}
void TpchQ12TxnBuffer::AddOrderDelta(TpchQ12OrderDelta delta) {
  pending = true;
  order_deltas.push_back(delta);
}


core::AcceleratorId TpchQ12Module::Id() const { return kId; }
std::vector<core::TableDependency> TpchQ12Module::Dependencies() const {
  return {{config.orders_table, {0, 1}, {0, 5}},
          {config.lineitem_table, {0, 1, 2, 3, 4}, {0, 14, 11, 12, 10}}};
}
std::unique_ptr<core::AuxTxnBuffer> TpchQ12Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ12TxnBuffer>();
}

void TpchQ12Module::Build(core::AuxEngineServices &services,
                          core::StateBuilder &builder) {
  TpchQ12DayModes days;
  auto state_owned = BuildState(services, days);
  std::map<core::PartitionId, TpchQ12Node> nodes;
  for (const auto &[day, modes] : days)
    AddToTree(nodes, day, modes);
  const auto typed_state =
      std::make_shared<const TpchQ12State>(std::move(state_owned));
  builder.Write(core::StateImage::Make(
      core::TpchQ12Layout::kAccelerator,
      core::TpchQ12Layout::kOrderPriorities, 0,
      MakeStateImage(TpchQ12ImageKind::OrderPriorities, 0, typed_state)));
  for (auto &entry : nodes) {
    auto image = NewNodeImage();
    image->node = std::move(entry.second);
    builder.Write(core::StateImage::Make(
        core::TpchQ12Layout::kAccelerator, entry.first, 0,
        std::shared_ptr<const TpchQ12StateImage>(std::move(image))));
  }
  builder.Finish();
}

core::InvalidationPlan
TpchQ12Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.lineitem_table ||
      mutation.table == config.orders_table) {
    core::InvalidationPlan plan;
    plan.scope =
        ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
    return plan;
  }
  return {};
}


void TpchQ12Module::AccumulateMutation(const core::MutationBatch &mutation,
                                       core::AuxEngineServices &,
                                       core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ12TxnBuffer &>(base_buffer);
  if (mutation.table == config.orders_table)
    AccumulateOrders(mutation, buffer);
  else if (mutation.table == config.lineitem_table)
    AccumulateLineitems(mutation, buffer);
}

core::StagedMaintenanceOutcome
TpchQ12Module::StageMaintenance(core::AuxEngineServices &services,
                                const core::InvalidationPlan &,
                                core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ12TxnBuffer &>(base_buffer);
  core::StagedMaintenanceOutcome outcome;
  if (buffer.RequiresRebuild()) {
    core::FinishRebuildAll(
        outcome, services.ChooseMaintenance(core::MaintenanceRequest::RebuildOnly(
                     services.ExpectedQueriesBeforeNextWrite())));
    return outcome;
  }
  if (buffer.Deltas().empty() && buffer.OrderDeltas().empty())
    return outcome;

  TreeMaintainer maintainer(services, buffer, initial_scan, outcome);
  if (!maintainer.Run())
    core::FinishRebuildAll(outcome);
  return outcome;
}

bool TpchQ12Module::CheckValidity(core::AuxEngineServices &services,
                                  const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}


void TpchQ12Module::Execute(core::AuxEngineServices &services,
                            const core::QueryArguments &arguments,
                            core::AuxRowSink &sink) {
  static const std::string kEmpty;
  const std::string *shipmode1 = &kEmpty, *shipmode2 = &kEmpty;
  int32_t start = 0;
  if (arguments.values.size() < 3)
    return ExecuteFallback(services, arguments, sink);
  if (!arguments.values[0].is_null)
    if (const auto *text =
            std::get_if<std::string>(&arguments.values[0].payload))
      shipmode1 = text;
  if (!arguments.values[1].is_null)
    if (const auto *text =
            std::get_if<std::string>(&arguments.values[1].payload))
      shipmode2 = text;
  if (!arguments.values[2].is_null) {
    if (std::holds_alternative<core::AuxDate>(arguments.values[2].payload))
      start =
          std::get<core::AuxDate>(arguments.values[2].payload).days_since_epoch;
    else if (std::holds_alternative<int64_t>(arguments.values[2].payload))
      start = static_cast<int32_t>(std::get<int64_t>(arguments.values[2].payload));
  }
  const auto end = static_cast<int32_t>(sql_date::AddMonths(start, 12));
  thread_local std::vector<core::PartitionId> partitions;
  partitions.clear();
  WindowPartitions(start, end, partitions);
  const bool outside = start < kTreeBase || end > int64_t(kTreeBase) + kTreeN;
  // Common case (every node present): validity and all partitions in one
  // linearized observation. Empty nodes are absent, so otherwise the general
  // path: validity, then whatever exists.
  auto snapshot = services.StateStore().ReadHitPartitions(
      services.CurrentVisibility(), {Id(), {}}, partitions);
  if (!snapshot.available) {
    if (!CheckValidity(services, arguments)) {
      ExecuteFallback(services, arguments, sink);
      return;
    }
    snapshot = services.StateStore().ReadPartitions(
        services.CurrentVisibility(), partitions);
  }
  // At most two nodes per level plus the overflow sum.
  constexpr std::size_t kMaxParts = 2 * (kTreeBits + 1) + 2;
  const TpchQ12Modes *parts[kMaxParts];
  std::size_t part_count = 0;
  TpchQ12Modes overflow_sum;
  for (const auto &image : snapshot.partitions) {
    const auto *node = AsNodeImage(image);
    if (!node || part_count >= kMaxParts - 1) {
      ExecuteFallback(services, arguments, sink);
      return;
    }
    if (image.partition == kOverflowPartition) {
      for (const auto &day : node->overflow_days) {
        if (day.first < start || day.first >= end)
          continue;
        MergeModes(overflow_sum, day.second);
      }
    } else {
      parts[part_count++] = &node->modes;
    }
  }
  if (outside)
    parts[part_count++] = &overflow_sum;
  TpchQ12FinalizeAndEmit(parts, part_count, *shipmode1, *shipmode2, sink);
}

TpchQ12State TpchQ12Module::BuildState(core::AuxEngineServices &services,
                                       TpchQ12DayModes &day_modes) const {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q12 requires a typed initial scan source");
  using Priorities = decltype(TpchQ12State::order_priorities);
  using Counts = decltype(TpchQ12State::line_counts);
  TpchQ12State state;
  state.order_priorities = core::PartitionedBuild<Priorities>(
      services, [] { return Priorities{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanOrders(services, parts,
                                 [&](std::size_t p, const TpchQ12OrderInput &input) {
                                   sink(p, core::SingleRowBatch<TpchQ12OrderInput>{input});
                                 });
      },
      [](Priorities &priorities, const TpchQ12OrderInput &input) {
        priorities.Set(input.key,
                       static_cast<uint8_t>(TpchQ12ClassifyPriority(input.orderpriority)));
      },
      [](Priorities &into, Priorities &&from) {
        into.MergeFrom(std::move(from), [](uint8_t mine, uint8_t theirs) {
          return theirs ? theirs : mine; // the last order row of a key wins
        });
      });

  struct LineitemPartial {
    TpchQ12DayModes days;
    Counts counts;
    int32_t last_day = 0;
    TpchQ12Modes *last_modes = nullptr; // receipt days arrive in runs

    TpchQ12Modes &ModesOf(int32_t day) {
      if (!last_modes || day != last_day) {
        last_day = day;
        last_modes = &days[day];
      }
      return *last_modes;
    }
  };
  auto lineitems = core::PartitionedBuild<LineitemPartial>(
      services, [] { return LineitemPartial{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanLineitems(services, parts,
                                    [&](std::size_t p, const TpchQ12LineitemInput &input) {
                                      sink(p, core::SingleRowBatch<TpchQ12LineitemInput>{input});
                                    });
      },
      [&](LineitemPartial &partial, const TpchQ12LineitemInput &input) {
        const auto priority = state.PriorityOf(input.orderkey);
        if (priority == TpchQ12PriorityClass::Absent ||
            priority == TpchQ12PriorityClass::Neither)
          return;
        if (!TpchQ12LineitemQualifies(input.commitdate_days, input.receiptdate_days,
                                      input.shipdate_days))
          return;
        partial.ModesOf(input.receiptdate_days)
            .Add(TpchQ12ModeSlot(input.shipmode), input.shipmode,
                 priority == TpchQ12PriorityClass::High, priority == TpchQ12PriorityClass::Low);
        const auto count = partial.counts.Get(input.orderkey);
        if (count != std::numeric_limits<uint16_t>::max())
          partial.counts.Set(input.orderkey, static_cast<uint16_t>(count + 1));
      },
      [](LineitemPartial &into, LineitemPartial &&from) {
        for (const auto &[day, modes] : from.days)
          MergeModes(into.ModesOf(day), modes);
        into.counts.MergeFrom(std::move(from.counts), [](uint16_t mine, uint16_t theirs) {
          return static_cast<uint16_t>(std::min<uint32_t>(
              std::numeric_limits<uint16_t>::max(), uint32_t{mine} + theirs));
        });
      });
  day_modes = std::move(lineitems.days);
  state.line_counts = std::move(lineitems.counts);
  return state;
}

std::vector<core::CalibrationWrite> TpchQ12Module::CalibrationWrites() const {
  return TpchQ12CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
