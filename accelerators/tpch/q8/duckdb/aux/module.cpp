#include "tpch/q8/duckdb/aux/module.hpp"
#include "tpch/q8/calibration_writes.hpp"
#include "substrate/aux/core/byte_stream.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/partitioned_build.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>


namespace accelerator_codegen::aux_state::query {
namespace {

using core::ByteReader;
using core::ByteWriter;

constexpr uint32_t kCubeMagic = 0x51385143; // Q8C
constexpr uint32_t kVersionV2 = 3;
// Lineitem deltas per slice when resolving them against the dimensions.
constexpr std::size_t kResolveGrain = 2048;
// Touched mid arrays per slice when publishing a dimension map.
constexpr std::size_t kFinishGrain = 16;
// Shards per slice when sorting a built index.
constexpr std::size_t kSortShardGrain = 1024;

// ── Cell extraction helpers ─────────────────────────────────────────────────
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

bool RawDecimalCell(const core::AuxRow &row, std::size_t index, int64_t &raw,
                    int32_t &scale) {
  if (index >= row.values.size() || row.values[index].is_null)
    return false;
  const auto &cell = row.values[index];
  if (std::holds_alternative<core::ScaledDecimal>(cell.payload)) {
    const auto decimal = std::get<core::ScaledDecimal>(cell.payload);
    raw = decimal.value;
    scale = decimal.scale;
    return true;
  }
  return false;
}

bool ReadLineitem(const core::AuxRow &row, TpchQ8LineitemInput &out) {
  int64_t partkey = 0, orderkey = 0, suppkey = 0, extended_raw = 0, discount_raw = 0;
  int32_t scale = 0;
  if (!IntCell(row, 0, partkey) ||
      !IntCell(row, 1, orderkey) ||
      !IntCell(row, 2, suppkey) ||
      !RawDecimalCell(row, 3, extended_raw, scale) ||
      !RawDecimalCell(row, 4, discount_raw, scale))
    return false;
  out.partkey = partkey;
  out.orderkey = orderkey;
  out.suppkey = suppkey;
  out.extendedprice_raw = extended_raw;
  out.discount_raw = discount_raw;
  return true;
}

bool ReadPart(const core::AuxRow &row, int64_t &key, std::string &type) {
  int64_t k = 0;
  if (!IntCell(row, 0, k) || !StringCell(row, 1, type))
    return false;
  key = k;
  return true;
}

bool ReadSupplier(const core::AuxRow &row, int64_t &key, int32_t &nationkey) {
  int64_t k = 0;
  if (!IntCell(row, 0, k) || !Int32Cell(row, 1, nationkey))
    return false;
  key = k;
  return true;
}

bool ReadCustomer(const core::AuxRow &row, int64_t &key, int32_t &nationkey) {
  int64_t k = 0;
  if (!IntCell(row, 0, k) || !Int32Cell(row, 1, nationkey))
    return false;
  key = k;
  return true;
}

bool ReadOrders(const core::AuxRow &row, int64_t &key, int64_t &custkey,
                int32_t &orderdate_days) {
  int64_t k = 0, ck = 0;
  if (!IntCell(row, 0, k) || !IntCell(row, 1, ck) || !Int32Cell(row, 2, orderdate_days))
    return false;
  key = k;
  custkey = ck;
  return true;
}

bool ReadNation(const core::AuxRow &row, int32_t &key, std::string &name,
                int32_t &regionkey) {
  if (!Int32Cell(row, 0, key) || !StringCell(row, 1, name) ||
      !Int32Cell(row, 2, regionkey))
    return false;
  return true;
}

bool ReadRegion(const core::AuxRow &row, int32_t &key, std::string &name) {
  if (!Int32Cell(row, 0, key) || !StringCell(row, 1, name))
    return false;
  return true;
}

// ── Image helpers ───────────────────────────────────────────────────────────
std::shared_ptr<const TpchQ8StateImage>
MakeStateImage(TpchQ8ImageKind kind, int32_t year,
               const std::shared_ptr<const TpchQ8State> &state) {
  auto image = std::make_shared<TpchQ8StateImage>();
  image->kind = kind;
  image->year = year;
  image->state = state;
  return image;
}

const TpchQ8StateImage *AsQ8Image(const core::StateImage &image,
                                  TpchQ8ImageKind kind) {
  const auto *payload = image.As<TpchQ8StateImage>();
  return payload && payload->kind == kind && payload->state ? payload : nullptr;
}

core::InvalidationPlan All() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

core::InvalidationPlan PartitionSet(std::vector<core::PartitionId> partitions) {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
  for (const auto partition : partitions)
    plan.partitions.insert(partition);
  return plan;
}

// ── Dense cube helpers ─────────────────────────────────────────────────────
constexpr int32_t kAbsent = std::numeric_limits<int32_t>::min();

int32_t AxisOf(const TpchQ8State &state) { return state.nat_dim + 1; }

// Slot of a nation key; negative keys share the "other" slot.  Returns false
// when the key is >= nat_dim (the layout cannot represent it).
bool NationSlot(int32_t key, int32_t nat_dim, int32_t &slot) {
  if (key < 0) {
    slot = nat_dim;
    return true;
  }
  if (key >= nat_dim)
    return false;
  slot = key;
  return true;
}

// Copy-on-write editor over the page table of a (not yet published) state.
// A page is copied the first time it is touched; untouched pages stay shared
// with the previous version.
class CubeEditor {
public:
  explicit CubeEditor(TpchQ8State &s)
      : st(s), owned(s.pages.size(), 0), mut(s.pages.size(), nullptr) {}

