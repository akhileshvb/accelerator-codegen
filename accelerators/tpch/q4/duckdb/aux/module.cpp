#include "tpch/q4/duckdb/aux/module.hpp"
#if defined(__AVX2__) || defined(__AVX512F__)
#include <immintrin.h>
#endif
#include "tpch/q4/calibration_writes.hpp"
#include "prims/sql_date.hpp"
#include "substrate/aux/core/aux_cells.hpp"
#include "substrate/aux/core/byte_stream.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/partitioned_build.hpp"
#include "substrate/aux/core/query_module_helpers.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <stdexcept>
#include <utility>


namespace accelerator_codegen::aux_state::query {
namespace {

constexpr uint32_t kMaxStride = 64;
// Months in the query's order-date window.
constexpr int32_t kWindowMonths = 3;
// Deltas above which the cost model chooses between delta-apply and rebuild.
constexpr std::size_t kBulkDeltaCount = 256;
// Shards per slice when sorting the built index (untimed).
constexpr std::size_t kSortShardGrain = 16;
// Shards per slice when counting the segment tree's leaves (untimed).
constexpr std::size_t kLeafShardGrain = 256;
// Buffered changes per slice when turning them into order touches.
constexpr std::size_t kTouchGrain = 4096;
// Upper bound on the shard groups of one batch (TpchQ4EligIndex::Txn::ApplyGrouped).
constexpr std::size_t kMaxGroups = 1024;
// Rows per shard group when applying a batch.
constexpr std::size_t kRowsPerGroup = 2048;
// Touched mid arrays per slice when publishing the eligibility index (untimed).
constexpr std::size_t kFinishGrain = 16;

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

// acc[0..n) += row[0..n) (AVX-512 / AVX2 when available, scalar otherwise).
// acc must be 64-byte aligned.
inline void AddRow(int64_t *acc, const int64_t *row, uint32_t n) {
  uint32_t i = 0;
#if defined(__AVX512F__)
  for (; i + 8 <= n; i += 8) {
    const __m512i a = _mm512_load_si512(acc + i);
    const __m512i b = _mm512_loadu_si512(row + i);
    _mm512_store_si512(acc + i, _mm512_add_epi64(a, b));
  }
  if (i < n) {
    const __mmask8 m = static_cast<__mmask8>((1u << (n - i)) - 1u);
    const __m512i a = _mm512_maskz_loadu_epi64(m, acc + i);
    const __m512i b = _mm512_maskz_loadu_epi64(m, row + i);
    _mm512_mask_storeu_epi64(acc + i, m, _mm512_add_epi64(a, b));
    return;
  }
#elif defined(__AVX2__)
  for (; i + 4 <= n; i += 4) {
    const __m256i a = _mm256_load_si256(reinterpret_cast<const __m256i *>(acc + i));
    const __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(row + i));
    _mm256_store_si256(reinterpret_cast<__m256i *>(acc + i), _mm256_add_epi64(a, b));
  }
#endif
  for (; i < n; ++i)
    acc[i] += row[i];
}

// Splits a flat node-major tree (node * stride + priority) into immutable
// pages of kTpchQ4PageNodes nodes.
std::vector<std::shared_ptr<const std::vector<int64_t>>>
PaginateTree(const std::vector<int64_t> &flat, std::size_t nodes,
             uint32_t stride) {
  const std::size_t page_len =
      static_cast<std::size_t>(kTpchQ4PageNodes) * stride;
  const std::size_t count = (nodes + kTpchQ4PageNodes - 1) / kTpchQ4PageNodes;
  std::vector<std::shared_ptr<const std::vector<int64_t>>> pages;
  pages.reserve(count);
  for (std::size_t p = 0; p < count; ++p) {
    auto page = std::make_shared<std::vector<int64_t>>(page_len, 0);
    const std::size_t begin = p * page_len;
    const std::size_t end = std::min(flat.size(), begin + page_len);
    if (begin < end)
      std::copy(flat.begin() + begin, flat.begin() + end, page->begin());
    pages.push_back(std::move(page));
  }
  return pages;
}

bool ReadLineitem(const core::AuxRow &row, TpchQ4LineitemInput &out) {
  int64_t orderkey = 0;
  if (!core::IntCell(row, 0, orderkey) ||
      !core::DateCell(row, 1, out.commitdate_days) ||
      !core::DateCell(row, 2, out.receiptdate_days))
    return false;
  out.orderkey = orderkey;
  return true;
}

bool LineitemQualifies(int32_t commitdate_days, int32_t receiptdate_days) {
  return commitdate_days < receiptdate_days;
}

bool ReadOrder(const core::AuxRow &row, int64_t &orderkey,
               int32_t &orderdate_days, std::string &orderpriority) {
  return IntCell(row, 0, orderkey) && core::DateCell(row, 1, orderdate_days) &&
         core::StringCell(row, 2, orderpriority);
}

// Orders of one scan partition: eligibility entries (count 0) numbered by the
// partition's own priority dictionary, plus the partition's order-date range.
struct OrdersPartial {
  core::InternedPieces<TpchQ4EligIndex::Element> rows;
  bool any = false;
  int32_t min_orderdate_days = 0, max_orderdate_days = 0;
};

// Appends `from` to `into` as if its rows had been scanned after into's.
void MergeOrders(OrdersPartial &into, OrdersPartial &&from) {
  core::InternedPieces<TpchQ4EligIndex::Element>::Merge(into.rows, std::move(from.rows));
  if (from.any) {
    into.min_orderdate_days = into.any ? std::min(into.min_orderdate_days, from.min_orderdate_days)
                                       : from.min_orderdate_days;
    into.max_orderdate_days = into.any ? std::max(into.max_orderdate_days, from.max_orderdate_days)
                                       : from.max_orderdate_days;
    into.any = true;
  }
}

using TreePages = std::vector<std::shared_ptr<const std::vector<int64_t>>>;

// Tree leaf deltas, dense: leaf ordinal * priorities + priority.
using LeafAdjustments = std::vector<int64_t>;

// The current tree image, or null when it is missing or does not match `side`.
std::shared_ptr<const TpchQ4TreePayload>
LoadTreePayload(core::AuxEngineServices &services, const TpchQ4State &side) {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {TpchQ4Layout::kTreeBase});
  for (const auto &image : snapshot.partitions) {
    if (image.partition != TpchQ4Layout::kTreeBase)
      continue;
    const auto *typed = AsQ4TreeImage(image);
    if (!typed || typed->kind != TpchQ4ImageKind::Tree || !typed->payload ||
        typed->payload->tree_size != side.tree_size || !typed->payload->WellFormed())
      return nullptr;
    return typed->payload;
  }
  return nullptr;
}

