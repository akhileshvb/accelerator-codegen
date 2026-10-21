#include "tpch/q5/duckdb/aux/module.hpp"
#if defined(__AVX2__) || defined(__AVX512F__)
#include <immintrin.h>
#endif
#include "tpch/q5/calibration_writes.hpp"
#include "prims/sql_date.hpp"
#include "substrate/aux/core/aux_cells.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/partitioned_build.hpp"
#include "substrate/aux/core/query_module_helpers.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include "tpch/q5/layout.hpp"


namespace accelerator_codegen::aux_state::query {
namespace {

// Shards per slice when sorting a built index or summing it into the tree (untimed).
constexpr std::size_t kShardGrain = 16;
// Touched mid arrays per slice when publishing a side index (untimed).
constexpr std::size_t kFinishGrain = 16;

template <class Builder> auto FinishShards(core::AuxEngineServices &services, Builder &index) {
  return index.Finish([&](std::size_t n, const auto &fn) {
    core::ParallelEach(services, n, 1, fn);
  });
}

template <class Builder>
void SortShards(core::AuxEngineServices &services, Builder &index) {
  index.Sort([&](std::size_t n, const auto &fn) {
    core::ParallelEach(services, n, kShardGrain, fn);
  });
}

// Order rows of one scan partition, with its order-date range.
struct OrdersPartial {
  core::RowPieces<TpchQ5ShardIndex<TpchQ5OrderRec>::Element> rows;
  int32_t min_orderdate_days = std::numeric_limits<int32_t>::max();
  int32_t max_orderdate_days = std::numeric_limits<int32_t>::min();
};

// The sorted key -> {nationkey, 0} index of a dimension table.
template <class Rec>
typename TpchQ5ShardIndex<Rec>::Builder
BuildDimension(core::AuxEngineServices &services, TpchQ5InitialScan &source, const char *table,
               const char *key_column, const char *nation_column) {
  using Rows = core::RowPieces<typename TpchQ5ShardIndex<Rec>::Element>;
  using scan::IntColumn;
  auto scanned = core::PartitionedBuild<Rows>(
      services, [] { return Rows{}; },
      [&](std::size_t parts, auto &&sink) {
        source.ScanTable(services, table, {IntColumn(key_column), IntColumn(nation_column)},
                         parts, sink);
      },
      [](Rows &into, const auto &row) {
        into.Add({row.Int(0), Rec{static_cast<int32_t>(row.Int(1)), 0}});
      },
      Rows::Merge);
  typename TpchQ5ShardIndex<Rec>::Builder index;
  index.AddPieces(scanned.pieces, core::Parallelism::OnPool(services));
  SortShards(services, index);
  return index;
}

// Lineitems of suppliers that do not exist, per supplier key.
using OrphanSuppliers = std::unordered_map<int64_t, int32_t>;

// Deltas above which the cost model chooses between delta-apply and rebuild.
constexpr std::size_t kBulkDeltaCount = 256;
// Months in the query's order-date window.
constexpr int32_t kWindowMonths = 12;
constexpr uint32_t kQ5MaxStride = 64;

// extendedprice * (1 - discount) at scale 4.
int64_t ComputeRevenueRaw(int64_t extended_price_raw, int64_t discount_raw) {
  return extended_price_raw * (100 - discount_raw);
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
                  const std::shared_ptr<const TpchQ5Tree> &tree) {
  auto image = std::make_shared<TpchQ5StateImage>();
  image->is_side = false;
  image->node_index = node_index;
  image->tree = tree;
  return image;
}

const TpchQ5StateImage *AsQ5Image(const core::StateImage &image) {
  return image.As<TpchQ5StateImage>();
}

bool ReadLineitem(const core::AuxRow &row, TpchQ5LineitemInput &out) {
  int64_t orderkey = 0, suppkey = 0;
  int64_t price_raw = 0, discount_raw = 0;
  int32_t price_scale = 2, discount_scale = 2;
  if (!core::IntCell(row, 0, orderkey))
    return false;

  // Scan and catalog probes project (orderkey, suppkey, price, discount).
  if (!core::IntCell(row, 1, suppkey))
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
  if (!core::IntCell(row, 0, orderkey) || !core::IntCell(row, 1, custkey) ||
      !core::DateCell(row, 2, orderdate_days))
    return false;
  return true;
}

// Copy-on-write writer over a published (immutable) tree: a delta is added
// along the leaf-to-root path; only the touched 32-node pages are copied.
class Q5TreeTxn {
public:
  explicit Q5TreeTxn(const TpchQ5Tree &base)
      : base_(base), dirty_(base.pages.size()) {}