  void Add(int year_idx, int32_t pt, int32_t cslot, int32_t sslot,
           int64_t vol) {
    const std::size_t idx = static_cast<std::size_t>(pt) * 2 +
                            static_cast<std::size_t>(year_idx);
    if (idx >= st.pages.size()) {
      st.pages.resize(idx + 1);
      owned.resize(idx + 1, 0);
      mut.resize(idx + 1, nullptr);
    }
    const std::size_t n = static_cast<std::size_t>(AxisOf(st));
    if (!owned[idx]) {
      std::shared_ptr<TpchQ8CubePage> p;
      if (st.pages[idx]) {
        p = std::make_shared<TpchQ8CubePage>(*st.pages[idx]);
      } else {
        p = std::make_shared<TpchQ8CubePage>();
        p->cell.assign(n * n, 0);
        p->roll.assign(n, 0);
      }
      mut[idx] = p.get();
      st.pages[idx] = std::move(p);
      owned[idx] = 1;
    }
    TpchQ8CubePage *p = mut[idx];
    p->cell[static_cast<std::size_t>(cslot) * n +
            static_cast<std::size_t>(sslot)] += vol;
    p->roll[static_cast<std::size_t>(cslot)] += vol;
  }

private:
  TpchQ8State &st;
  std::vector<uint8_t> owned;
  std::vector<TpchQ8CubePage *> mut;
};

// Read-only dense key -> int32 lookup used to speed up the build scan.
struct DenseI32 {
  static constexpr int64_t kMaxKey = int64_t(1) << 27;
  std::vector<int32_t> v;
  template <class M> void Fill(M &m, int64_t max_key) {
    if (max_key < 0 || max_key > kMaxKey)
      return;
    v.assign(static_cast<std::size_t>(max_key) + 1, kAbsent);
    m.ForEach([&](const TpchQ8SmallMap::Element &e) {
      if (e.key >= 0 && e.key <= max_key)
        v[static_cast<std::size_t>(e.key)] = e.value;
    });
  }
  template <class M>
  bool Get(M &m, int64_t k, int32_t &out) const {
    if (!v.empty()) {
      if (k < 0 || k >= static_cast<int64_t>(v.size()))
        return false;
      out = v[static_cast<std::size_t>(k)];
      return out != kAbsent;
    }
    const auto *p = m.Find(k);
    if (!p)
      return false;
    out = p->value;
    return true;
  }
};

// ── Date helpers ────────────────────────────────────────────────────────────
// Use the range-based helper on TpchQ8State. A naive Howard-Hinnant year
// extraction without the Jan/Feb (m <= 2) civil adjustment mis-bins those
// months (e.g. 1995-01 → 1994) and shifts Q8 market share (~0.046 vs 0.0455).
int32_t OrderDateToYear(int32_t days) {
  return TpchQ8State::YearFromDays(days);
}

// ── Volume computation ──────────────────────────────────────────────────────
int64_t ComputeVolumeRaw(int64_t extendedprice_raw, int64_t discount_raw) {
  // volume = l_extendedprice * (1 - l_discount)
  // Both inputs are scaled by 100 (scale=2).
  // So volume_raw = extendedprice_raw * (100 - discount_raw)
  // Result scale is 2+2=4, i.e. raw *= 100 for output scale 4
  return extendedprice_raw * (100 - discount_raw);
}

} // namespace

int64_t TpchQ8VolumeRaw(int64_t extendedprice_raw, int64_t discount_raw) {
  return ComputeVolumeRaw(extendedprice_raw, discount_raw);
}

// ── Encode ──────────────────────────────────────────────────────────────────
// Format V2: sparse nonzero cells (same cell record as V1), then nat_dim and
// the fresh-key bounds, then the dictionaries and reverse maps.
std::vector<uint8_t> EncodeTpchQ8AnnualCube(const TpchQ8State &state) {
  ByteWriter writer;
  const int32_t axis = AxisOf(state);
  auto key_of = [&](int32_t slot) { return slot >= state.nat_dim ? -1 : slot; };

  uint32_t cell_count = 0;
  for (const auto &page : state.pages) {
    if (!page)
      continue;
    for (const int64_t v : page->cell)
      if (v != 0)
        ++cell_count;
  }
  core::WriteImageHeader(writer, kCubeMagic, kVersionV2, cell_count);
  for (std::size_t idx = 0; idx < state.pages.size(); ++idx) {
    const auto &page = state.pages[idx];
    if (!page)
      continue;
    const int32_t pt = static_cast<int32_t>(idx / 2);
    const int32_t year = 1995 + static_cast<int32_t>(idx % 2);
    for (int32_t c = 0; c < axis; ++c) {
      for (int32_t s = 0; s < axis; ++s) {
        const int64_t v = page->cell[static_cast<std::size_t>(c) * axis + s];
        if (v == 0)
          continue;
        writer.I32(year);
        writer.I32(key_of(c));
        writer.I32(key_of(s));
        writer.I32(pt);
        writer.I64(v);
        writer.I64(v);
      }
    }
  }
  const auto &d = *state.dims;
  writer.I32(state.nat_dim);
  writer.I64(d.part_build_max);
  writer.I64(d.supp_build_max);
  writer.I64(d.cust_build_max);
  writer.I64(d.order_build_max);
  for (const auto *set : {&d.fresh_part_ref, &d.fresh_supp_ref,
                          &d.fresh_cust_ref, &d.deleted_orders}) {
    writer.U32(static_cast<uint32_t>(set->size()));
    for (const auto k : *set)
      writer.I64(k);
  }

  writer.U32(static_cast<uint32_t>(d.dict->part_types.size()));
  for (const auto &pt : d.dict->part_types)
    writer.String(pt);
  writer.U32(static_cast<uint32_t>(d.dict->nation_name_to_key.size()));
  for (const auto &[name, nk] : d.dict->nation_name_to_key) {
    writer.String(name);
    writer.I32(nk);
  }
  writer.U32(static_cast<uint32_t>(d.dict->nation_region.size()));
  for (const auto &[nk, rname] : d.dict->nation_region) {
    writer.I32(nk);
    writer.String(rname);
  }
  writer.U32(static_cast<uint32_t>(d.dict->region_nations.size()));
  for (const auto &[rname, nks] : d.dict->region_nations) {
    writer.String(rname);
    writer.U32(static_cast<uint32_t>(nks.size()));
    for (const auto nk : nks)
      writer.I32(nk);
  }
  // Reverse maps for maintenance
  writer.U32(static_cast<uint32_t>(d.part_key_to_type.size()));
  d.part_key_to_type.ForEach([&](const TpchQ8SmallMap::Element &e) {
    writer.I64(e.key);
    writer.String(d.dict->part_types[static_cast<std::size_t>(e.value)]);
  });
  writer.U32(static_cast<uint32_t>(d.suppkey_to_nation.size()));
  d.suppkey_to_nation.ForEach([&](const TpchQ8SmallMap::Element &e) {
    writer.I64(e.key);
    writer.I32(e.value);
  });
  writer.U32(static_cast<uint32_t>(d.custkey_to_nation.size()));
  d.custkey_to_nation.ForEach([&](const TpchQ8SmallMap::Element &e) {
    writer.I64(e.key);
    writer.I32(e.value);
  });
  writer.U64(d.order_info.size());
  d.order_info.ForEach([&](const TpchQ8OrderMap::Element &e) {
    writer.I64(e.key);
    writer.I64(e.value.custkey);
    writer.I32(e.value.orderdate_days);
  });
  return std::move(writer.bytes);
}

// ── Decode ──────────────────────────────────────────────────────────────────
bool DecodeTpchQ8AnnualCube(const std::vector<uint8_t> &bytes,
                            TpchQ8State &state) {
  TpchQ8Dimensions d;
  ByteReader reader{bytes};
  uint64_t count = 0;
  if (!core::ReadImageHeader(reader, kCubeMagic, kVersionV2, count))
    return false;

  std::vector<TpchQ8CubeCell> cells;
  cells.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    TpchQ8CubeCell cell;
    if (!reader.I32(cell.year) || !reader.I32(cell.customer_nationkey) ||
        !reader.I32(cell.supplier_nationkey) || !reader.I32(cell.part_type_index) ||
        !reader.I64(cell.nation_volume_raw) || !reader.I64(cell.total_volume_raw))
      return false;
    cells.push_back(cell);
  }

  int32_t nat_dim = 0;
  if (!reader.I32(nat_dim) || nat_dim < 1 || nat_dim > (1 << 16) ||
      !reader.I64(d.part_build_max) || !reader.I64(d.supp_build_max) ||
      !reader.I64(d.cust_build_max) || !reader.I64(d.order_build_max))
    return false;
  for (auto *set : {&d.fresh_part_ref, &d.fresh_supp_ref, &d.fresh_cust_ref,
                    &d.deleted_orders}) {
    uint32_t n = 0;
    if (!reader.U32(n))
      return false;
    for (uint32_t i = 0; i < n; ++i) {
      int64_t k = 0;
      if (!reader.I64(k))
        return false;
      set->insert(k);
    }
  }

  uint32_t pt_count = 0;
  if (!reader.U32(pt_count))
    return false;
  d.dict->part_types.reserve(pt_count);
  for (uint32_t i = 0; i < pt_count; ++i) {
    std::string pt;
    if (!reader.String(pt))
      return false;
    d.dict->part_type_to_index[pt] = static_cast<int32_t>(d.dict->part_types.size());
    d.dict->part_types.push_back(pt);
  }

  uint32_t nn_count = 0;
  if (!reader.U32(nn_count))
    return false;
  for (uint32_t i = 0; i < nn_count; ++i) {
    std::string name;
    int32_t nk = 0;
    if (!reader.String(name) || !reader.I32(nk))
      return false;
    d.dict->nation_name_to_key[name] = nk;
  }

  uint32_t nr_count = 0;
  if (!reader.U32(nr_count))
    return false;
  for (uint32_t i = 0; i < nr_count; ++i) {
    int32_t nk = 0;
    std::string rname;
    if (!reader.I32(nk) || !reader.String(rname))
      return false;
    d.dict->nation_region[nk] = rname;
  }

  uint32_t rn_count = 0;
  if (!reader.U32(rn_count))
    return false;
  for (uint32_t i = 0; i < rn_count; ++i) {
    std::string rname;
    uint32_t nks_count = 0;
    if (!reader.String(rname) || !reader.U32(nks_count))
      return false;
    auto &nks = d.dict->region_nations[rname];
    nks.reserve(nks_count);
    for (uint32_t j = 0; j < nks_count; ++j) {
      int32_t nk = 0;
      if (!reader.I32(nk))
        return false;
      nks.push_back(nk);
    }
  }

  uint32_t pkt_count = 0;
  if (!reader.U32(pkt_count))
    return false;
  // Bulk load; a repeated key keeps its last value.
  TpchQ8SmallMap::Builder part_b, supp_b, cust_b;
  TpchQ8OrderMap::Builder order_b;
  for (uint32_t i = 0; i < pkt_count; ++i) {
    int64_t pk = 0;
    std::string pt;
    if (!reader.I64(pk) || !reader.String(pt))
      return false;
    const auto it = d.dict->part_type_to_index.find(pt);
    if (it == d.dict->part_type_to_index.end())
      return false;
    part_b.Add({pk, it->second});
  }