// One change to an order's eligibility: an order row (is_order: delete or insert,
// with its date and priority) or a qualifying lineitem (sign only).
struct OrderTouch {
  int64_t orderkey = 0;
  int32_t orderdate_days = 0;
  int32_t priority_idx = 0;
  int8_t sign = 0;
  bool is_order = false;
};

// Registers the priorities of inserted orders in `side` (in delta order). New ones
// are rare, so the pass that numbers them runs only when a parallel check finds one.
void RegisterPriorities(core::AuxEngineServices &services, const TpchQ4TxnBuffer &buffer,
                        TpchQ4State &side) {
  const auto &orders = buffer.OrderDeltas();
  std::atomic<bool> unknown{false};
  core::ParallelEach(services, orders.size(), kTouchGrain, [&](std::size_t i) {
    if (orders[i].sign > 0 && !side.priority_map.count(orders[i].orderpriority))
      unknown = true;
  });
  if (!unknown)
    return;
  for (const auto &delta : orders)
    if (delta.sign > 0)
      TpchQ4PriorityIndex(delta.orderpriority, side.priority_names, side.priority_map);
}

// The buffered order and lineitem changes as pieces, order rows first, each piece
// a slice of the buffer so piece order is delta order.
std::vector<std::vector<OrderTouch>> TouchPieces(core::AuxEngineServices &services,
                                                 const TpchQ4TxnBuffer &buffer,
                                                 const TpchQ4State &side) {
  const auto &orders = buffer.OrderDeltas();
  const auto &lines = buffer.Deltas();
  const std::size_t order_slices = core::SliceCount(services, orders.size(), kTouchGrain);
  const std::size_t line_slices = core::SliceCount(services, lines.size(), kTouchGrain);
  std::vector<std::vector<OrderTouch>> pieces(order_slices + line_slices);
  core::RunSlices(services, order_slices, orders.size(),
                  [&](std::size_t slice, std::size_t begin, std::size_t end) {
    for (std::size_t i = begin; i < end; ++i) {
      const auto &delta = orders[i];
      const auto priority = delta.sign > 0 ? side.priority_map.at(delta.orderpriority) : 0;
      pieces[slice].push_back({delta.orderkey, delta.orderdate_days, priority, delta.sign, true});
    }
  });
  core::RunSlices(services, line_slices, lines.size(),
                  [&](std::size_t slice, std::size_t begin, std::size_t end) {
    for (std::size_t i = begin; i < end; ++i)
      pieces[order_slices + slice].push_back({lines[i].orderkey, 0, 0, lines[i].sign, false});
  });
  return pieces;
}