  void Add(uint32_t leaf, int32_t col, int64_t delta) {
    const uint32_t stride = base_.stride;
    for (uint32_t node = leaf; node >= 1; node >>= 1) {
      const uint32_t p = node / kTpchQ5PageNodes;
      if (p >= dirty_.size())
        continue;
      if (!dirty_[p]) {
        dirty_[p] = std::make_shared<std::vector<int64_t>>(*base_.pages[p]);
        any_ = true;
      }
      (*dirty_[p])[static_cast<std::size_t>(node % kTpchQ5PageNodes) * stride +
                   static_cast<uint32_t>(col)] += delta;
    }
  }
  bool Dirty() const { return any_; }
  TpchQ5Tree Finish() {
    TpchQ5Tree tree = base_;
    for (std::size_t p = 0; p < dirty_.size(); ++p)
      if (dirty_[p])
        tree.pages[p] = std::move(dirty_[p]);
    return tree;
  }

private:
  const TpchQ5Tree &base_;
  std::vector<std::shared_ptr<std::vector<int64_t>>> dirty_;
  bool any_ = false;
};

TpchQ5Tree PaginateTree(const std::vector<int64_t> &flat, uint32_t tree_size,
                        uint32_t stride) {
  TpchQ5Tree tree;
  tree.tree_size = tree_size;
  tree.stride = stride;
  const std::size_t nodes = static_cast<std::size_t>(tree_size) * 2;
  const std::size_t pages = (nodes + kTpchQ5PageNodes - 1) / kTpchQ5PageNodes;
  const std::size_t page_len = static_cast<std::size_t>(kTpchQ5PageNodes) * stride;
  tree.pages.reserve(pages);
  for (std::size_t p = 0; p < pages; ++p) {
    auto page = std::make_shared<std::vector<int64_t>>(page_len, 0);
    const std::size_t begin = p * page_len;
    const std::size_t end = std::min(flat.size(), begin + page_len);
    if (begin < end)
      std::copy(flat.begin() + begin, flat.begin() + end, page->begin());
    tree.pages.push_back(std::move(page));
  }
  return tree;
}

// Splits a built state into the two published images.
void AppendStateImages(TpchQ5State &&state,
                       std::vector<core::StateImage> &updates) {
  auto tree = std::make_shared<const TpchQ5Tree>(std::move(state.tree));
  auto side = std::make_shared<TpchQ5State>();
  side->meta = std::move(state.meta);
  side->orders = std::move(state.orders);
  side->supps = std::move(state.supps);
  side->custs = std::move(state.custs);
  updates.push_back(core::StateImage::Make(
      TpchQ5Module::kId, core::TpchQ5Layout::kSide, 0,
      MakeSideImage(std::shared_ptr<const TpchQ5State>(std::move(side)))));
  updates.push_back(core::StateImage::Make(
      TpchQ5Module::kId, core::TpchQ5Layout::kTreeBase, 0, MakeTreeNodeImage(0, tree)));
}

void ParseArgs(const core::QueryArguments &arguments, std::string &region_name,
               int32_t &start_days) {
  region_name.clear();
  start_days = 0;
  if (arguments.values.size() >= 1 && !arguments.values[0].is_null) {
    if (std::holds_alternative<std::string>(arguments.values[0].payload))
      region_name = std::get<std::string>(arguments.values[0].payload);
  }
  if (arguments.values.size() >= 2 && !arguments.values[1].is_null) {
    if (std::holds_alternative<core::AuxDate>(arguments.values[1].payload))
      start_days =
          std::get<core::AuxDate>(arguments.values[1].payload).days_since_epoch;
    else if (std::holds_alternative<int64_t>(arguments.values[1].payload))
      start_days =
          static_cast<int32_t>(std::get<int64_t>(arguments.values[1].payload));
  }
}

// acc[0..n) += row[0..n) (AVX-512 / AVX2 when available).
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

// Sums the covered tree nodes into revenue-by-nationkey (zero columns skipped).
std::unordered_map<int32_t, int64_t>
SumCover(const TpchQ5Meta &meta, const TpchQ5Tree &tree,
         const std::vector<uint32_t> &nodes) {
  std::unordered_map<int32_t, int64_t> out;
  if (tree.stride == 0)
    return out;
  std::vector<int64_t> acc(tree.stride, 0);
  for (const auto node : nodes) {
    if (!tree.Valid(node))
      continue;
    const int64_t *row = tree.Node(node);
    for (uint32_t c = 0; c < tree.stride; ++c)
      acc[c] += row[c];
  }
  for (uint32_t c = 0; c < tree.stride && c < meta.nation_keys.size(); ++c)
    if (acc[c] != 0)
      out[meta.nation_keys[c]] = acc[c];
  return out;
}

// Applies a commit's order, lineitem, customer and supplier deltas to
// copy-on-write transactions over the published side indexes and tree. Stage()
// returns false when the published state cannot take them (the caller
// rebuilds).
class IncrementalStager {
public:
  using Kind = TpchQ5TxnBuffer::DimDelta::Kind;

  IncrementalStager(core::AuxEngineServices &services, TpchQ5InitialScan *initial_scan,
                    const TpchQ5TxnBuffer &buffer, std::shared_ptr<const TpchQ5State> side,
                    const TpchQ5Tree &tree)
      : services_(services), initial_scan_(initial_scan), buffer_(buffer),
        side_(std::move(side)), meta_(*side_->meta), otx_(side_->orders),
        stx_(side_->supps), ctx_(side_->custs), ttx_(tree) {}

  bool Stage(core::StagedMaintenanceOutcome &outcome) {
    if (!AddCustomers() || !ReplaceUpdatedOrders() || !InsertOrders() ||
        !ApplyLineitems())
      return false;
    DeleteOrders();
    if (!DeleteCustomers() || !ApplySupplierChanges())
      return false;
    Publish(outcome);
    return true;
  }

private:
  // Adds `rev` to (leaf of ordinal, nation column) and to the order's record.
  void CreditOrder(TpchQ5OrderRec &orec, int64_t rev) {
    const int32_t col = meta_.NationColumn(orec.nationkey);
    if (col < 0 || rev == 0)
      return;
    orec.rev += rev;
    ttx_.Add(TpchQ5LeafNode(meta_.tree_size, orec.ordinal), col, rev);
  }

