#include "tpch/q5/duckdb/aux/module.hpp"
#if defined(__AVX2__) || defined(__AVX512F__)
#include <immintrin.h>
#endif
#include "tpch/q5/calibration_writes.hpp"
#include "accelerator_codegen/stateful/core/byte_stream.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include "tpch/q5/layout.hpp"


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

namespace {

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

constexpr uint32_t kQ5MaxStride = 64;

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

} // namespace

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
    return EmptyPlan();
  return NoOpPlan();
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
      return IntCell(row, 0, key) && Int32Cell(row, 1, nation);
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
    auto inc_outcome = StageIncrementalMaintenance(services, plan, buffer);
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
TpchQ5Module::StageIncrementalMaintenance(
    core::AuxEngineServices &services, const core::InvalidationPlan &,
    TpchQ5TxnBuffer &buffer) {
  core::StagedMaintenanceOutcome outcome;
  outcome.action = core::MaintenanceAction::NoOp;
  auto rebuild = [&]() {
    outcome.action = core::MaintenanceAction::RebuildAll;
    outcome.updates.clear();
    return outcome;
  };

  std::shared_ptr<const TpchQ5State> side;
  std::shared_ptr<const TpchQ5Tree> tree;
  if (!LoadSnapshot(services, side, tree))
    return rebuild();
  const auto &meta = *side->meta;

  // RF-sized batches are always applied incrementally; the cost model is only
  // consulted for bulk changes.
  const auto total_deltas = buffer.LineitemMutations().size() +
                            buffer.OrderMutations().size() +
                            buffer.CustomerMutations().size() +
                            buffer.SupplierMutations().size();
  if (total_deltas > 256) {
    const auto decision = services.ChooseMaintenance(
        {static_cast<uint64_t>(total_deltas), 2, true, false, true, false,
         false, core::MaintenanceAction::NoOp,
         services.ExpectedQueriesBeforeNextWrite()});
    if (decision.action != core::MaintenanceAction::ApplyDelta)
      return rebuild();
  }

  TpchQ5ShardTxn<TpchQ5OrderRec> otx(side->orders);
  TpchQ5ShardTxn<TpchQ5SuppRec> stx(side->supps);
  TpchQ5ShardTxn<TpchQ5CustRec> ctx(side->custs);
  Q5TreeTxn ttx(*tree);
  using Kind = TpchQ5TxnBuffer::DimDelta::Kind;

  // Adds `rev` to (leaf of ordinal, nation column) and to the order's record.
  auto credit_order = [&](TpchQ5OrderRec &orec, int64_t rev) {
    const int32_t col = meta.NationColumn(orec.nationkey);
    if (col < 0 || rev == 0)
      return;
    orec.rev += rev;
    ttx.Add(TpchQ5LeafNode(meta.tree_size, orec.ordinal), col, rev);
  };

  // Phase A: customer inserts and nation changes. A nation change (or
  // removal) of a customer that existing orders reference would re-attribute
  // their revenue: rebuild in that (rare) case.
  for (const auto &d : buffer.CustomerMutations()) {
    if (d.kind == Kind::Insert) {
      if (ctx.Find(d.key))
        return rebuild();
      ctx.Set(d.key, TpchQ5CustRec{d.nationkey, 0});
    } else if (d.kind == Kind::UpdateNation) {
      const auto *rec = ctx.Find(d.key);
      if (!rec || rec->order_count > 0)
        return rebuild();
      ctx.Set(d.key, TpchQ5CustRec{d.nationkey, 0});
    }
  }

  // Orders whose lineitem contribution was recomputed from the post-write
  // table (their lineitem deltas must not be applied again), and orders
  // dropped by an update (lineitem deltas ignored).
  std::unordered_set<int64_t> recomputed, dropped;
  std::unordered_set<int64_t> updated; // keys both deleted and inserted
  {
    std::unordered_set<int64_t> ins;
    for (const auto &o : buffer.OrderMutations())
      if (o.sign > 0)
        ins.insert(o.orderkey);
    for (const auto &o : buffer.OrderMutations())
      if (o.sign < 0 && ins.count(o.orderkey))
        updated.insert(o.orderkey);
  }

  // Contribution of an order's current lineitems under its new customer.
  auto recompute_rev = [&](int64_t orderkey, int32_t nationkey,
                           bool &ok) -> int64_t {
    int64_t rev = 0;
    ok = initial_scan &&
         initial_scan->ScanLineitemsByOrder(
             orderkey, [&](const TpchQ5LineitemInput &li) {
               const auto *supp = stx.Find(li.suppkey);
               if (supp && supp->present && supp->nationkey == nationkey)
                 rev += ComputeRevenueRaw(li.extended_price_raw, li.discount_raw);
             });
    return rev;
  };

  // Phase A2: order UPDATEs (same key removed and inserted).
  for (const auto key64 : updated) {
    const auto *found = otx.Find(key64);
    const TpchQ5TxnBuffer::OrderDelta *nd = nullptr;
    for (const auto &o : buffer.OrderMutations())
      if (o.sign > 0 && o.orderkey == key64)
        nd = &o;
    TpchQ5OrderRec old_rec;
    const bool had = found != nullptr;
    if (had) {
      old_rec = *found;
      // Detach the old contribution and customer reference.
      const int32_t col = meta.NationColumn(old_rec.nationkey);
      if (col >= 0 && old_rec.rev != 0)
        ttx.Add(TpchQ5LeafNode(meta.tree_size, old_rec.ordinal), col,
                -old_rec.rev);
      if (const auto *oc = ctx.Find(old_rec.custkey))
        ctx.Set(old_rec.custkey,
                TpchQ5CustRec{oc->nationkey, oc->order_count - 1});
      otx.Erase(key64);
    }
    const auto *cust = ctx.Find(nd->custkey);
    if (!cust) {
      dropped.insert(key64); // customer outside the join
      continue;
    }
    const int32_t ord =
        TpchQ5DateOrdinal(nd->orderdate_days, meta.min_orderdate_days);
    if (ord < 0 || ord >= static_cast<int32_t>(meta.tree_size))
      return rebuild();
    TpchQ5OrderRec nr{cust->nationkey, ord, nd->custkey, 0};
    if (had && old_rec.nationkey == cust->nationkey) {
      // Same nation: the existing revenue just moves (date / customer).
      credit_order(nr, old_rec.rev);
    } else {
      bool ok = false;
      const int64_t rev = recompute_rev(key64, cust->nationkey, ok);
      if (!ok)
        return rebuild();
      credit_order(nr, rev);
      recomputed.insert(key64);
    }
    ctx.Set(nd->custkey, TpchQ5CustRec{cust->nationkey, cust->order_count + 1});
    otx.Set(key64, nr);
  }

  // Phase A3: plain order inserts. Orders of unknown customers contribute
  // nothing and are not tracked (as in BuildState).
  for (const auto &order : buffer.OrderMutations()) {
    if (order.sign < 0 || updated.count(order.orderkey))
      continue;
    const auto *cust = ctx.Find(order.custkey);
    if (!cust)
      continue;
    const int32_t ord =
        TpchQ5DateOrdinal(order.orderdate_days, meta.min_orderdate_days);
    if (ord < 0 || ord >= static_cast<int32_t>(meta.tree_size))
      return rebuild(); // outside the tree's date range: re-derive bounds
    if (otx.Find(order.orderkey))
      return rebuild();
    otx.Set(order.orderkey,
            TpchQ5OrderRec{cust->nationkey, ord, order.custkey, 0});
    ctx.Set(order.custkey,
            TpchQ5CustRec{cust->nationkey, cust->order_count + 1});
  }

  // Phase B: lineitems. Credit a nation only when supplier nation equals
  // customer nation (same filter as BuildState / the Q5 join). A lineitem of
  // an absent supplier only adjusts the supplier's reference count.
  bool unresolved = false;
  auto apply_row = [&](const TpchQ5LineitemInput &row, int64_t sign) {
    const auto *supp = stx.Find(row.suppkey);
    TpchQ5SuppRec srec;
    if (supp)
      srec = *supp;
    else
      srec = TpchQ5SuppRec{0, 0, false};
    srec.line_count += static_cast<int32_t>(sign);
    stx.Set(row.suppkey, srec);
    if (dropped.count(row.orderkey) || recomputed.count(row.orderkey))
      return;
    const auto *order = otx.Find(row.orderkey);
    if (!order) {
      unresolved = true;
      return;
    }
    if (!srec.present || srec.nationkey != order->nationkey)
      return;
    TpchQ5OrderRec orec = *order;
    credit_order(orec, sign * ComputeRevenueRaw(row.extended_price_raw,
                                                row.discount_raw));
    otx.Set(row.orderkey, orec);
  };
  for (const auto &m : buffer.LineitemMutations()) {
    if (!m.is_insert)
      apply_row(m.old_row, -1);
    if (!m.is_delete)
      apply_row(m.new_row, 1);
    if (unresolved)
      return rebuild();
  }

  // Phase C: order deletes subtract whatever the order still contributes.
  for (const auto &order : buffer.OrderMutations()) {
    if (order.sign > 0 || updated.count(order.orderkey))
      continue;
    const auto *rec = otx.Find(order.orderkey);
    if (!rec)
      continue;
    const TpchQ5OrderRec orec = *rec;
    if (orec.rev != 0) {
      const int32_t col = meta.NationColumn(orec.nationkey);
      if (col >= 0)
        ttx.Add(TpchQ5LeafNode(meta.tree_size, orec.ordinal), col, -orec.rev);
    }
    if (const auto *cust = ctx.Find(orec.custkey))
      ctx.Set(orec.custkey,
              TpchQ5CustRec{cust->nationkey, cust->order_count - 1});
    otx.Erase(order.orderkey);
  }

  // Phase D: customer deletes (only when nothing references them).
  for (const auto &d : buffer.CustomerMutations()) {
    if (d.kind != Kind::Delete)
      continue;
    const auto *rec = ctx.Find(d.key);
    if (!rec || rec->order_count > 0)
      return rebuild();
    ctx.Erase(d.key);
  }

  // Phase E: supplier changes, in arrival order, against the post-write
  // lineitem table: the supplier's lineitems are fetched with one point query
  // and each matching contribution is removed / added through the order map.
  // (A key with no lineitems needs no query.)
  auto adjust_supplier = [&](int64_t suppkey, bool has_old, int32_t old_nation,
                             bool has_new, int32_t new_nation) -> bool {
    const auto *rec = stx.Find(suppkey);
    const int32_t lines = rec ? rec->line_count : 0;
    if (lines > 0) {
      std::vector<TpchQ5LineitemInput> rows;
      if (!initial_scan ||
          !initial_scan->ScanLineitemsBySupplier(
              suppkey,
              [&](const TpchQ5LineitemInput &li) { rows.push_back(li); }))
        return false;
      for (const auto &li : rows) {
        const auto *order = otx.Find(li.orderkey);
        if (!order)
          continue; // no tracked order: contributed nothing
        TpchQ5OrderRec orec = *order;
        const int64_t rev =
            ComputeRevenueRaw(li.extended_price_raw, li.discount_raw);
        if (has_old && orec.nationkey == old_nation)
          credit_order(orec, -rev);
        if (has_new && orec.nationkey == new_nation)
          credit_order(orec, rev);
        otx.Set(li.orderkey, orec);
      }
    }
    if (has_new)
      stx.Set(suppkey, TpchQ5SuppRec{new_nation, lines, true});
    else if (lines > 0)
      stx.Set(suppkey, TpchQ5SuppRec{0, lines, false});
    else
      stx.Erase(suppkey);
    return true;
  };
  for (const auto &d : buffer.SupplierMutations()) {
    const auto *rec = stx.Find(d.key);
    if (d.kind == Kind::Insert) {
      if (rec && rec->present)
        return rebuild(); // duplicate key
      if (!adjust_supplier(d.key, false, 0, true, d.nationkey))
        return rebuild();
    } else if (d.kind == Kind::Delete) {
      if (!rec || !rec->present)
        return rebuild();
      if (!adjust_supplier(d.key, true, rec->nationkey, false, 0))
        return rebuild();
    } else {
      if (!rec || !rec->present)
        return rebuild();
      if (!adjust_supplier(d.key, true, rec->nationkey, true, d.nationkey))
        return rebuild();
    }
  }

  const bool side_dirty = otx.Dirty() || stx.Dirty() || ctx.Dirty();
  if (!side_dirty && !ttx.Dirty())
    return outcome;

  outcome.action = core::MaintenanceAction::ApplyDelta;
  if (side_dirty) {
    auto new_side = std::make_shared<TpchQ5State>();
    new_side->meta = side->meta;
    new_side->orders = otx.Finish();
    new_side->supps = stx.Finish();
    new_side->custs = ctx.Finish();
    outcome.updates.push_back(core::StateImage::Make(
        kId, core::TpchQ5Layout::kSide, 0,
        MakeSideImage(std::shared_ptr<const TpchQ5State>(std::move(new_side)))));
  }
  if (ttx.Dirty()) {
    auto new_tree = std::make_shared<const TpchQ5Tree>(ttx.Finish());
    outcome.updates.push_back(core::StateImage::Make(
        kId, core::TpchQ5Layout::kTreeBase, 0, MakeTreeNodeImage(0, new_tree)));
  }
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
  const int32_t end_days = TpchQ5AddMonths(start_days, 12);

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

void TpchQ5Module::ExecuteFallback(core::AuxEngineServices &services,
                                   const core::QueryArguments &arguments,
                                   core::AuxRowSink &sink) {
  std::string region_name;
  int32_t start_days = 0;
  ParseArgs(arguments, region_name, start_days);
  const int32_t end_days = TpchQ5AddMonths(start_days, 12);

  auto state = BuildState(services);
  const auto &meta = *state.meta;

  std::vector<int32_t> eligible_nations;
  int32_t target_regionkey = ResolveRegion(region_name, meta, eligible_nations);
  if (target_regionkey < 0) {
    sink.Begin({{{"n_name", core::AuxType::String, false},
                 {"revenue", core::AuxType::ScaledDecimal, false}}});
    sink.End();
    return;
  }

  int32_t start_ord = TpchQ5DateOrdinal(start_days, meta.min_orderdate_days);
  int32_t end_ord = TpchQ5DateOrdinal(end_days, meta.min_orderdate_days);
  if (start_ord < 0)
    start_ord = 0;
  if (end_ord > static_cast<int32_t>(meta.tree_size))
    end_ord = static_cast<int32_t>(meta.tree_size);

  const auto nodes = TpchQ5TreeCover(meta.tree_size, start_ord, end_ord);
  EmitResult(services, meta, target_regionkey,
             SumCover(meta, state.tree, nodes), sink);
}

TpchQ5State TpchQ5Module::BuildState(core::AuxEngineServices &services) const {
  (void)services;
  TpchQ5State state;

  if (!initial_scan)
    throw std::logic_error("TPC-H Q5 requires a typed initial scan source");

  auto meta_ptr = std::make_shared<TpchQ5Meta>();
  auto &meta = *meta_ptr;

  initial_scan->ScanRegions([&](const TpchQ5RegionInput &input) {
    meta.region_name_to_key[input.name] = input.key;
  });

  initial_scan->ScanNations([&](const TpchQ5NationInput &input) {
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
  TpchQ5ShardBuilder<TpchQ5CustRec> custs;
  initial_scan->ScanCustomers([&](const TpchQ5CustomerInput &input) {
    custs.Add(input.custkey, TpchQ5CustRec{input.nationkey, 0});
  });
  custs.Sort();

  TpchQ5ShardBuilder<TpchQ5SuppRec> supps;
  initial_scan->ScanSuppliers([&](const TpchQ5SupplierInput &input) {
    supps.Add(input.suppkey, TpchQ5SuppRec{input.nationkey, 0});
  });
  supps.Sort();

  TpchQ5ShardBuilder<TpchQ5OrderRec> orders;
  meta.min_orderdate_days = std::numeric_limits<int32_t>::max();
  meta.max_orderdate_days = std::numeric_limits<int32_t>::min();
  initial_scan->ScanOrders([&](const TpchQ5OrderInput &input) {
    if (input.orderdate_days < meta.min_orderdate_days)
      meta.min_orderdate_days = input.orderdate_days;
    if (input.orderdate_days > meta.max_orderdate_days)
      meta.max_orderdate_days = input.orderdate_days;
    auto *cust = custs.Find(input.custkey);
    if (!cust)
      return;
    cust->order_count += 1;
    // `ordinal` temporarily holds the order date; rebased below.
    orders.Add(input.orderkey,
               TpchQ5OrderRec{cust->nationkey, input.orderdate_days,
                              input.custkey, 0});
  });

  if (meta.min_orderdate_days > meta.max_orderdate_days) {
    meta.min_orderdate_days = 0;
    meta.max_orderdate_days = 0;
  }

  int32_t num_days = meta.max_orderdate_days - meta.min_orderdate_days + 1;
  if (num_days < 1)
    num_days = 1;
  meta.tree_size = NextPowerOf2(static_cast<uint32_t>(num_days));
  const uint32_t tree_size = meta.tree_size;
  orders.ForEach([&](TpchQ5ShardBuilder<TpchQ5OrderRec>::Entry &e) {
    e.val.ordinal = TpchQ5DateOrdinal(e.val.ordinal, meta.min_orderdate_days);
  });
  orders.Sort();

  std::unordered_map<int64_t, int32_t> orphan_supp;
  std::vector<int64_t> flat(static_cast<std::size_t>(tree_size) * 2 * stride, 0);
  initial_scan->ScanLineitems([&](const TpchQ5LineitemInput &input) {
    auto *supp = supps.Find(input.suppkey);
    if (!supp) {
      ++orphan_supp[input.suppkey]; // lineitem of a supplier that does not exist
      return;
    }
    supp->line_count += 1;
    auto *order = orders.Find(input.orderkey);
    if (!order || supp->nationkey != order->nationkey)
      return;
    const int32_t col = meta.NationColumn(order->nationkey);
    if (col < 0 || order->ordinal < 0 ||
        order->ordinal >= static_cast<int32_t>(tree_size))
      return;
    const int64_t revenue =
        ComputeRevenueRaw(input.extended_price_raw, input.discount_raw);
    order->rev += revenue;
    flat[static_cast<std::size_t>(TpchQ5LeafNode(tree_size, order->ordinal)) *
             stride +
         static_cast<uint32_t>(col)] += revenue;
  });

  if (!orphan_supp.empty()) {
    for (const auto &entry : orphan_supp)
      supps.Add(entry.first, TpchQ5SuppRec{0, entry.second, false});
    supps.Sort();
  }

  for (uint32_t i = tree_size - 1; i > 0; i--)
    for (uint32_t c = 0; c < stride; ++c)
      flat[static_cast<std::size_t>(i) * stride + c] =
          flat[static_cast<std::size_t>(i * 2) * stride + c] +
          flat[static_cast<std::size_t>(i * 2 + 1) * stride + c];

  state.tree = PaginateTree(flat, tree_size, stride);
  state.orders = orders.Finish();
  state.supps = supps.Finish();
  state.custs = custs.Finish();
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