// One order's new info after its changes (order rows in sequence: delete, then
// insert for an update; lineitem deltas summed), or nullopt when it no longer exists.
std::optional<TpchQ4OrderInfo> ReplayOrder(const TpchQ4OrderInfo *current,
                                           const OrderTouch *begin, const OrderTouch *end) {
  TpchQ4OrderInfo info;
  if (current)
    info = *current;
  bool exists = current != nullptr;
  for (const auto *t = begin; t != end; ++t) {
    if (!t->is_order) {
      info.qualifying_count += t->sign;
    } else if (t->sign < 0) {
      exists = false;
    } else if (t->sign > 0) {
      info.orderdate_days = t->orderdate_days;
      info.priority_idx = t->priority_idx;
      exists = true;
    }
  }
  if (info.qualifying_count < 0)
    info.qualifying_count = 0;
  return exists ? std::optional<TpchQ4OrderInfo>(info) : std::nullopt;
}

// Adds `by` to the leaf of `info` in a dense leaf-delta array.
void AdjustLeaf(const TpchQ4State &side, const TpchQ4OrderInfo &info, int64_t by,
                std::vector<int64_t> &adjustments) {
  const int32_t ord = info.orderdate_days - side.min_orderdate_days;
  const auto num_p = static_cast<int32_t>(side.priority_names.size());
  if (ord < 0 || ord >= static_cast<int32_t>(side.tree_size) || info.priority_idx < 0 ||
      info.priority_idx >= num_p)
    return;
  adjustments[static_cast<std::size_t>(ord) * num_p + info.priority_idx] += by;
}