  void DetachRevenue(const TpchQ5OrderRec &orec) {
    const int32_t col = meta_.NationColumn(orec.nationkey);
    if (col >= 0 && orec.rev != 0)
      ttx_.Add(TpchQ5LeafNode(meta_.tree_size, orec.ordinal), col, -orec.rev);
  }

  // Adds `delta` to the customer's count of orders.
  void AdjustOrderCount(int64_t custkey, int32_t delta) {
    if (const auto *cust = TpchQ5Val(ctx_.Find(custkey)))
      ctx_.Upsert({custkey, TpchQ5CustRec{cust->nationkey, cust->order_count + delta}});
  }

  // Contribution of an order's current lineitems under its new customer.
  bool RecomputeRevenue(int64_t orderkey, int32_t nationkey, int64_t &rev) {
    rev = 0;
    return initial_scan_ &&
           initial_scan_->ScanLineitemsByOrder(
               orderkey, [&](const TpchQ5LineitemInput &li) {
                 const auto *supp = TpchQ5Val(stx_.Find(li.suppkey));
                 if (supp && supp->present && supp->nationkey == nationkey)
                   rev += ComputeRevenueRaw(li.extended_price_raw, li.discount_raw);
               });
  }

  // Phase A: customer inserts and nation changes. A nation change (or
  // removal) of a customer that existing orders reference would re-attribute
  // their revenue: rebuild in that (rare) case.
  bool AddCustomers() {
    for (const auto &d : buffer_.CustomerMutations()) {
      if (d.kind == Kind::Insert) {
        if (TpchQ5Val(ctx_.Find(d.key)))
          return false;
        ctx_.Upsert({d.key, TpchQ5CustRec{d.nationkey, 0}});
      } else if (d.kind == Kind::UpdateNation) {
        const auto *rec = TpchQ5Val(ctx_.Find(d.key));
        if (!rec || rec->order_count > 0)
          return false;
        ctx_.Upsert({d.key, TpchQ5CustRec{d.nationkey, 0}});
      }
    }
    return true;
  }

  // Phase A2: order UPDATEs (same key removed and inserted).
  bool ReplaceUpdatedOrders() {
    std::unordered_set<int64_t> inserted;
    for (const auto &o : buffer_.OrderMutations())
      if (o.sign > 0)
        inserted.insert(o.orderkey);
    for (const auto &o : buffer_.OrderMutations())
      if (o.sign < 0 && inserted.count(o.orderkey))
        updated_.insert(o.orderkey);

    for (const auto key : updated_) {
      const auto *found = TpchQ5Val(otx_.Find(key));
      const TpchQ5TxnBuffer::OrderDelta *nd = nullptr;
      for (const auto &o : buffer_.OrderMutations())
        if (o.sign > 0 && o.orderkey == key)
          nd = &o;
      TpchQ5OrderRec old_rec;
      const bool had = found != nullptr;
      if (had) {
        old_rec = *found;
        DetachRevenue(old_rec);
        AdjustOrderCount(old_rec.custkey, -1);
        otx_.Erase(key);
      }
      const auto *cust = TpchQ5Val(ctx_.Find(nd->custkey));
      if (!cust) {
        dropped_.insert(key); // customer outside the join
        continue;
      }
      const int32_t ord = TpchQ5DateOrdinal(nd->orderdate_days, meta_.min_orderdate_days);
      if (ord < 0 || ord >= static_cast<int32_t>(meta_.tree_size))
        return false;
      TpchQ5OrderRec nr{cust->nationkey, ord, nd->custkey, 0};
      if (had && old_rec.nationkey == cust->nationkey) {
        // Same nation: the existing revenue just moves (date / customer).
        CreditOrder(nr, old_rec.rev);
      } else {
        int64_t rev = 0;
        if (!RecomputeRevenue(key, cust->nationkey, rev))
          return false;
        CreditOrder(nr, rev);
        recomputed_.insert(key);
      }
      ctx_.Upsert({nd->custkey, TpchQ5CustRec{cust->nationkey, cust->order_count + 1}});
      otx_.Upsert({key, nr});
    }
    return true;
  }

  // Phase A3: plain order inserts. Orders of unknown customers contribute
  // nothing and are not tracked (as in BuildState).
  bool InsertOrders() {
    for (const auto &order : buffer_.OrderMutations()) {
      if (order.sign < 0 || updated_.count(order.orderkey))
        continue;
      const auto *cust = TpchQ5Val(ctx_.Find(order.custkey));
      if (!cust)
        continue;
      const int32_t ord = TpchQ5DateOrdinal(order.orderdate_days, meta_.min_orderdate_days);
      if (ord < 0 || ord >= static_cast<int32_t>(meta_.tree_size))
        return false; // outside the tree's date range: re-derive bounds
      if (TpchQ5Val(otx_.Find(order.orderkey)))
        return false;
      otx_.Upsert({order.orderkey, TpchQ5OrderRec{cust->nationkey, ord, order.custkey, 0}});
      ctx_.Upsert({order.custkey, TpchQ5CustRec{cust->nationkey, cust->order_count + 1}});
    }
    return true;
  }