  uint32_t skn_count = 0;
  if (!reader.U32(skn_count))
    return false;
  for (uint32_t i = 0; i < skn_count; ++i) {
    int64_t sk = 0;
    int32_t nk = 0;
    if (!reader.I64(sk) || !reader.I32(nk))
      return false;
    supp_b.Add({sk, nk});
  }

  uint32_t ckn_count = 0;
  if (!reader.U32(ckn_count))
    return false;
  for (uint32_t i = 0; i < ckn_count; ++i) {
    int64_t ck = 0;
    int32_t nk = 0;
    if (!reader.I64(ck) || !reader.I32(nk))
      return false;
    cust_b.Add({ck, nk});
  }

  uint64_t oi_count = 0;
  if (!reader.U64(oi_count))
    return false;
  for (uint64_t i = 0; i < oi_count; ++i) {
    int64_t ok = 0;
    TpchQ8Dimensions::OrderInfo oi;
    if (!reader.I64(ok) || !reader.I64(oi.custkey) || !reader.I32(oi.orderdate_days))
      return false;
    order_b.Add({ok, oi});
  }

  part_b.SortDedupe();
  supp_b.SortDedupe();
  cust_b.SortDedupe();
  order_b.SortDedupe();
  d.part_key_to_type = part_b.Finish();
  d.suppkey_to_nation = supp_b.Finish();
  d.custkey_to_nation = cust_b.Finish();
  d.order_info = order_b.Finish();

  state.nat_dim = nat_dim;
  state.pages.clear();
  {
    CubeEditor editor(state);
    for (const auto &cell : cells) {
      if ((cell.year != 1995 && cell.year != 1996) ||
          cell.part_type_index < 0 ||
          static_cast<std::size_t>(cell.part_type_index) >= d.dict->part_types.size())
        return false;
      int32_t cs = 0, ss = 0;
      if (!NationSlot(cell.customer_nationkey, nat_dim, cs) ||
          !NationSlot(cell.supplier_nationkey, nat_dim, ss))
        return false;
      editor.Add(cell.year - 1995, cell.part_type_index, cs, ss,
                 cell.total_volume_raw);
    }
  }
  state.dims = std::make_shared<const TpchQ8Dimensions>(std::move(d));
  return reader.FullyConsumed();
}

// ── TxnBuffer ───────────────────────────────────────────────────────────────
bool TpchQ8TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ8TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  deltas.clear();
  order_info_overlay.clear();
  order_erase.clear();
  part_net.clear();
  supplier_net.clear();
  customer_net.clear();
}
bool TpchQ8TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
const std::vector<TpchQ8LineitemDelta> &TpchQ8TxnBuffer::Deltas() const {
  return deltas;
}
void TpchQ8TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  deltas.clear();
}
void TpchQ8TxnBuffer::AddDelta(TpchQ8LineitemDelta delta) {
  pending = true;
  if (delta.volume_raw != 0)
    deltas.push_back(delta);
}
void TpchQ8TxnBuffer::AddOrderInfo(int64_t orderkey, int64_t custkey,
                                  int32_t orderdate_days) {
  pending = true;
  order_info_overlay[orderkey] = TpchQ8Dimensions::OrderInfo{custkey,
                                                            orderdate_days};
  order_erase.erase(orderkey);
}
void TpchQ8TxnBuffer::EraseOrder(int64_t orderkey) {
  pending = true;
  order_erase.insert(orderkey);
}
const std::unordered_map<int64_t, TpchQ8Dimensions::OrderInfo> &
TpchQ8TxnBuffer::OrderInfoOverlay() const {
  return order_info_overlay;
}
const std::unordered_set<int64_t> &TpchQ8TxnBuffer::OrderErase() const {
  return order_erase;
}
void TpchQ8TxnBuffer::SetPart(int64_t key, bool present, std::string type) {
  pending = true;
  auto &n = part_net[key];
  n.present = present;
  n.type = std::move(type);
}
void TpchQ8TxnBuffer::SetSupplier(int64_t key, bool present,
                                  int32_t nationkey) {
  pending = true;
  auto &n = supplier_net[key];
  n.present = present;
  n.nationkey = nationkey;
}
void TpchQ8TxnBuffer::SetCustomer(int64_t key, bool present,
                                  int32_t nationkey) {
  pending = true;
  auto &n = customer_net[key];
  n.present = present;
  n.nationkey = nationkey;
}
const std::unordered_map<int64_t, TpchQ8DimNet> &
TpchQ8TxnBuffer::PartNet() const {
  return part_net;
}
const std::unordered_map<int64_t, TpchQ8DimNet> &
TpchQ8TxnBuffer::SupplierNet() const {
  return supplier_net;
}
const std::unordered_map<int64_t, TpchQ8DimNet> &
TpchQ8TxnBuffer::CustomerNet() const {
  return customer_net;
}

// ── Module identity ─────────────────────────────────────────────────────────
core::AcceleratorId TpchQ8Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ8Module::Dependencies() const {
  return {{config.part_table, {0, 1}, {0, 4}},
          {config.supplier_table, {0, 1}, {0, 3}},
          {config.customer_table, {0, 1}, {0, 3}},
          {config.orders_table, {0, 1, 2}, {0, 1, 4}},
          {config.lineitem_table, {0, 1, 2, 3, 4}, {1, 0, 2, 5, 6}},
          {config.nation_table, {0, 1, 2}, {0, 1, 2}},
          {config.region_table, {0, 1}, {0, 1}}};
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ8Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ8TxnBuffer>();
}

// ── Build ───────────────────────────────────────────────────────────────────
void TpchQ8Module::Build(core::AuxEngineServices &services,
                         core::StateBuilder &builder) {
  const auto state = BuildState(services);
  auto typed_state = std::make_shared<const TpchQ8State>(state);
  // Store full state in both year partitions (same full cube with both years)
  builder.Write(core::StateImage::Make(
      TpchQ8Layout::kAccelerator, TpchQ8Layout::kYear1995, 0,
      MakeStateImage(TpchQ8ImageKind::AnnualCube, 1995, typed_state)));
  builder.Write(core::StateImage::Make(
      TpchQ8Layout::kAccelerator, TpchQ8Layout::kYear1996, 0,
      MakeStateImage(TpchQ8ImageKind::AnnualCube, 1996, typed_state)));
  builder.Finish();
}

// ── InterpretMutation ──────────────────────────────────────────────────────
core::InvalidationPlan
TpchQ8Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.lineitem_table ||
      mutation.table == config.orders_table ||
      mutation.table == config.part_table ||
      mutation.table == config.supplier_table ||
      mutation.table == config.customer_table ||
      mutation.table == config.nation_table ||
      mutation.table == config.region_table) {
    return All();
  }
  return {};
}

// ── AccumulateMutation ─────────────────────────────────────────────────────
namespace {
// Fold a dimension-table mutation into per-key net changes.  Any shape we
// cannot interpret (unreadable row, size mismatch) forces a rebuild.
template <class V, class ReadFn, class SetFn>
void AccumulateDim(const core::MutationBatch &mutation, TpchQ8TxnBuffer &buffer,
                   ReadFn read, SetFn set) {
  switch (mutation.kind) {
  case core::MutationKind::Insert:
    for (const auto &row : mutation.new_rows) {
      int64_t k = 0;
      V v{};
      if (!read(row, k, v)) {
        buffer.MarkRebuild();
        return;
      }
      set(k, true, v);
    }
    break;
  case core::MutationKind::Delete:
    for (const auto &row : mutation.old_rows) {
      int64_t k = 0;
      V v{};
      if (!read(row, k, v)) {
        buffer.MarkRebuild();
        return;
      }
      set(k, false, v);
    }
    break;
  case core::MutationKind::Update:
    if (mutation.old_rows.size() != mutation.new_rows.size()) {
      buffer.MarkRebuild();
      return;
    }
    for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
      int64_t ok = 0, nk = 0;
      V ov{}, nv{};
      if (!read(mutation.old_rows[i], ok, ov) ||
          !read(mutation.new_rows[i], nk, nv)) {
        buffer.MarkRebuild();
        return;
      }
      if (ok != nk)
        set(ok, false, ov);
      set(nk, true, nv);
    }
    break;
  }
}
} // namespace