// Per order key (the order's changes are contiguous after a stable sort by key):
// removes the old contribution to the tree (if it counted), replays the changes,
// writes or erases the order, adds the new contribution. Groups of shards run
// concurrently, each with its own leaf deltas; the sum is returned.
LeafAdjustments ReplayOrderChanges(core::AuxEngineServices &services, const TpchQ4TxnBuffer &buffer,
                                   TpchQ4State &side, TpchQ4EligIndex::Txn &elig) {
  RegisterPriorities(services, buffer, side);
  auto pieces = TouchPieces(services, buffer, side);
  const std::size_t leaf_cells = static_cast<std::size_t>(side.tree_size) * side.priority_names.size();
  std::vector<LeafAdjustments> partials(kMaxGroups);
  const auto parallel_for = [&](std::size_t n, const auto &fn) {
    core::ParallelEach(services, n, 1, fn);
  };
  elig.ApplyGrouped(
      pieces, [](const OrderTouch &t) -> const int64_t & { return t.orderkey; }, parallel_for,
      [&](std::size_t group, TpchQ4EligIndex::Txn::Scope &scope, std::vector<OrderTouch> &rows) {
        auto &adjustments = partials[group];
        adjustments.assign(leaf_cells, 0);
        std::stable_sort(rows.begin(), rows.end(), [](const OrderTouch &a, const OrderTouch &b) {
          return a.orderkey < b.orderkey;
        });
        for (auto *run = rows.data(); run != rows.data() + rows.size();) {
          auto *next = run;
          while (next != rows.data() + rows.size() && next->orderkey == run->orderkey)
            ++next;
          const auto *found = scope.Find(run->orderkey);
          const auto *current = found ? &found->value : nullptr;
          if (current && current->qualifying_count > 0)
            AdjustLeaf(side, *current, -1, adjustments);
          const auto info = ReplayOrder(current, run, next);
          if (!info) {
            if (current)
              scope.Erase(run->orderkey);
          } else {
            scope.Upsert({run->orderkey, *info});
            if (info->qualifying_count > 0)
              AdjustLeaf(side, *info, +1, adjustments);
          }
          run = next;
        }
      },
      core::CappedGrain(kRowsPerGroup));
  std::vector<LeafAdjustments> used;
  for (auto &p : partials)
    if (!p.empty())
      used.push_back(std::move(p));
  if (used.empty())
    return LeafAdjustments(leaf_cells, 0);
  core::SumInto(core::Parallelism::OnPool(services), used);
  return std::move(used[0]);
}

// Adds each leaf's delta to the leaf and every ancestor (an ancestor is the sum
// of its children, so adding the same delta keeps the tree exact). Pages are
// shared with the published tree and copied the first time they are hit. A new
// priority string widens the per-node stride (rare): the tree is re-laid out.
TreePages ApplyLeafAdjustments(const TpchQ4TreePayload &tree_in, const TpchQ4State &side,
                               const LeafAdjustments &adjustments, uint32_t &stride) {
  TreePages pages = tree_in.pages;
  const std::size_t total_nodes = static_cast<std::size_t>(side.tree_size) * 2;
  const uint32_t num_p = static_cast<uint32_t>(side.priority_names.size());
  if (num_p > stride) {
    std::vector<int64_t> wide(total_nodes * num_p, 0);
    for (std::size_t n = 0; n < total_nodes; ++n) {
      const int64_t *src = tree_in.Node(static_cast<uint32_t>(n));
      for (uint32_t p = 0; p < stride; ++p)
        wide[n * num_p + p] = src[p];
    }
    pages = PaginateTree(wide, total_nodes, num_p);
    stride = num_p;
  }
  std::vector<std::shared_ptr<std::vector<int64_t>>> dirty(pages.size());
  for (uint32_t ord = 0; ord < side.tree_size; ++ord) {
    const int64_t *adj = adjustments.data() + static_cast<std::size_t>(ord) * num_p;
    if (std::all_of(adj, adj + num_p, [](int64_t v) { return v == 0; }))
      continue;
    const std::size_t lim = std::min<std::size_t>(num_p, stride);
    for (uint32_t node = LeafNode(side.tree_size, static_cast<int32_t>(ord)); node > 0; node >>= 1) {
      const std::size_t pg = node / kTpchQ4PageNodes;
      if (!dirty[pg])
        dirty[pg] = std::make_shared<std::vector<int64_t>>(*pages[pg]);
      int64_t *row = dirty[pg]->data() +
                     static_cast<std::size_t>(node % kTpchQ4PageNodes) * stride;
      for (std::size_t pi = 0; pi < lim; ++pi)
        row[pi] += adj[pi];
    }
  }
  for (std::size_t pg = 0; pg < dirty.size(); ++pg)
    if (dirty[pg])
      pages[pg] = std::move(dirty[pg]);
  return pages;
}