  // Phase B: lineitems. Credit a nation only when supplier nation equals
  // customer nation (same filter as BuildState / the Q5 join). A lineitem of
  // an absent supplier only adjusts the supplier's reference count. False when
  // a lineitem's order is not tracked.
  bool ApplyLineitem(const TpchQ5LineitemInput &row, int64_t sign) {
    const auto *supp = TpchQ5Val(stx_.Find(row.suppkey));
    TpchQ5SuppRec srec = supp ? *supp : TpchQ5SuppRec{0, 0, false};
    srec.line_count += static_cast<int32_t>(sign);
    stx_.Upsert({row.suppkey, srec});
    if (dropped_.count(row.orderkey) || recomputed_.count(row.orderkey))
      return true;
    const auto *order = TpchQ5Val(otx_.Find(row.orderkey));
    if (!order)
      return false;
    if (!srec.present || srec.nationkey != order->nationkey)
      return true;
    TpchQ5OrderRec orec = *order;
    CreditOrder(orec, sign * ComputeRevenueRaw(row.extended_price_raw, row.discount_raw));
    otx_.Upsert({row.orderkey, orec});
    return true;
  }

  bool ApplyLineitems() {
    for (const auto &m : buffer_.LineitemMutations()) {
      if (!m.is_insert && !ApplyLineitem(m.old_row, -1))
        return false;
      if (!m.is_delete && !ApplyLineitem(m.new_row, 1))
        return false;
    }
    return true;
  }

  // Phase C: order deletes subtract whatever the order still contributes.
  void DeleteOrders() {
    for (const auto &order : buffer_.OrderMutations()) {
      if (order.sign > 0 || updated_.count(order.orderkey))
        continue;
      const auto *rec = TpchQ5Val(otx_.Find(order.orderkey));
      if (!rec)
        continue;
      const TpchQ5OrderRec orec = *rec;
      DetachRevenue(orec);
      AdjustOrderCount(orec.custkey, -1);
      otx_.Erase(order.orderkey);
    }
  }

  // Phase D: customer deletes (only when nothing references them).
  bool DeleteCustomers() {
    for (const auto &d : buffer_.CustomerMutations()) {
      if (d.kind != Kind::Delete)
        continue;
      const auto *rec = TpchQ5Val(ctx_.Find(d.key));
      if (!rec || rec->order_count > 0)
        return false;
      ctx_.Erase(d.key);
    }
    return true;
  }

  // Phase E: moves one supplier between nations against the post-write
  // lineitem table: the supplier's lineitems are fetched with one point query
  // and each matching contribution is removed / added through the order map.
  // (A key with no lineitems needs no query.)
  bool AdjustSupplier(int64_t suppkey, bool has_old, int32_t old_nation, bool has_new,
                      int32_t new_nation) {
    const auto *rec = TpchQ5Val(stx_.Find(suppkey));
    const int32_t lines = rec ? rec->line_count : 0;
    if (lines > 0) {
      std::vector<TpchQ5LineitemInput> rows;
      if (!initial_scan_ ||
          !initial_scan_->ScanLineitemsBySupplier(
              suppkey, [&](const TpchQ5LineitemInput &li) { rows.push_back(li); }))
        return false;
      for (const auto &li : rows) {
        const auto *order = TpchQ5Val(otx_.Find(li.orderkey));
        if (!order)
          continue; // no tracked order: contributed nothing
        TpchQ5OrderRec orec = *order;
        const int64_t rev = ComputeRevenueRaw(li.extended_price_raw, li.discount_raw);
        if (has_old && orec.nationkey == old_nation)
          CreditOrder(orec, -rev);
        if (has_new && orec.nationkey == new_nation)
          CreditOrder(orec, rev);
        otx_.Upsert({li.orderkey, orec});
      }
    }
    if (has_new)
      stx_.Upsert({suppkey, TpchQ5SuppRec{new_nation, lines, true}});
    else if (lines > 0)
      stx_.Upsert({suppkey, TpchQ5SuppRec{0, lines, false}});
    else
      stx_.Erase(suppkey);
    return true;
  }

  // Supplier changes, in arrival order.
  bool ApplySupplierChanges() {
    for (const auto &d : buffer_.SupplierMutations()) {
      const auto *rec = TpchQ5Val(stx_.Find(d.key));
      if (d.kind == Kind::Insert) {
        if (rec && rec->present)
          return false; // duplicate key
        if (!AdjustSupplier(d.key, false, 0, true, d.nationkey))
          return false;
      } else {
        if (!rec || !rec->present)
          return false;
        const bool moves = d.kind != Kind::Delete;
        if (!AdjustSupplier(d.key, true, rec->nationkey, moves, moves ? d.nationkey : 0))
          return false;
      }
    }
    return true;
  }

  void Publish(core::StagedMaintenanceOutcome &outcome) {
    const bool side_dirty = otx_.Dirty() || stx_.Dirty() || ctx_.Dirty();
    if (!side_dirty && !ttx_.Dirty())
      return;
    outcome.action = core::MaintenanceAction::ApplyDelta;
    if (side_dirty) {
      auto new_side = std::make_shared<TpchQ5State>();
      new_side->meta = side_->meta;
      const auto for_each_mid = [&](std::size_t n, const auto &fn) {
        core::ParallelEach(services_, n, kFinishGrain, fn);
      };
      new_side->orders = otx_.Finish(for_each_mid);
      new_side->supps = stx_.Finish(for_each_mid);
      new_side->custs = ctx_.Finish(for_each_mid);
      outcome.updates.push_back(core::StateImage::Make(
          TpchQ5Module::kId, core::TpchQ5Layout::kSide, 0,
          MakeSideImage(std::shared_ptr<const TpchQ5State>(std::move(new_side)))));
    }
    if (ttx_.Dirty()) {
      auto new_tree = std::make_shared<const TpchQ5Tree>(ttx_.Finish());
      outcome.updates.push_back(core::StateImage::Make(
          TpchQ5Module::kId, core::TpchQ5Layout::kTreeBase, 0,
          MakeTreeNodeImage(0, new_tree)));
    }
  }