void TpchQ8Module::AccumulateMutation(const core::MutationBatch &mutation,
                                      core::AuxEngineServices &,
                                      core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ8TxnBuffer &>(base_buffer);

  // nation / region: the state caches nation names and regions → rebuild.
  if (mutation.table == config.nation_table ||
      mutation.table == config.region_table) {
    buffer.MarkRebuild();
    return;
  }

  // part / supplier / customer: net per-key change, applied incrementally.
  if (mutation.table == config.part_table) {
    AccumulateDim<std::string>(
        mutation, buffer,
        [](const core::AuxRow &r, int64_t &k, std::string &v) {
          return ReadPart(r, k, v);
        },
        [&](int64_t k, bool present, const std::string &v) {
          buffer.SetPart(k, present, v);
        });
    return;
  }
  if (mutation.table == config.supplier_table) {
    AccumulateDim<int32_t>(
        mutation, buffer,
        [](const core::AuxRow &r, int64_t &k, int32_t &v) {
          return ReadSupplier(r, k, v);
        },
        [&](int64_t k, bool present, int32_t v) {
          buffer.SetSupplier(k, present, v);
        });
    return;
  }
  if (mutation.table == config.customer_table) {
    AccumulateDim<int32_t>(
        mutation, buffer,
        [](const core::AuxRow &r, int64_t &k, int32_t &v) {
          return ReadCustomer(r, k, v);
        },
        [&](int64_t k, bool present, int32_t v) {
          buffer.SetCustomer(k, present, v);
        });
    return;
  }

  if (mutation.table == config.orders_table) {
    switch (mutation.kind) {
    case core::MutationKind::Insert:
      for (const auto &row : mutation.new_rows) {
        int64_t orderkey = 0, custkey = 0;
        int32_t days = 0;
        if (!ReadOrders(row, orderkey, custkey, days)) {
          buffer.MarkRebuild();
          return;
        }
        buffer.AddOrderInfo(orderkey, custkey, days);
      }
      break;
    case core::MutationKind::Delete:
      for (const auto &row : mutation.old_rows) {
        int64_t orderkey = 0, custkey = 0;
        int32_t days = 0;
        if (!ReadOrders(row, orderkey, custkey, days)) {
          buffer.MarkRebuild();
          return;
        }
        // Overlay keeps the old info so this transaction's lineitem deltas
        // still resolve; the dims entry is erased at commit.
        buffer.AddOrderInfo(orderkey, custkey, days);
        buffer.EraseOrder(orderkey);
      }
      break;
    case core::MutationKind::Update:
      // Orders whose (key, custkey, orderdate) are unchanged are no-ops;
      // otherwise the change is a delete of the old key plus an insert of the
      // new one, reconciled against the lineitems at commit.
      if (mutation.old_rows.size() != mutation.new_rows.size()) {
        buffer.MarkRebuild();
        return;
      }
      for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
        int64_t ok = 0, oc = 0, nk = 0, nc = 0;
        int32_t od = 0, nd = 0;
        if (!ReadOrders(mutation.old_rows[i], ok, oc, od) ||
            !ReadOrders(mutation.new_rows[i], nk, nc, nd)) {
          buffer.MarkRebuild();
          return;
        }
        if (ok == nk && oc == nc && od == nd)
          continue;
        if (ok != nk)
          buffer.EraseOrder(ok);
        buffer.AddOrderInfo(nk, nc, nd);
      }
      break;
    }
    return;
  }

  if (mutation.table != config.lineitem_table)
    return;

  auto add = [&](const core::AuxRow &row, int8_t sign) {
    TpchQ8LineitemInput input;
    if (!ReadLineitem(row, input)) {
      buffer.MarkRebuild();
      return;
    }
    const int64_t volume = ComputeVolumeRaw(input.extendedprice_raw,
                                            input.discount_raw);
    buffer.AddDelta({input.partkey, input.orderkey, input.suppkey,
                     volume, sign});
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

// ── StageMaintenance ───────────────────────────────────────────────────────
core::StagedMaintenanceOutcome
TpchQ8Module::StageMaintenance(core::AuxEngineServices &services,
                               const core::InvalidationPlan &,
                               core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ8TxnBuffer &>(base_buffer);
  core::StagedMaintenanceOutcome outcome;

  auto Rebuild = [&]() {
    core::StagedMaintenanceOutcome o;
    o.action = core::MaintenanceAction::RebuildAll;
    o.invalidations = All();
    o.pending_rebuild = o.invalidations;
    return o;
  };

  if (buffer.RequiresRebuild()) {
    const auto decision = services.ChooseMaintenance(
        {0, 0, false, false, true, true, false,
         core::MaintenanceAction::NoOp,
         services.ExpectedQueriesBeforeNextWrite()});
    outcome.action = decision.action == core::MaintenanceAction::ApplyDelta
                         ? core::MaintenanceAction::RebuildAll
                         : decision.action;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }
  const std::size_t work =
      buffer.Deltas().size() + buffer.OrderInfoOverlay().size() +
      buffer.OrderErase().size() + buffer.PartNet().size() +
      buffer.SupplierNet().size() + buffer.CustomerNet().size();
  if (work == 0)
    return outcome;

  const auto decision = services.ChooseMaintenance(
      {static_cast<uint64_t>(work), 1, true, false, true,
       false, false, core::MaintenanceAction::NoOp,
       services.ExpectedQueriesBeforeNextWrite()});
  if (decision.action == core::MaintenanceAction::RebuildPartition) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }
  if (decision.action != core::MaintenanceAction::ApplyDelta) {
    outcome.action = decision.action;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  // Read existing state.  The page table (a vector of shared_ptr) is copied;
  // pages and dimension shards are shared and copied only when touched.
  core::Version existing_version = 0;
  const auto visibility = services.CurrentVisibility();
  std::shared_ptr<TpchQ8State> new_state;
  const auto snapshot = services.StateStore().ReadPartitions(
      visibility, {TpchQ8Layout::kYear1995});
  for (const auto &image : snapshot.partitions) {
    if (image.partition == TpchQ8Layout::kYear1995) {
      const auto *typed = AsQ8Image(image, TpchQ8ImageKind::AnnualCube);
      if (!typed)
        return Rebuild();
      new_state = std::make_shared<TpchQ8State>();
      new_state->nat_dim = typed->state->nat_dim;
      new_state->pages = typed->state->pages;
      new_state->dims = typed->state->dims;  // shared until a write needs it
      existing_version = image.version;
      break;
    }
  }
  if (!new_state)
    return Rebuild();

  CubeEditor editor(*new_state);
  const int32_t nat_dim = new_state->nat_dim;

  // COW helper: the Dimensions object is copied (small dictionaries + shard
  // pointer tables) at most once per commit, and only when a write needs it.
  const TpchQ8Dimensions *dims = new_state->dims.get();
  std::shared_ptr<TpchQ8Dimensions> mutable_dims;
  // Writers over the dimension maps (created with mutable_dims; they clone
  // each touched shard once and are published into mutable_dims at the end).
  std::optional<TpchQ8SmallMap::Txn> part_tx, supp_tx, cust_tx;
  std::optional<TpchQ8OrderMap::Txn> order_tx;
  auto EnsureMutableDims = [&]() {
    if (!mutable_dims) {
      mutable_dims = std::make_shared<TpchQ8Dimensions>(*dims);
      new_state->dims = mutable_dims;
      dims = mutable_dims.get();
      part_tx.emplace(dims->part_key_to_type);
      supp_tx.emplace(dims->suppkey_to_nation);
      cust_tx.emplace(dims->custkey_to_nation);
      order_tx.emplace(dims->order_info);
    }
  };
  // Current (new-view) lookups: through the writers once they exist.
  auto NewPart = [&](int64_t k) {
    return TpchQ8Val(part_tx ? part_tx->Find(k) : dims->part_key_to_type.Find(k));
  };
  auto NewSupp = [&](int64_t k) {
    return TpchQ8Val(supp_tx ? supp_tx->Find(k)
                             : dims->suppkey_to_nation.Find(k));
  };
  auto NewCust = [&](int64_t k) {
    return TpchQ8Val(cust_tx ? cust_tx->Find(k)
                             : dims->custkey_to_nation.Find(k));
  };
  auto NewOrder = [&](int64_t k) {
    return TpchQ8Val(order_tx ? order_tx->Find(k) : dims->order_info.Find(k));
  };
  bool dict_cloned = false;
  auto TypeIndex = [&](const std::string &type) -> int32_t {
    const auto it = dims->dict->part_type_to_index.find(type);
    if (it != dims->dict->part_type_to_index.end())
      return it->second;
    EnsureMutableDims();
    if (!dict_cloned) {
      mutable_dims->dict = std::make_shared<TpchQ8Dict>(*mutable_dims->dict);
      dict_cloned = true;
    }
    const int32_t idx = static_cast<int32_t>(mutable_dims->dict->part_types.size());
    mutable_dims->dict->part_type_to_index[type] = idx;
    mutable_dims->dict->part_types.push_back(type);
    return idx;
  };

  // ── Exactness argument ─────────────────────────────────────────────────
  // cube(L, dims) = sum over lineitems l in L of cell(l; dims).  Telescope:
  //   cube(L1,D1) - cube(L0,D0)
  //     = [cube(L1,D1) - cube(L1,D0)]      (A) dimension changes on current L1
  //     + [cube(L1,D0) - cube(L0,D0)]      (B) lineitem deltas under old dims
  // (A) is computed by point-scanning the current lineitems of every "heavy"
  // changed entity (part / supplier / customer / order), deduplicated so that
  // a lineitem touching several changed entities is counted once.  (B) uses
  // the old view of heavy entities.  "Fresh" entities (new keys nobody
  // references) are not scanned; they are visible in both views.
  struct Chg {
    int64_t key;
    bool old_present;
    int32_t old_val;
    bool new_present;
    int32_t new_val;
  };
  std::vector<Chg> heavy[3];  // 0 = part, 1 = supplier, 2 = customer
  std::vector<int64_t> heavy_orders;
  // Heavy orders whose lineitems are known to be all gone: no point scan needed.
  std::unordered_set<int64_t> scan_skip;
  // Current line count of scanned (heavy) orders that remain present.
  std::unordered_map<int64_t, int32_t> scanned_lines;
  const std::shared_ptr<const TpchQ8Dimensions> old_dims = new_state->dims;

  // Net lineitem row count change per order in this transaction.
  std::unordered_map<int64_t, int32_t> dsum;
  dsum.reserve(buffer.Deltas().size());
  for (const auto &delta : buffer.Deltas())
    dsum[delta.orderkey] += delta.sign;

  // Fresh keys referenced by this transaction's lineitem deltas.
  std::unordered_set<int64_t> dref[3];
  for (const auto &delta : buffer.Deltas()) {
    if (delta.partkey > old_dims->part_build_max)
      dref[0].insert(delta.partkey);
    if (delta.suppkey > old_dims->supp_build_max)
      dref[1].insert(delta.suppkey);
    const TpchQ8Dimensions::OrderInfo *oi = nullptr;
    const auto ov = buffer.OrderInfoOverlay().find(delta.orderkey);
    if (ov != buffer.OrderInfoOverlay().end())
      oi = &ov->second;
    else
      oi = TpchQ8Val(old_dims->order_info.Find(delta.orderkey));
    if (oi && oi->custkey > old_dims->cust_build_max)
      dref[2].insert(oi->custkey);
  }

  // ── Phase 1: apply dimension changes, classify them ────────────────────
  for (const auto &[key, net] : buffer.PartNet()) {
    const int32_t *cur = NewPart(key);
    Chg c{key, cur != nullptr, cur ? *cur : 0, net.present, 0};
    if (net.present)
      c.new_val = TypeIndex(net.type);
    if (c.old_present == c.new_present && (!c.new_present || c.old_val == c.new_val))
      continue;
    const bool fresh = key > old_dims->part_build_max &&
                       !old_dims->fresh_part_ref.count(key) && !dref[0].count(key);
    if (!fresh)
      heavy[0].push_back(c);
    EnsureMutableDims();
    if (c.new_present)
      part_tx->Upsert({key, c.new_val});
    else
      part_tx->Erase(key);
  }
  auto ApplyNationNet = [&](const std::unordered_map<int64_t, TpchQ8DimNet> &nets,
                            int kind, int64_t build_max,
                            const std::unordered_set<int64_t> &ref_set) {
    for (const auto &[key, net] : nets) {
      const int32_t *cur = kind == 1 ? NewSupp(key) : NewCust(key);
      Chg c{key, cur != nullptr, cur ? *cur : 0, net.present,
            net.present ? net.nationkey : 0};
      if (c.old_present == c.new_present &&
          (!c.new_present || c.old_val == c.new_val))
        continue;
      const bool fresh =
          key > build_max && !ref_set.count(key) && !dref[kind].count(key);
      if (!fresh)
        heavy[kind].push_back(c);
      EnsureMutableDims();
      auto &mm = kind == 1 ? supp_tx : cust_tx;
      if (c.new_present)
        mm->Upsert({key, c.new_val});
      else
        mm->Erase(key);
    }
  };
  ApplyNationNet(buffer.SupplierNet(), 1, old_dims->supp_build_max,
                 old_dims->fresh_supp_ref);
  ApplyNationNet(buffer.CustomerNet(), 2, old_dims->cust_build_max,
                 old_dims->fresh_cust_ref);

  {
    std::unordered_set<int64_t> order_keys;
    for (const auto &kv : buffer.OrderInfoOverlay())
      order_keys.insert(kv.first);
    for (const auto k : buffer.OrderErase())
      order_keys.insert(k);
    for (const auto okey : order_keys) {
      const auto ov = buffer.OrderInfoOverlay().find(okey);
      const bool new_present = ov != buffer.OrderInfoOverlay().end() &&
                               !buffer.OrderErase().count(okey);
      const auto *cur = TpchQ8Val(old_dims->order_info.Find(okey));
      const bool same = (cur != nullptr) == new_present &&
                        (!new_present ||
                         (cur->custkey == ov->second.custkey &&
                          cur->orderdate_days == ov->second.orderdate_days));
      if (same)
        continue;
      // Fresh order: a new key above every key seen at build, never deleted
      // with lineitems left behind.  Its lineitems can only arrive as deltas.
      const bool fresh = new_present && !cur &&
                         okey > old_dims->order_build_max &&
                         !old_dims->deleted_orders.count(okey);
      if (!fresh) {
        heavy_orders.push_back(okey);
        if (cur && cur->lines >= 0) {
          const auto ds = dsum.find(okey);
          if (cur->lines + (ds == dsum.end() ? 0 : ds->second) == 0)
            scan_skip.insert(okey);
        }
      }
      EnsureMutableDims();
      if (new_present) {
        TpchQ8Dimensions::OrderInfo ni = ov->second;
        ni.lines = cur ? cur->lines : (fresh ? 0 : -1);
        order_tx->Upsert({okey, ni});
        mutable_dims->deleted_orders.erase(okey);
      } else {
        order_tx->Erase(okey);
      }
    }
  }

  // Heavy key sets (for dedup and for the old view).
  std::unordered_set<int64_t> hk[4];
  for (int k = 0; k < 3; ++k)
    for (const Chg &c : heavy[k])
      hk[k].insert(c.key);
  for (const auto k : heavy_orders)
    hk[3].insert(k);

  // Old view: heavy entities as before the transaction; everything else as
  // in the new dims (identical unless fresh).
  auto OldPart = [&](int64_t k) {
    return hk[0].count(k) ? TpchQ8Val(old_dims->part_key_to_type.Find(k))
                          : NewPart(k);
  };
  auto OldSupp = [&](int64_t k) {
    return hk[1].count(k) ? TpchQ8Val(old_dims->suppkey_to_nation.Find(k))
                          : NewSupp(k);
  };
  auto OldCust = [&](int64_t k) {
    return hk[2].count(k) ? TpchQ8Val(old_dims->custkey_to_nation.Find(k))
                          : NewCust(k);
  };
  auto OldOrder = [&](int64_t k) {
    return hk[3].count(k) ? TpchQ8Val(old_dims->order_info.Find(k))
                          : NewOrder(k);
  };

  // Cell of a lineitem in the old (use_old) or new view.
  // Returns 0 = not joinable / out of range, 1 = ok, -1 = unrepresentable.
  auto CellOf = [&](bool use_old, int64_t partkey, int64_t orderkey,
                    int64_t suppkey, int &yi, int32_t &pt, int32_t &cs,
                    int32_t &ss) -> int {
    const auto *oi = use_old ? OldOrder(orderkey) : NewOrder(orderkey);
    if (!oi)
      return 0;
    const int32_t year = OrderDateToYear(oi->orderdate_days);
    if (year != 1995 && year != 1996)
      return 0;
    yi = year - 1995;
    const int32_t *pp = use_old ? OldPart(partkey) : NewPart(partkey);
    const int32_t *sp = use_old ? OldSupp(suppkey) : NewSupp(suppkey);
    const int32_t *cp = use_old ? OldCust(oi->custkey) : NewCust(oi->custkey);
    if (!pp || !sp || !cp)
      return 0;
    pt = *pp;
    if (!NationSlot(*cp, nat_dim, cs) || !NationSlot(*sp, nat_dim, ss))
      return -1;
    return 1;
  };
  bool failed = false;
  auto AddCell = [&](bool use_old, int64_t partkey, int64_t orderkey,
                     int64_t suppkey, int64_t signed_vol) {
    int yi = 0;
    int32_t pt = 0, cs = 0, ss = 0;
    const int r = CellOf(use_old, partkey, orderkey, suppkey, yi, pt, cs, ss);
    if (r < 0)
      failed = true;
    else if (r > 0)
      editor.Add(yi, pt, cs, ss, signed_vol);
  };

  // ── Phase A: dimension changes applied to the current lineitems ────────
  const bool any_heavy = !heavy[0].empty() || !heavy[1].empty() ||
                         !heavy[2].empty() || !heavy_orders.empty();
  if (any_heavy && !initial_scan)
    return Rebuild();
  for (int kind = 0; kind < 4; ++kind) {
    if (kind == 3) {
      // Orders: every heavy order's lineitems in one batched scan, not one query
      // per order.  The cells are order-independent sums; the per-order row
      // counts are settled after the scan (they only touch dims fields the
      // cell lookups do not read).
      std::vector<int64_t> keys;
      keys.reserve(heavy_orders.size());
      for (const int64_t key : heavy_orders)
        if (!scan_skip.count(key))
          keys.push_back(key);
      if (keys.empty())
        continue;
      std::unordered_map<int64_t, std::size_t> rows_of;
      rows_of.reserve(keys.size());
      const auto consume_order = [&](const TpchQ8LineitemInput &row) {
        ++rows_of[row.orderkey];
        const int64_t vol =
            ComputeVolumeRaw(row.extendedprice_raw, row.discount_raw);
        AddCell(false, row.partkey, row.orderkey, row.suppkey, vol);
        AddCell(true, row.partkey, row.orderkey, row.suppkey, -vol);
      };
      if (!initial_scan->ScanLineitemsOfOrders(keys, consume_order) || failed)
        return Rebuild();
      for (const int64_t key : keys) {
        const auto it = rows_of.find(key);
        const std::size_t rows = it == rows_of.end() ? 0 : it->second;
        if (rows > 0 && !NewOrder(key)) {
          // Deleted while lineitems remain: remember so a re-insert is scanned.
          EnsureMutableDims();
          mutable_dims->deleted_orders.insert(key);
        } else if (NewOrder(key)) {
          scanned_lines[key] = static_cast<int32_t>(rows);
        }
      }
      continue;
    }
    const std::size_t n = kind < 3 ? heavy[kind].size() : heavy_orders.size();
    for (std::size_t ci = 0; ci < n; ++ci) {
      const int64_t key = kind < 3 ? heavy[kind][ci].key : heavy_orders[ci];
      if (kind == 3 && scan_skip.count(key))
        continue;
      std::size_t rows = 0;
      const auto consume = [&](const TpchQ8LineitemInput &row) {
        ++rows;
        // Dedup: a lineitem touching several changed entities is handled by
        // the highest-priority one (order > part > supplier > customer).
        if (kind == 0 && hk[3].count(row.orderkey))
          return;
        if (kind == 1 && (hk[3].count(row.orderkey) || hk[0].count(row.partkey)))
          return;
        if (kind == 2 && (hk[3].count(row.orderkey) || hk[0].count(row.partkey) ||
                          hk[1].count(row.suppkey)))
          return;
        const int64_t vol =
            ComputeVolumeRaw(row.extendedprice_raw, row.discount_raw);
        AddCell(false, row.partkey, row.orderkey, row.suppkey, vol);
        AddCell(true, row.partkey, row.orderkey, row.suppkey, -vol);
      };
      bool ok = false;
      if (kind == 0)
        ok = initial_scan->ScanLineitemsOfPart(key, consume);
      else if (kind == 1)
        ok = initial_scan->ScanLineitemsOfSupplier(key, consume);
      else if (kind == 2)
        ok = initial_scan->ScanLineitemsOfCustomer(key, consume);
      else
        ok = initial_scan->ScanLineitemsOfOrder(key, consume);
      if (!ok || failed)
        return Rebuild();
      if (kind == 3) {
        if (rows > 0 && !NewOrder(key)) {
          // Deleted while lineitems remain: remember so a re-insert is scanned.
          EnsureMutableDims();
          mutable_dims->deleted_orders.insert(key);
        } else if (NewOrder(key)) {
          scanned_lines[key] = static_cast<int32_t>(rows);
        }
      }
    }
  }

  // ── Phase B: lineitem deltas under the old view ────────────────────────
  // Each delta is resolved against the dimensions in parallel (read-only),
  // then the cube and the fresh-key bookkeeping are updated in delta order.
  struct Resolved {
    bool joined = false; // part, supplier, order and customer all resolve
    int64_t custkey = 0;
    int year_idx = -1; // -1: outside the cube's years
    int32_t part_type = 0, cust_slot = 0, supp_slot = 0;
    bool unrepresentable = false;
  };
  const auto &line_deltas = buffer.Deltas();
  std::vector<Resolved> resolved(line_deltas.size());
  core::ParallelEach(services, line_deltas.size(), kResolveGrain, [&](std::size_t i) {
    const auto &delta = line_deltas[i];
    auto &r = resolved[i];
    // dims are exact, so an unresolved entity means the lineitem does not join.
    const int32_t *pp = OldPart(delta.partkey);
    const int32_t *sp = OldSupp(delta.suppkey);
    const auto *oi = OldOrder(delta.orderkey);
    if (!pp || !sp || !oi)
      return;
    const int32_t *cp = OldCust(oi->custkey);
    if (!cp)
      return;
    r.joined = true;
    r.custkey = oi->custkey;
    const int32_t year = OrderDateToYear(oi->orderdate_days);
    if (year != 1995 && year != 1996)
      return;
    r.year_idx = year - 1995;
    r.part_type = *pp;
    r.unrepresentable = !NationSlot(*cp, nat_dim, r.cust_slot) ||
                        !NationSlot(*sp, nat_dim, r.supp_slot);
  });
  for (std::size_t i = 0; i < line_deltas.size(); ++i) {
    const auto &delta = line_deltas[i];
    const auto &r = resolved[i];
    if (!r.joined)
      continue;

    // Remember fresh keys that now have lineitems.
    if (delta.partkey > dims->part_build_max &&
        !dims->fresh_part_ref.count(delta.partkey)) {
      EnsureMutableDims();
      mutable_dims->fresh_part_ref.insert(delta.partkey);
    }
    if (delta.suppkey > dims->supp_build_max &&
        !dims->fresh_supp_ref.count(delta.suppkey)) {
      EnsureMutableDims();
      mutable_dims->fresh_supp_ref.insert(delta.suppkey);
    }
    if (r.custkey > dims->cust_build_max &&
        !dims->fresh_cust_ref.count(r.custkey)) {
      EnsureMutableDims();
      mutable_dims->fresh_cust_ref.insert(r.custkey);
    }
    if (r.year_idx < 0)
      continue;
    if (r.unrepresentable)
      return Rebuild();
    editor.Add(r.year_idx, r.part_type, r.cust_slot, r.supp_slot,
               static_cast<int64_t>(delta.sign) * delta.volume_raw);
  }

  // Keep the per-order line counts exact.
  for (const auto &sc : scanned_lines) {
    EnsureMutableDims();
    if (auto *e = TpchQ8ValMut(order_tx->Mutable(sc.first)))
      e->lines = sc.second;
  }
  for (const auto &ds : dsum) {
    if (ds.second == 0 || scanned_lines.count(ds.first))
      continue;
    const auto *cur = NewOrder(ds.first);
    if (!cur || cur->lines < 0)
      continue;
    EnsureMutableDims();
    auto *e = TpchQ8ValMut(order_tx->Mutable(ds.first));
    e->lines += ds.second;
    if (e->lines < 0)
      e->lines = -1;
  }

  if (mutable_dims) {
    const auto for_each_mid = [&](std::size_t n, const auto &fn) {
      core::ParallelEach(services, n, kFinishGrain, fn);
    };
    mutable_dims->part_key_to_type = part_tx->Finish(for_each_mid);
    mutable_dims->suppkey_to_nation = supp_tx->Finish(for_each_mid);
    mutable_dims->custkey_to_nation = cust_tx->Finish(for_each_mid);
    mutable_dims->order_info = order_tx->Finish(for_each_mid);
  }

  outcome.action = core::MaintenanceAction::ApplyDelta;
  const auto typed_state =
      std::static_pointer_cast<const TpchQ8State>(std::move(new_state));
  // Publish to both year partitions
  outcome.updates.push_back(core::StateImage::Make(
      TpchQ8Layout::kAccelerator, TpchQ8Layout::kYear1995, existing_version,
      MakeStateImage(TpchQ8ImageKind::AnnualCube, 1995, typed_state)));
  outcome.updates.push_back(core::StateImage::Make(
      TpchQ8Layout::kAccelerator, TpchQ8Layout::kYear1996, existing_version,
      MakeStateImage(TpchQ8ImageKind::AnnualCube, 1996, typed_state)));
  return outcome;
}

// ── CheckValidity ──────────────────────────────────────────────────────────
bool TpchQ8Module::CheckValidity(core::AuxEngineServices &services,
                                 const core::QueryArguments &arguments) const {
  if (arguments.values.size() < 3)
    return false;
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

// ── ResolveBindings ────────────────────────────────────────────────────────
bool TpchQ8Module::ResolveBindings(core::AuxEngineServices &services,
                                   const core::QueryArguments &arguments,
                                   std::string &nation_name,
                                   std::string &region_name,
                                   std::string &part_type) const {
  (void)services;
  if (arguments.values.size() < 3)
    return false;
  if (arguments.values[0].is_null || arguments.values[1].is_null ||
      arguments.values[2].is_null)
    return false;
  nation_name = std::get<std::string>(arguments.values[0].payload);
  region_name = std::get<std::string>(arguments.values[1].payload);
  part_type = std::get<std::string>(arguments.values[2].payload);
  return !nation_name.empty() && !region_name.empty() && !part_type.empty();
}

// ── Execute ────────────────────────────────────────────────────────────────
void TpchQ8Module::Execute(core::AuxEngineServices &services,
                           const core::QueryArguments &arguments,
                           core::AuxRowSink &sink) {
  std::string nation_name, region_name, part_type;
  if (!ResolveBindings(services, arguments, nation_name, region_name, part_type)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  if (!services.StateStore()
           .CheckValidity({Id(), {}}, services.CurrentVisibility())
           .IsValid()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  std::shared_ptr<const TpchQ8State> state;
  if (!LoadState(services, state)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  EmitResult(*state, nation_name, region_name, part_type, services, sink);
}

void TpchQ8Module::ExecuteFallback(core::AuxEngineServices &services,
                                   const core::QueryArguments &arguments,
                                   core::AuxRowSink &sink) {
  std::string nation_name, region_name, part_type;
  if (!ResolveBindings(services, arguments, nation_name, region_name, part_type)) {
    sink.Begin(core::OutputSchema{});
    sink.End();
    return;
  }

  EmitResult(BuildState(services), nation_name, region_name, part_type,
             services, sink);
}

// ── BuildState ─────────────────────────────────────────────────────────────
namespace {

// Orders of one scan partition.
struct OrdersPartial {
  TpchQ8OrderMap::Builder orders;
  int64_t max_key = 0;
};

// A partition's cube: the state and its editor (the editor keeps a reference,
// so both live behind pointers and the partial can move).
struct CubePartial {
  explicit CubePartial(int32_t nat_dim) : state(std::make_unique<TpchQ8State>()) {
    state->nat_dim = nat_dim;
    editor = std::make_unique<CubeEditor>(*state);
  }
  std::unique_ptr<TpchQ8State> state;
  std::unique_ptr<CubeEditor> editor;
};

// Adds the cells of `from` to `into` (same nation axis).
void MergeCubes(TpchQ8State &into, TpchQ8State &from) {
  if (into.pages.size() < from.pages.size())
    into.pages.resize(from.pages.size());
  for (std::size_t i = 0; i < from.pages.size(); ++i) {
    if (!from.pages[i])
      continue;
    if (!into.pages[i]) {
      into.pages[i] = std::move(from.pages[i]);
      continue;
    }
    auto sum = std::make_shared<TpchQ8CubePage>(*into.pages[i]);
    for (std::size_t c = 0; c < sum->cell.size(); ++c)
      sum->cell[c] += from.pages[i]->cell[c];
    for (std::size_t c = 0; c < sum->roll.size(); ++c)
      sum->roll[c] += from.pages[i]->roll[c];
    into.pages[i] = std::move(sum);
  }
}

} // namespace

TpchQ8State TpchQ8Module::BuildState(core::AuxEngineServices &services) const {
  TpchQ8State state;
  TpchQ8Dimensions dims;
  TpchQ8SmallMap::Builder part_b, supp_b, cust_b;
  TpchQ8OrderMap::Builder order_b;
  if (!initial_scan)
    throw std::logic_error("TPC-H Q8 aux requires a typed initial scan source");

  // Dimension dictionaries: parts (value = index into part_types)
  initial_scan->ScanParts([&](const TpchQ8PartInput &input) {
    if (input.key <= 0)
      return;
    auto ti = dims.dict->part_type_to_index.find(input.type);
    if (ti == dims.dict->part_type_to_index.end()) {
      ti = dims.dict->part_type_to_index
               .emplace(input.type, static_cast<int32_t>(dims.dict->part_types.size()))
               .first;
      dims.dict->part_types.push_back(input.type);
    }
    part_b.Add({input.key, ti->second});
    dims.part_build_max = std::max(dims.part_build_max, input.key);
  });

  int32_t max_nation = 0;
  initial_scan->ScanSuppliers([&](const TpchQ8SupplierInput &input) {
    if (input.key <= 0)
      return;
    supp_b.Add({input.key, input.nationkey});
    dims.supp_build_max = std::max(dims.supp_build_max, input.key);
    max_nation = std::max(max_nation, input.nationkey);
  });

  initial_scan->ScanCustomers([&](const TpchQ8CustomerInput &input) {
    if (input.key <= 0)
      return;
    cust_b.Add({input.key, input.nationkey});
    dims.cust_build_max = std::max(dims.cust_build_max, input.key);
    max_nation = std::max(max_nation, input.nationkey);
  });

  using scan::DateColumn;
  using scan::DecimalColumn;
  using scan::IntColumn;
  auto scanned = core::PartitionedBuild<OrdersPartial>(
      services, [] { return OrdersPartial{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanTable(services, "orders",
                                {IntColumn("o_orderkey"), IntColumn("o_custkey"),
                                 DateColumn("o_orderdate")},
                                parts, sink);
      },
      [](OrdersPartial &p, const auto &row) {
        const int64_t key = row.Int(0);
        if (key <= 0)
          return;
        p.orders.Add({key, TpchQ8OrderInfo{row.Int(1), row.Date(2), 0}});
        p.max_key = std::max(p.max_key, key);
      },
      [](OrdersPartial &into, OrdersPartial &&from) {
        into.orders.Merge(std::move(from.orders));
        into.max_key = std::max(into.max_key, from.max_key);
      });
  order_b = std::move(scanned.orders);
  dims.order_build_max = std::max(dims.order_build_max, scanned.max_key);

  // Nation/region dictionaries in one pass each.
  std::unordered_map<int32_t, std::string> regionkey_to_name;
  initial_scan->ScanRegions([&](const TpchQ8RegionInput &input) {
    regionkey_to_name[input.key] = input.name;
  });

  initial_scan->ScanNations([&](const TpchQ8NationInput &input) {
    dims.dict->nation_name_to_key[input.name] = input.key;
    max_nation = std::max(max_nation, input.key);
    const auto rit = regionkey_to_name.find(input.regionkey);
    if (rit != regionkey_to_name.end()) {
      dims.dict->nation_region[input.key] = rit->second;
      dims.dict->region_nations[rit->second].push_back(input.key);
    }
  });

  state.nat_dim = max_nation + 1;
  part_b.SortDedupe();
  supp_b.SortDedupe();
  cust_b.SortDedupe();
  order_b.SortDedupe([&](std::size_t n, const auto &fn) {
    core::ParallelEach(services, n, kSortShardGrain, fn);
  });

  // Dense key->value arrays make the per-lineitem lookups O(1) array reads.
  DenseI32 part_dense, supp_dense, cust_dense;
  part_dense.Fill(part_b, dims.part_build_max);
  supp_dense.Fill(supp_b, dims.supp_build_max);
  cust_dense.Fill(cust_b, dims.cust_build_max);

  {
    // Each partition sums into its own cube; the order index is sorted and
    // fixed, so partitions add to the per-order line counts atomically.
    auto cube = core::PartitionedBuild<CubePartial>(
        services, [&] { return CubePartial(state.nat_dim); },
        [&](std::size_t parts, auto &&sink) {
          initial_scan->ScanTable(services, "lineitem",
                                  {IntColumn("l_partkey"), IntColumn("l_orderkey"),
                                   IntColumn("l_suppkey"), DecimalColumn("l_extendedprice", 2),
                                   DecimalColumn("l_discount", 2)},
                                  parts, sink);
        },
        [&](CubePartial &p, const auto &row) {
          const int64_t orderkey = row.Int(1);
          if (orderkey <= 0)
            return;
          // Count every lineitem row of an order (exact per-order line count).
          auto *oi = TpchQ8ValMut(order_b.Find(orderkey));
          if (!oi)
            return;
          AtomicAdd<int32_t>(oi->lines, 1);
          const int64_t partkey = row.Int(0), suppkey = row.Int(2);
          if (partkey <= 0 || suppkey <= 0)
            return;
          int32_t pt_idx = 0, snk = 0, cnk = 0;
          if (!part_dense.Get(part_b, partkey, pt_idx))
            return;
          if (!cust_dense.Get(cust_b, oi->custkey, cnk))
            return;
          if (!supp_dense.Get(supp_b, suppkey, snk))
            return;
          const int32_t year = OrderDateToYear(oi->orderdate_days);
          if (year != 1995 && year != 1996)
            return;
          int32_t cs = 0, ss = 0;
          if (!NationSlot(cnk, state.nat_dim, cs) || !NationSlot(snk, state.nat_dim, ss))
            return;
          p.editor->Add(year - 1995, pt_idx, cs, ss,
                       ComputeVolumeRaw(row.Decimal(3), row.Decimal(4)));
        },
        [](CubePartial &into, CubePartial &&from) { MergeCubes(*into.state, *from.state); });
    state.pages = std::move(cube.state->pages);
  }

  dims.part_key_to_type = part_b.Finish();
  dims.suppkey_to_nation = supp_b.Finish();
  dims.custkey_to_nation = cust_b.Finish();
  dims.order_info = order_b.Finish();

  // Keep reverse maps published. PostgreSQL PreCommit cannot safely run SPI
  // InitialScan lookups, and RF inserts/deletes need order/part/supplier
  // resolution without falling back to RebuildAll.
  state.dims = std::make_shared<const TpchQ8Dimensions>(std::move(dims));
  return state;
}

// ── LoadState ──────────────────────────────────────────────────────────────
bool TpchQ8Module::LoadState(core::AuxEngineServices &services,
                             std::shared_ptr<const TpchQ8State> &state) const {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {TpchQ8Layout::kYear1995, TpchQ8Layout::kYear1996});
  for (const auto &image : snapshot.partitions) {
    const auto *typed = AsQ8Image(image, TpchQ8ImageKind::AnnualCube);
    if (!typed)
      continue;
    state = typed->state;
    return true;
  }
  return false;
}


// ── EmitResult ─────────────────────────────────────────────────────────────
// Cost: O(years x region nations) page reads, independent of cube size.
void TpchQ8Module::EmitResult(const TpchQ8State &state,
                              const std::string &nation_name,
                              const std::string &region_name,
                              const std::string &part_type,
                              core::AuxEngineServices &services,
                              core::AuxRowSink &sink) const {
  (void)services;

  // Resolve target nationkey
  int32_t target_nationkey = -1;
  const auto nn_it = state.dims->dict->nation_name_to_key.find(nation_name);
  if (nn_it != state.dims->dict->nation_name_to_key.end())
    target_nationkey = nn_it->second;

  // Resolve part type index
  const auto pt_it = state.dims->dict->part_type_to_index.find(part_type);
  if (pt_it == state.dims->dict->part_type_to_index.end()) {
    sink.Begin(core::OutputSchema{});
    sink.End();
    return;
  }
  const int32_t pt_index = pt_it->second;

  // Resolve region nations
  const auto rn_it = state.dims->dict->region_nations.find(region_name);
  if (rn_it == state.dims->dict->region_nations.end() || rn_it->second.empty()) {
    sink.Begin(core::OutputSchema{});
    sink.End();
    return;
  }
  // Distinct valid region nation keys (a handful): small on-stack array.
  int32_t region_nationkeys[64];
  std::size_t region_count = 0;
  for (const int32_t cn : rn_it->second) {
    if (cn < 0 || cn >= state.nat_dim)
      continue;
    bool dup = false;
    for (std::size_t i = 0; i < region_count; ++i)
      dup |= region_nationkeys[i] == cn;
    if (dup)
      continue;
    if (region_count == 64) {  // pathological: fall back to the generic path
      region_count = 65;
      break;
    }
    region_nationkeys[region_count++] = cn;
  }
  std::vector<int32_t> region_overflow;
  if (region_count == 65) {
    region_overflow = rn_it->second;
    std::sort(region_overflow.begin(), region_overflow.end());
    region_overflow.erase(
        std::unique(region_overflow.begin(), region_overflow.end()),
        region_overflow.end());
  }
  const int32_t *rk_begin = region_count == 65 ? region_overflow.data()
                                               : region_nationkeys;
  const int32_t *rk_end = rk_begin + (region_count == 65 ? region_overflow.size()
                                                         : region_count);

  // Aggregate: year → (nation_volume, total_volume)
  struct YearAgg {
    int64_t nation_volume_raw = 0;
    int64_t total_volume_raw = 0;
  };
  YearAgg year_aggs[2];
  static constexpr int32_t kYear1995 = 1995;
  static constexpr int32_t kYear1996 = 1996;

  const std::size_t axis = static_cast<std::size_t>(AxisOf(state));
  const bool have_target =
      target_nationkey >= 0 && target_nationkey < state.nat_dim;
  for (int yi = 0; yi < 2; ++yi) {
    const std::size_t idx = static_cast<std::size_t>(pt_index) * 2 +
                            static_cast<std::size_t>(yi);
    if (idx >= state.pages.size() || !state.pages[idx])
      continue;
    const TpchQ8CubePage &page = *state.pages[idx];
    for (const int32_t *cnp = rk_begin; cnp != rk_end; ++cnp) {
      const int32_t cn = *cnp;
      // Customer nation must be in region (region nations are valid keys)
      if (cn < 0 || cn >= state.nat_dim)
        continue;
      const std::size_t c = static_cast<std::size_t>(cn);
      year_aggs[yi].total_volume_raw += page.roll[c];
      if (have_target)
        year_aggs[yi].nation_volume_raw +=
            page.cell[c * axis + static_cast<std::size_t>(target_nationkey)];
    }
  }

  // Build output rows sorted by year.
  // SQL GROUP BY only emits years with at least one qualifying fact row;
  // a year with no denominator does not appear in the result set.
  TpchQ8OutputRow output_rows[2];
  int output_count = 0;
  for (int i = 0; i < 2; ++i) {
    const auto &agg = year_aggs[i];
    if (agg.total_volume_raw == 0)
      continue;
    const int64_t year = i == 0 ? kYear1995 : kYear1996;
    // nation_volume_raw / total_volume_raw; scales cancel.
    double mkt_share = static_cast<double>(agg.nation_volume_raw) /
                       static_cast<double>(agg.total_volume_raw);
    output_rows[output_count++] = {year, mkt_share};
  }

  sink.Begin(core::OutputSchema{});
  for (int ri = 0; ri < output_count; ++ri) {
    const auto &row = output_rows[ri];
    core::AuxRow out;
    out.values.reserve(2);
    out.values.push_back(core::AuxValue::Of(core::AuxType::Int64, row.o_year));
    out.values.push_back(core::AuxValue::Of(core::AuxType::Double,
                                            row.mkt_share));
    sink.WriteRow(out);
  }
  sink.End();
}

std::vector<core::CalibrationWrite> TpchQ8Module::CalibrationWrites() const {
  return TpchQ8CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