void PublishImages(core::AuxEngineServices &services, TpchQ4State &&side,
                   TpchQ4EligIndex::Txn &elig, TreePages &&pages, uint32_t stride,
                   core::StagedMaintenanceOutcome &outcome) {
  outcome.action = core::MaintenanceAction::ApplyDelta;
  auto side_payload = std::make_shared<TpchQ4State>();
  side_payload->priority_names = std::move(side.priority_names);
  side_payload->priority_map = std::move(side.priority_map);
  side_payload->min_orderdate_days = side.min_orderdate_days;
  side_payload->max_orderdate_days = side.max_orderdate_days;
  side_payload->tree_size = side.tree_size;
  outcome.updates.push_back(core::StateImage::Make(
      TpchQ4Layout::kAccelerator, TpchQ4Layout::kSide, 0, MakeSideImage(side_payload)));

  auto eligibility = std::make_shared<TpchQ4EligibilityPayload>();
  eligibility->order_eligibility = elig.Finish([&](std::size_t n, const auto &fn) {
    core::ParallelEach(services, n, kFinishGrain, fn);
  });
  outcome.updates.push_back(core::StateImage::Make(
      TpchQ4Layout::kAccelerator, TpchQ4Layout::kEligibility, 0,
      MakeEligibilityImage(eligibility)));

  auto tree = std::make_shared<TpchQ4TreePayload>();
  tree->tree_size = side.tree_size;
  tree->tree_stride = stride;
  tree->pages = std::move(pages);
  outcome.updates.push_back(core::StateImage::Make(
      TpchQ4Layout::kAccelerator, TpchQ4Layout::kTreeBase, 0, MakeTreeImage(tree)));
}

}  // namespace

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
  auto state = BuildState(services);
  auto side_state = std::make_shared<TpchQ4State>();
  side_state->priority_names = state.priority_names;
  side_state->priority_map = state.priority_map;
  side_state->min_orderdate_days = state.min_orderdate_days;
  side_state->max_orderdate_days = state.max_orderdate_days;
  side_state->tree_size = state.tree_size;
  builder.Write(core::StateImage::Make(
      TpchQ4Layout::kAccelerator, TpchQ4Layout::kSide, 0,
      MakeSideImage(side_state)));
  auto eligibility_payload = std::make_shared<TpchQ4EligibilityPayload>();
  eligibility_payload->order_eligibility = std::move(state.order_eligibility);
  builder.Write(core::StateImage::Make(
      TpchQ4Layout::kAccelerator, TpchQ4Layout::kEligibility, 0,
      MakeEligibilityImage(eligibility_payload)));
  auto tree_payload = std::make_shared<TpchQ4TreePayload>();
  tree_payload->tree_size = state.tree_size;
  tree_payload->tree_stride = state.tree_stride;
  tree_payload->pages = PaginateTree(
      state.tree, static_cast<std::size_t>(state.tree_size) * 2,
      state.tree_stride);
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
    core::FinishRebuildAll(
        outcome, services.ChooseMaintenance(core::MaintenanceRequest::RebuildOnly(
                     services.ExpectedQueriesBeforeNextWrite())));
    return outcome;
  }
  const auto total_deltas = buffer.Deltas().size() + buffer.OrderDeltas().size();
  if (total_deltas == 0)
    return outcome;

  // RF-sized batches are always applied incrementally; the cost model is only
  // consulted for bulk changes.
  if (total_deltas > kBulkDeltaCount) {
    const auto decision =
        services.ChooseMaintenance(core::MaintenanceRequest::IncrementalSingleState(
            total_deltas, services.ExpectedQueriesBeforeNextWrite()));
    if (decision.action != core::MaintenanceAction::ApplyDelta) {
      core::FinishRebuildAll(outcome, decision);
      return outcome;
    }
  }

  TpchQ4State side;
  std::shared_ptr<const TpchQ4EligibilityPayload> elig_payload;
  const auto tree_in = LoadSide(services, side, elig_payload)
                           ? LoadTreePayload(services, side)
                           : nullptr;
  if (!tree_in) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }

  TpchQ4EligIndex::Txn elig(elig_payload->order_eligibility);
  const auto adjustments = ReplayOrderChanges(services, buffer, side, elig);
  uint32_t stride = tree_in->tree_stride;
  auto pages = ApplyLeafAdjustments(*tree_in, side, adjustments, stride);
  PublishImages(services, std::move(side), elig, std::move(pages), stride, outcome);
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
  if (arguments.values.size() < 1)
    return ExecuteFallback(services, arguments, sink);

  int32_t start = 0;
  if (!arguments.values[0].is_null) {
    if (std::holds_alternative<core::AuxDate>(arguments.values[0].payload))
      start = std::get<core::AuxDate>(arguments.values[0].payload).days_since_epoch;
    else if (std::holds_alternative<int64_t>(arguments.values[0].payload))
      start = static_cast<int32_t>(std::get<int64_t>(arguments.values[0].payload));
  }

  const auto end = static_cast<int32_t>(core::sql_date::AddMonths(start, kWindowMonths));

  // One ReadPartitions for both the side metadata and the tree.
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

  // Canonical cover on the stack (at most ~2 log2(tree_size) nodes), summed
  // straight out of the pages with a vector add per node row.
  uint32_t node_indices[96];
  std::size_t node_count = 0;
  uint32_t l = LeafNode(side.tree_size, left_ord);
  uint32_t r = LeafNode(side.tree_size, right_ord);
  while (l < r && node_count + 2 <= 96) {
    if (l & 1)
      node_indices[node_count++] = l++;
    if (r & 1)
      node_indices[node_count++] = --r;
    l >>= 1;
    r >>= 1;
  }

  const std::size_t stride = tree_payload.tree_stride;
  std::vector<int64_t> counts(side.priority_names.size(), 0);
  if (stride > 0 && stride <= kMaxStride) {
    alignas(64) int64_t acc[kMaxStride] = {};
    for (std::size_t i = 0; i < node_count; ++i) {
      if (!tree_payload.Valid(node_indices[i]))
        continue;
      AddRow(acc, tree_payload.Node(node_indices[i]),
             static_cast<uint32_t>(stride));
    }
    const std::size_t lim = std::min(stride, counts.size());
    for (std::size_t pi = 0; pi < lim; ++pi)
      counts[pi] = acc[pi];
  } else {
    for (std::size_t i = 0; i < node_count; ++i) {
      if (!tree_payload.Valid(node_indices[i]))
        continue;
      const int64_t *node = tree_payload.Node(node_indices[i]);
      for (size_t pi = 0; pi < stride && pi < counts.size(); ++pi)
        counts[pi] += node[pi];
    }
  }

  EmitResult(side, counts, sink);
}