  core::AuxEngineServices &services_;
  TpchQ5InitialScan *initial_scan_;
  const TpchQ5TxnBuffer &buffer_;
  std::shared_ptr<const TpchQ5State> side_;
  const TpchQ5Meta &meta_;
  TpchQ5ShardIndex<TpchQ5OrderRec>::Txn otx_;
  TpchQ5ShardIndex<TpchQ5SuppRec>::Txn stx_;
  TpchQ5ShardIndex<TpchQ5CustRec>::Txn ctx_;
  Q5TreeTxn ttx_;
  // Orders whose lineitem contribution was recomputed from the post-write
  // table (their lineitem deltas must not be applied again), orders dropped by
  // an update (lineitem deltas ignored), and keys both deleted and inserted.
  std::unordered_set<int64_t> recomputed_, dropped_, updated_;
};

} // namespace

// TxnBuffer

bool TpchQ5TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ5TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  lineitem_mutations.clear();
  order_mutations.clear();
  customer_mutations.clear();
  supplier_mutations.clear();
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

void TpchQ5TxnBuffer::AddCustomerMutation(const DimDelta &d) {
  pending = true;
  customer_mutations.push_back(d);
}
void TpchQ5TxnBuffer::AddSupplierMutation(const DimDelta &d) {
  pending = true;
  supplier_mutations.push_back(d);
}
const std::vector<TpchQ5TxnBuffer::DimDelta> &
TpchQ5TxnBuffer::CustomerMutations() const {
  return customer_mutations;
}
const std::vector<TpchQ5TxnBuffer::DimDelta> &
TpchQ5TxnBuffer::SupplierMutations() const {
  return supplier_mutations;
}
void TpchQ5TxnBuffer::MarkNoop() { pending = true; }
bool TpchQ5TxnBuffer::HasAnyDelta() const {
  return !lineitem_mutations.empty() || !order_mutations.empty() ||
         !customer_mutations.empty() || !supplier_mutations.empty();
}



int32_t TpchQ5AddMonths(int32_t days_since_epoch, int32_t months) {
  return static_cast<int32_t>(core::sql_date::AddMonths(days_since_epoch, months));
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
  std::vector<core::StateImage> images;
  AppendStateImages(std::move(state), images);
  for (auto &image : images)
    builder.Write(std::move(image));
  builder.Finish();
}

core::InvalidationPlan
TpchQ5Module::InterpretMutation(const core::MutationBatch &mutation) const {
  // Lineitem/orders/customer/supplier mutations are all applied precisely by
  // StageIncrementalMaintenance (or MarkRebuild from AccumulateMutation when
  // that is impossible), so they never need a blanket invalidation. Only the
  // nation/region dimension tables do.
  if (mutation.table == config.nation_table ||
      mutation.table == config.region_table)
    return core::AllPartitionsPlan();
  return {};
}

void TpchQ5Module::AccumulateMutation(const core::MutationBatch &mutation,
                                      core::AuxEngineServices &,
                                      core::AuxTxnBuffer &buf) {
  auto &buffer = dynamic_cast<TpchQ5TxnBuffer &>(buf);

  if (mutation.table == config.nation_table ||
      mutation.table == config.region_table) {
    buffer.MarkRebuild();
    return;
  }

  if (mutation.table == config.customer_table ||
      mutation.table == config.supplier_table) {
    const bool is_customer = mutation.table == config.customer_table;
    auto add = [&](const TpchQ5TxnBuffer::DimDelta &d) {
      if (is_customer)
        buffer.AddCustomerMutation(d);
      else
        buffer.AddSupplierMutation(d);
    };
    auto read = [&](const core::AuxRow &row, int64_t &key,
                    int32_t &nation) -> bool {
      return core::IntCell(row, 0, key) && core::Int32Cell(row, 1, nation);
    };
    using Kind = TpchQ5TxnBuffer::DimDelta::Kind;
    switch (mutation.kind) {
    case core::MutationKind::Insert:
    case core::MutationKind::Delete: {
      const bool ins = mutation.kind == core::MutationKind::Insert;
      const auto &rows = ins ? mutation.new_rows : mutation.old_rows;
      for (const auto &row : rows) {
        TpchQ5TxnBuffer::DimDelta d;
        if (!read(row, d.key, d.nationkey)) {
          buffer.MarkRebuild();
          return;
        }
        d.kind = ins ? Kind::Insert : Kind::Delete;
        add(d);
      }
      break;
    }
    case core::MutationKind::Update:
      if (mutation.old_rows.size() != mutation.new_rows.size()) {
        buffer.MarkRebuild();
        return;
      }
      for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
        int64_t ok = 0, nk = 0;
        int32_t on = 0, nn = 0;
        if (!read(mutation.old_rows[i], ok, on) ||
            !read(mutation.new_rows[i], nk, nn)) {
          buffer.MarkRebuild();
          return;
        }
        if (ok == nk) {
          if (on == nn) {
            buffer.MarkNoop();
          } else {
            TpchQ5TxnBuffer::DimDelta d;
            d.kind = Kind::UpdateNation;
            d.key = ok;
            d.old_nationkey = on;
            d.nationkey = nn;
            add(d);
          }
        } else {
          TpchQ5TxnBuffer::DimDelta del, ins;
          del.kind = Kind::Delete;
          del.key = ok;
          del.nationkey = on;
          ins.kind = Kind::Insert;
          ins.key = nk;
          ins.nationkey = nn;
          add(del);
          add(ins);
        }
      }
      break;
    }
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
        // Q5 reads only (orderkey, custkey, orderdate): an update of other
        // columns leaves the state unchanged.
        TpchQ5TxnBuffer::OrderDelta o, n;
        if (ReadOrder(mutation.old_rows[i], o.orderkey, o.custkey,
                      o.orderdate_days) &&
            ReadOrder(mutation.new_rows[i], n.orderkey, n.custkey,
                      n.orderdate_days) &&
            o.orderkey == n.orderkey && o.custkey == n.custkey &&
            o.orderdate_days == n.orderdate_days) {
          buffer.MarkNoop();
          continue;
        }
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

  // Incremental maintenance for lineitem/orders RF writes, price updates and
  // customer/supplier dimension writes.
  if (!buffer.RequiresRebuild() && buffer.HasAnyDelta()) {
    auto inc_outcome = StageIncrementalMaintenance(services, buffer);
    if (inc_outcome.action == core::MaintenanceAction::ApplyDelta) {
      // The updated partitions are the whole change; no blanket invalidation.
      return inc_outcome;
    }
    if (inc_outcome.action != core::MaintenanceAction::NoOp) {
      inc_outcome.invalidations = plan;
      return inc_outcome;
    }
    // Incremental resolved the write as not touching the state.
    return outcome;
  }

  // Nothing to do if plan is NoOp and no rebuild pending
  if (!buffer.RequiresRebuild() &&
      plan.scope == ::accelerator_codegen::aux_state::InvalidationScope::None) {
    return outcome;
  }

  // Fallback: full rebuild
  outcome.action = core::MaintenanceAction::RebuildAll;
  AppendStateImages(BuildState(services), outcome.updates);
  outcome.invalidations = plan;
  return outcome;
}

core::StagedMaintenanceOutcome
TpchQ5Module::StageIncrementalMaintenance(core::AuxEngineServices &services,
                                          const TpchQ5TxnBuffer &buffer) {
  core::StagedMaintenanceOutcome outcome;
  outcome.action = core::MaintenanceAction::NoOp;
  const auto rebuild = [&] {
    outcome.action = core::MaintenanceAction::RebuildAll;
    outcome.updates.clear();
    return outcome;
  };

  std::shared_ptr<const TpchQ5State> side;
  std::shared_ptr<const TpchQ5Tree> tree;
  if (!LoadSnapshot(services, side, tree))
    return rebuild();

  // RF-sized batches are always applied incrementally; the cost model is only
  // consulted for bulk changes.
  const auto total_deltas = buffer.LineitemMutations().size() +
                            buffer.OrderMutations().size() +
                            buffer.CustomerMutations().size() +
                            buffer.SupplierMutations().size();
  if (total_deltas > kBulkDeltaCount) {
    auto request = core::MaintenanceRequest::IncrementalSingleState(
        total_deltas, services.ExpectedQueriesBeforeNextWrite());
    request.affected_partition_count = 2; // side index and tree
    if (services.ChooseMaintenance(request).action != core::MaintenanceAction::ApplyDelta)
      return rebuild();
  }

  IncrementalStager stager(services, initial_scan, buffer, side, *tree);
  if (!stager.Stage(outcome))
    return rebuild();
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
  ParseArgs(arguments, region_name, start_days);
  const int32_t end_days = TpchQ5AddMonths(start_days, kWindowMonths);

  std::shared_ptr<const TpchQ5State> side;
  std::shared_ptr<const TpchQ5Tree> tree;
  if (!LoadSnapshot(services, side, tree)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  const auto &meta = *side->meta;

  if (region_name.empty()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  // The state holds every order, so a region with no nations or a date range
  // past the tree's has no revenue: answer from the state, not SQL.
  std::vector<int32_t> eligible_nations;
  int32_t target_regionkey = ResolveRegion(region_name, meta, eligible_nations);
  if (target_regionkey < 0 || eligible_nations.empty()) {
    EmitResult(services, meta, target_regionkey, {}, sink);
    return;
  }

  const int32_t start_ord = std::max<int32_t>(
      0, TpchQ5DateOrdinal(start_days, meta.min_orderdate_days));
  const int32_t end_ord =
      std::min<int32_t>(static_cast<int32_t>(meta.tree_size),
                        TpchQ5DateOrdinal(end_days, meta.min_orderdate_days));
  if (start_ord >= end_ord) {
    EmitResult(services, meta, target_regionkey, {}, sink);
    return;
  }

  // Canonical cover on the stack, then sum the covered node rows (one
  // contiguous stride-sized row per node) and emit straight from the columns.
  uint32_t nodes[96];
  std::size_t node_count = 0;
  {
    int32_t l = start_ord + static_cast<int32_t>(meta.tree_size);
    int32_t r = end_ord + static_cast<int32_t>(meta.tree_size);
    while (l < r && node_count + 2 <= 96) {
      if (l & 1)
        nodes[node_count++] = static_cast<uint32_t>(l++);
      if (r & 1)
        nodes[node_count++] = static_cast<uint32_t>(--r);
      l >>= 1;
      r >>= 1;
    }
  }
  const uint32_t stride = tree->stride;
  if (stride == 0 || stride > kQ5MaxStride) {
    const std::vector<uint32_t> cover(nodes, nodes + node_count);
    EmitResult(services, meta, target_regionkey, SumCover(meta, *tree, cover),
               sink);
    return;
  }
  alignas(64) int64_t acc[kQ5MaxStride] = {};
  for (std::size_t i = 0; i < node_count; ++i) {
    if (!tree->Valid(nodes[i]))
      continue;
    AddRow(acc, tree->Node(nodes[i]), stride);
  }
  TpchQ5FinalizeColumns(meta, target_regionkey, acc, stride, sink);
}

TpchQ5State TpchQ5Module::BuildState(core::AuxEngineServices &services) const {
  TpchQ5State state;

  if (!initial_scan)
    throw std::logic_error("TPC-H Q5 requires a typed initial scan source");

  auto meta_ptr = std::make_shared<TpchQ5Meta>();
  auto &meta = *meta_ptr;

  initial_scan->ScanRegionsSerial([&](const TpchQ5RegionInput &input) {
    meta.region_name_to_key[input.name] = input.key;
  });

  initial_scan->ScanNationsSerial([&](const TpchQ5NationInput &input) {
    meta.nation_name[input.key] = input.name;
    meta.nation_region[input.key] = input.regionkey;
    meta.region_nations[input.regionkey].push_back(input.key);
    if (!meta.nation_index.count(input.key)) {
      meta.nation_index[input.key] =
          static_cast<int32_t>(meta.nation_keys.size());
      meta.nation_keys.push_back(input.key);
    }
  });
  const uint32_t stride = static_cast<uint32_t>(meta.nation_keys.size());
  meta.column_region.assign(stride, -1);
  meta.column_name.assign(stride, std::string());
  for (uint32_t c = 0; c < stride; ++c) {
    const int32_t nk = meta.nation_keys[c];
    const auto rit = meta.nation_region.find(nk);
    if (rit != meta.nation_region.end())
      meta.column_region[c] = rit->second;
    const auto nit = meta.nation_name.find(nk);
    if (nit != meta.nation_name.end())
      meta.column_name[c] = nit->second;
  }

  // Dimension and order lookups live in hash-sharded sorted vectors (no
  // 30M-entry node maps); they become the published persistent indexes.
  auto custs = BuildDimension<TpchQ5CustRec>(services, *initial_scan, "customer",
                                             "c_custkey", "c_nationkey");
  auto supps = BuildDimension<TpchQ5SuppRec>(services, *initial_scan, "supplier",
                                             "s_suppkey", "s_nationkey");

  // Orders: one piece of rows per scan partition; each order counts toward its
  // customer (the customer index is sorted and fixed, so partitions add to the
  // counters atomically).
  using scan::DecimalColumn;
  using scan::DateColumn;
  using scan::IntColumn;
  auto scanned = core::PartitionedBuild<OrdersPartial>(
      services, [] { return OrdersPartial{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanTable(services, "orders",
                                {IntColumn("o_orderkey"), IntColumn("o_custkey"),
                                 DateColumn("o_orderdate")},
                                parts, sink);
      },
      [&](OrdersPartial &p, const auto &row) {
        const int32_t date = row.Date(2);
        p.min_orderdate_days = std::min(p.min_orderdate_days, date);
        p.max_orderdate_days = std::max(p.max_orderdate_days, date);
        auto *cust = TpchQ5ValMut(custs.Find(row.Int(1)));
        if (!cust)
          return;
        AtomicAdd<int32_t>(cust->order_count, 1);
        // `ordinal` temporarily holds the order date; rebased below.
        p.rows.Add({row.Int(0), TpchQ5OrderRec{cust->nationkey, date, row.Int(1), 0}});
      },
      [](OrdersPartial &into, OrdersPartial &&from) {
        into.min_orderdate_days = std::min(into.min_orderdate_days, from.min_orderdate_days);
        into.max_orderdate_days = std::max(into.max_orderdate_days, from.max_orderdate_days);
        core::RowPieces<TpchQ5ShardIndex<TpchQ5OrderRec>::Element>::Merge(into.rows,
                                                                          std::move(from.rows));
      });
  meta.min_orderdate_days = scanned.min_orderdate_days;
  meta.max_orderdate_days = scanned.max_orderdate_days;

  if (meta.min_orderdate_days > meta.max_orderdate_days) {
    meta.min_orderdate_days = 0;
    meta.max_orderdate_days = 0;
  }

  int32_t num_days = meta.max_orderdate_days - meta.min_orderdate_days + 1;
  if (num_days < 1)
    num_days = 1;
  meta.tree_size = core::NextPowerOf2(static_cast<uint32_t>(num_days));
  const uint32_t tree_size = meta.tree_size;
  auto &order_pieces = scanned.rows.pieces;
  core::ParallelEach(services, order_pieces.size(), 1, [&](std::size_t p) {
    for (auto &e : order_pieces[p])
      e.value.ordinal = TpchQ5DateOrdinal(e.value.ordinal, meta.min_orderdate_days);
  });
  TpchQ5ShardIndex<TpchQ5OrderRec>::Builder orders;
  orders.AddPieces(order_pieces, core::Parallelism::OnPool(services));
  SortShards(services, orders);

  // Lineitems: the supplier and order counters are shared and added atomically;
  // an order's revenue lands in the tree afterwards.
  auto orphan_supp = core::PartitionedBuild<OrphanSuppliers>(
      services, [] { return OrphanSuppliers{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanTable(services, "lineitem",
                                {IntColumn("l_orderkey"), IntColumn("l_suppkey"),
                                 DecimalColumn("l_extendedprice", 2),
                                 DecimalColumn("l_discount", 2)},
                                parts, sink);
      },
      [&](OrphanSuppliers &orphans, const auto &row) {
        auto *supp = TpchQ5ValMut(supps.Find(row.Int(1)));
        if (!supp) {
          ++orphans[row.Int(1)]; // lineitem of a supplier that does not exist
          return;
        }
        AtomicAdd<int32_t>(supp->line_count, 1);
        auto *order = TpchQ5ValMut(orders.Find(row.Int(0)));
        if (!order || supp->nationkey != order->nationkey)
          return;
        const int32_t col = meta.NationColumn(order->nationkey);
        if (col < 0 || order->ordinal < 0 ||
            order->ordinal >= static_cast<int32_t>(tree_size))
          return;
        AtomicAdd<int64_t>(order->rev, ComputeRevenueRaw(row.Decimal(2), row.Decimal(3)));
      },
      [](OrphanSuppliers &into, OrphanSuppliers &&from) {
        for (const auto &entry : from)
          into[entry.first] += entry.second;
      });

  if (!orphan_supp.empty()) {
    for (const auto &entry : orphan_supp)
      supps.Add({entry.first, TpchQ5SuppRec{0, entry.second, false}});
    SortShards(services, supps);
  }

  // Tree leaves: slices of order shards sum into private trees, added in parallel.
  const std::size_t tree_cells = static_cast<std::size_t>(tree_size) * 2 * stride;
  std::vector<std::vector<int64_t>> trees(
      core::SliceCount(services, orders.shard_count(), kShardGrain));
  core::ParallelSlices(
      services, orders.shard_count(), kShardGrain,
      [&](std::size_t slice, std::size_t begin, std::size_t end) {
        auto &leaves = trees[slice];
        leaves.assign(tree_cells, 0);
        orders.ForEachInShards(begin, end, [&](const TpchQ5ShardIndex<TpchQ5OrderRec>::Element &e) {
          const auto &order = e.value;
          const int32_t col = meta.NationColumn(order.nationkey);
          if (order.rev == 0 || col < 0 || order.ordinal < 0 ||
              order.ordinal >= static_cast<int32_t>(tree_size))
            return;
          leaves[static_cast<std::size_t>(TpchQ5LeafNode(tree_size, order.ordinal)) * stride +
                 static_cast<uint32_t>(col)] += order.rev;
        });
      });
  core::SumInto(core::Parallelism::OnPool(services), trees);
  std::vector<int64_t> flat = std::move(trees[0]);

  for (uint32_t i = tree_size - 1; i > 0; i--)
    for (uint32_t c = 0; c < stride; ++c)
      flat[static_cast<std::size_t>(i) * stride + c] =
          flat[static_cast<std::size_t>(i * 2) * stride + c] +
          flat[static_cast<std::size_t>(i * 2 + 1) * stride + c];

  state.tree = PaginateTree(flat, tree_size, stride);
  state.orders = FinishShards(services, orders);
  state.supps = FinishShards(services, supps);
  state.custs = FinishShards(services, custs);
  state.meta = std::move(meta_ptr);
  return state;
}

bool TpchQ5Module::LoadSnapshot(
    core::AuxEngineServices &services,
    std::shared_ptr<const TpchQ5State> &side,
    std::shared_ptr<const TpchQ5Tree> &tree) const {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(),
      {core::TpchQ5Layout::kSide, core::TpchQ5Layout::kTreeBase});
  for (const auto &image : snapshot.partitions) {
    const auto *typed = AsQ5Image(image);
    if (!typed)
      return false;
    if (image.partition == core::TpchQ5Layout::kSide) {
      if (!typed->is_side || !typed->state || !typed->state->meta)
        return false;
      side = typed->state;
    } else if (image.partition == core::TpchQ5Layout::kTreeBase) {
      if (typed->is_side || !typed->tree)
        return false;
      tree = typed->tree;
    }
  }
  return side && tree && tree->tree_size == side->meta->tree_size;
}

int32_t TpchQ5Module::ResolveRegion(
    const std::string &region_name, const TpchQ5Meta &meta,
    std::vector<int32_t> &eligible_nations) const {
  auto it = meta.region_name_to_key.find(region_name);
  if (it == meta.region_name_to_key.end())
    return -1;
  int32_t rkey = it->second;
  auto nat_it = meta.region_nations.find(rkey);
  if (nat_it != meta.region_nations.end())
    eligible_nations = nat_it->second;
  return rkey;
}

void TpchQ5Module::EmitResult(
    core::AuxEngineServices &, const TpchQ5Meta &meta,
    int32_t target_regionkey,
    const std::unordered_map<int32_t, int64_t> &revenue_by_nation,
    core::AuxRowSink &sink) const {
  TpchQ5FinalizeAndEmit(meta.nation_name, meta.nation_region,
                        target_regionkey, revenue_by_nation, sink);
}

std::vector<core::CalibrationWrite> TpchQ5Module::CalibrationWrites() const {
  return TpchQ5CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