TpchQ4State TpchQ4Module::BuildState(core::AuxEngineServices &services) const {
  TpchQ4State state;
  if (!initial_scan)
    throw std::logic_error("TPC-H Q4 requires a typed initial scan source");
  using scan::DateColumn;
  using scan::IntColumn;
  using scan::TextColumn;

  // First scan: orders are kept as one piece of rows per scan partition (no
  // per-partition shard arrays, no merge of rows); counts are filled in by the
  // lineitem scan.
  auto orders = core::PartitionedBuild<OrdersPartial>(
      services, [] { return OrdersPartial{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanTable(services, "orders",
                                {IntColumn("o_orderkey"), DateColumn("o_orderdate"),
                                 TextColumn("o_orderpriority")},
                                parts, sink);
      },
      [](OrdersPartial &p, const auto &row) {
        const int32_t date = row.Date(1);
        p.rows.Add({row.Int(0), TpchQ4OrderInfo{date, p.rows.Intern(row.Text(2)), 0}});
        p.min_orderdate_days = p.any ? std::min(p.min_orderdate_days, date) : date;
        p.max_orderdate_days = p.any ? std::max(p.max_orderdate_days, date) : date;
        p.any = true;
      },
      MergeOrders);
  const auto parallelism = core::Parallelism::OnPool(services);
  orders.rows.Unify(
      parallelism,
      [&](const std::string &name) {
        return TpchQ4PriorityIndex(name, state.priority_names, state.priority_map);
      },
      [](TpchQ4EligIndex::Element &e, const std::vector<int32_t> &table) {
        e.value.priority_idx = table[e.value.priority_idx];
      });
  if (orders.any) {
    state.min_orderdate_days = orders.min_orderdate_days;
    state.max_orderdate_days = orders.max_orderdate_days;
  }
  std::size_t order_count = 0;
  for (const auto &piece : orders.rows.pieces)
    order_count += piece.size();
  TpchQ4EligIndex::Builder shards(order_count);
  shards.AddPieces(orders.rows.pieces, parallelism);
  shards.Sort([&](std::size_t n, const auto &fn) {
    core::ParallelEach(services, n, kSortShardGrain, fn);
  });

  if (state.min_orderdate_days > state.max_orderdate_days) {
    state.min_orderdate_days = 0;
    state.max_orderdate_days = 0;
  }

  // Second scan: count qualifying lineitems per order. The index is sorted and
  // no longer changes shape, so partitions add to the counters atomically.
  struct NoPartial {};
  core::PartitionedBuild<NoPartial>(
      services, [] { return NoPartial{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanTable(services, "lineitem",
                                {IntColumn("l_orderkey"), DateColumn("l_commitdate"),
                                 DateColumn("l_receiptdate")},
                                parts, sink);
      },
      [&](NoPartial &, const auto &row) {
        if (!LineitemQualifies(row.Date(1), row.Date(2)))
          return;
        if (auto *entry = shards.Find(row.Int(0)))
          AtomicAdd<int64_t>(entry->value.qualifying_count, 1);
      },
      [](NoPartial &, NoPartial &&) {});

  // Build segment tree (flat: node * stride + priority)
  int32_t num_days = state.max_orderdate_days - state.min_orderdate_days + 1;
  if (num_days < 1)
    num_days = 1;
  state.tree_size = core::NextPowerOf2(static_cast<uint32_t>(num_days));
  const uint32_t num_p = static_cast<uint32_t>(state.priority_names.size());
  state.tree_stride = num_p;
  state.tree.assign(static_cast<std::size_t>(state.tree_size) * 2 * num_p, 0);

  // Populate leaves: slices of shards count into private trees, summed in parallel.
  std::vector<std::vector<int64_t>> trees(
      core::SliceCount(services, shards.shard_count(), kLeafShardGrain));
  core::ParallelSlices(
      services, shards.shard_count(), kLeafShardGrain,
      [&](std::size_t slice, std::size_t begin, std::size_t end) {
        auto &tree = trees[slice];
        tree.assign(state.tree.size(), 0);
        shards.ForEachInShards(begin, end, [&](const TpchQ4EligIndex::Element &entry) {
          const auto &info = entry.value;
          if (info.qualifying_count <= 0)
            return;
          int32_t ord = info.orderdate_days - state.min_orderdate_days;
          if (ord < 0 || ord >= static_cast<int32_t>(state.tree_size))
            return;
          uint32_t leaf = LeafNode(state.tree_size, ord);
          if (info.priority_idx >= 0 && info.priority_idx < static_cast<int32_t>(num_p))
            tree[static_cast<std::size_t>(leaf) * num_p + info.priority_idx] += 1;
        });
      });
  core::SumInto(parallelism, trees);
  state.tree = std::move(trees[0]);

  // Populate internal nodes
  for (uint32_t i = state.tree_size - 1; i > 0; i--)
    for (uint32_t pi = 0; pi < num_p; ++pi)
      state.tree[static_cast<std::size_t>(i) * num_p + pi] =
          state.tree[static_cast<std::size_t>(i * 2) * num_p + pi] +
          state.tree[static_cast<std::size_t>(i * 2 + 1) * num_p + pi];

  // Pack shards into the immutable index (moves, no copies).
  state.order_eligibility = shards.Finish([&](std::size_t n, const auto &fn) {
    core::ParallelEach(services, n, 1, fn);
  });
  return state;
}

bool TpchQ4Module::LoadSide(
    core::AuxEngineServices &services, TpchQ4State &state,
    std::shared_ptr<const TpchQ4EligibilityPayload> &eligibility) const {
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
      eligibility = typed->payload;
      have_eligibility = true;
    }
  }
  return have_meta && have_eligibility;
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
