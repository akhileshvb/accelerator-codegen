#include "tpch/q8/postgres/aux/module.hpp"
#include "tpch/q8/calibration_writes.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "accelerator_codegen/stateful/core/pg_numeric_div.hpp"
#include "accelerator_codegen/stateful/core/query_module_helpers.hpp"
#include "accelerator_codegen/stateful/core/sql_date.hpp"

namespace accelerator_codegen::aux_state::query {

namespace {

namespace sql_date = core::sql_date;
using Int128 = ::accelerator_codegen::pg_numeric::Int128;

constexpr core::PartitionId kStatePartition = TpchQ8Layout::kYear1995;
// volume = price * (1 - discount): numeric(15,2) * numeric(15,2) -> scale 4.
constexpr int32_t kVolumeScale = 4;

// ---- Row decoding (mutation capture rows) ---------------------------------
// A cell is Ok, SQL NULL (the row cannot join / pass a predicate), or of an
// unexpected type (the state cannot follow the write: it is rebuilt).
enum class Cell { Ok, Null, Bad };

Cell IntCell(const core::AuxRow &row, std::size_t index, int64_t &out) {
  if (index >= row.values.size())
    return Cell::Bad;
  const auto &cell = row.values[index];
  if (cell.is_null)
    return Cell::Null;
  if (std::holds_alternative<int64_t>(cell.payload)) {
    out = std::get<int64_t>(cell.payload);
    return Cell::Ok;
  }
  if (std::holds_alternative<uint64_t>(cell.payload)) {
    const auto value = std::get<uint64_t>(cell.payload);
    if (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
      return Cell::Bad;
    out = static_cast<int64_t>(value);
    return Cell::Ok;
  }
  return Cell::Bad;
}

// A nation / region key (int32 column). INT32_MIN is the state's "absent"
// marker, so it cannot be stored.
Cell KeyCell(const core::AuxRow &row, std::size_t index, int32_t &out) {
  int64_t v = 0;
  const Cell c = IntCell(row, index, v);
  if (c != Cell::Ok)
    return c;
  if (v <= std::numeric_limits<int32_t>::min() ||
      v > std::numeric_limits<int32_t>::max())
    return Cell::Bad;
  out = static_cast<int32_t>(v);
  return Cell::Ok;
}

Cell DecimalCell(const core::AuxRow &row, std::size_t index, int64_t &raw,
                 int32_t &scale) {
  if (index >= row.values.size())
    return Cell::Bad;
  const auto &cell = row.values[index];
  if (cell.is_null)
    return Cell::Null;
  if (!std::holds_alternative<core::ScaledDecimal>(cell.payload))
    return Cell::Bad;
  const auto decimal = std::get<core::ScaledDecimal>(cell.payload);
  raw = decimal.value;
  scale = decimal.scale;
  return Cell::Ok;
}

Cell DateCell(const core::AuxRow &row, std::size_t index, int32_t &days) {
  if (index >= row.values.size())
    return Cell::Bad;
  const auto &cell = row.values[index];
  if (cell.is_null)
    return Cell::Null;
  if (!std::holds_alternative<core::AuxDate>(cell.payload))
    return Cell::Bad;
  days = std::get<core::AuxDate>(cell.payload).days_since_epoch;
  return Cell::Ok;
}

Cell TextCell(const core::AuxRow &row, std::size_t index, std::string &out) {
  if (index >= row.values.size())
    return Cell::Bad;
  const auto &cell = row.values[index];
  if (cell.is_null)
    return Cell::Null;
  if (!std::holds_alternative<std::string>(cell.payload))
    return Cell::Bad;
  out = std::get<std::string>(cell.payload);
  return Cell::Ok;
}

// price * (1 - discount) as a scale-4 integer; false on overflow or when the
// operand scales do not give scale 4.
bool VolumeRaw(int64_t price_raw, int32_t price_scale, int64_t discount_raw,
               int32_t discount_scale, int64_t &out) {
  if (price_scale + discount_scale != kVolumeScale || discount_scale < 0 ||
      discount_scale > 9)
    return false;
  int64_t one = 1;
  for (int32_t i = 0; i < discount_scale; ++i)
    one *= 10;
  int64_t factor = 0;
  if (__builtin_sub_overflow(one, discount_raw, &factor))
    return false;
  return !__builtin_mul_overflow(price_raw, factor, &out);
}

// o_orderdate BETWEEN 1995-01-01 AND 1996-12-31
bool InWindow(int32_t day) {
  static const int64_t first = sql_date::DaysFromCivil(kTpchQ8MinYear, 1, 1);
  static const int64_t last = sql_date::DaysFromCivil(kTpchQ8MaxYear, 12, 31);
  return day >= first && day <= last;
}

// ---- Order entry: the one blob kept per orderkey ---------------------------
struct LineRec {
  int64_t partkey = 0;
  int64_t suppkey = 0;
  int64_t volume = 0;
  bool operator==(const LineRec &o) const {
    return partkey == o.partkey && suppkey == o.suppkey && volume == o.volume;
  }
};

// Forms (invariants):
//   records      !counts_only: `lines` are all lineitems of the order. If
//                present, the order is in the window (and contributes).
//   counts_only  the order is out of the window (present) or its row was
//                deleted while it had lineitems (!present): only the number of
//                lineitems is kept.
struct OrderEntry {
  bool present = false;
  bool counts_only = false;
  int64_t custkey = 0;
  int32_t day = 0;
  std::vector<LineRec> lines;
  uint64_t nlines = 0; // counts_only
  bool Empty() const {
    return !present && (counts_only ? nlines == 0 : lines.empty());
  }
};

constexpr uint8_t kFlagPresent = 1;
constexpr uint8_t kFlagCountsOnly = 2;

void EncodeEntry(const OrderEntry &e, std::vector<uint8_t> &out) {
  out.clear();
  out.push_back(static_cast<uint8_t>((e.present ? kFlagPresent : 0) |
                                     (e.counts_only ? kFlagCountsOnly : 0)));
  if (e.present) {
    core::PutSignedVarint(out, e.custkey);
    core::PutSignedVarint(out, e.day);
  }
  if (e.counts_only) {
    core::PutVarint(out, e.nlines);
    return;
  }
  for (const auto &l : e.lines) {
    core::PutVarint(out, static_cast<uint64_t>(l.partkey));
    core::PutVarint(out, static_cast<uint64_t>(l.suppkey));
    core::PutSignedVarint(out, l.volume);
  }
}

bool DecodeEntry(std::string_view v, OrderEntry &e) {
  e = OrderEntry{};
  const auto *p = reinterpret_cast<const uint8_t *>(v.data());
  const auto *end = p + v.size();
  if (p >= end)
    return false;
  const uint8_t flags = *p++;
  e.present = (flags & kFlagPresent) != 0;
  e.counts_only = (flags & kFlagCountsOnly) != 0;
  if (e.present) {
    int64_t day = 0;
    if (!core::GetSignedVarint(p, end, e.custkey) ||
        !core::GetSignedVarint(p, end, day))
      return false;
    e.day = static_cast<int32_t>(day);
  }
  if (e.counts_only)
    return core::GetVarint(p, end, e.nlines);
  while (p < end) {
    LineRec l;
    uint64_t partkey = 0, suppkey = 0;
    if (!core::GetVarint(p, end, partkey) || !core::GetVarint(p, end, suppkey) ||
        !core::GetSignedVarint(p, end, l.volume))
      return false;
    l.partkey = static_cast<int64_t>(partkey);
    l.suppkey = static_cast<int64_t>(suppkey);
    e.lines.push_back(l);
  }
  return true;
}

bool LoadEntry(const TpchQ8State &s, int64_t orderkey, OrderEntry &e) {
  std::string_view v;
  if (!s.orders.Get(orderkey, v)) {
    e = OrderEntry{};
    return true;
  }
  return DecodeEntry(v, e);
}

bool StoreEntry(TpchQ8State &s, int64_t orderkey, const OrderEntry &e) {
  if (orderkey < 0)
    return false;
  if (e.Empty()) {
    s.orders.Erase(orderkey);
    return true;
  }
  thread_local std::vector<uint8_t> buf;
  EncodeEntry(e, buf);
  return s.orders.Set(orderkey, buf);
}

// ---- Cube --------------------------------------------------------------------
void Contribute(TpchQ8State &s, uint32_t type_id, int32_t year, int32_t cust_nk,
                int32_t supp_nk, int64_t volume, int sign) {
  auto &cube = s.cubes.Mutable(type_id);
  if (!cube)
    cube = std::make_shared<TpchQ8TypeCube>();
  else if (cube.use_count() > 1)
    cube = std::make_shared<TpchQ8TypeCube>(*cube);
  auto it = std::lower_bound(
      cube->begin(), cube->end(), std::make_tuple(year, cust_nk, supp_nk),
      [](const TpchQ8CubeCell &c, const std::tuple<int32_t, int32_t, int32_t> &k) {
        return std::make_tuple(c.year, c.customer_nationkey,
                               c.supplier_nationkey) < k;
      });
  if (it == cube->end() || it->year != year || it->customer_nationkey != cust_nk ||
      it->supplier_nationkey != supp_nk)
    it = cube->insert(it, TpchQ8CubeCell{year, cust_nk, supp_nk, 0, 0});
  it->volume += static_cast<Int128>(sign) * volume;
  it->rows += sign;
  if (it->rows == 0 && it->volume == 0)
    cube->erase(it);
}

// The cube contribution of a record of the present, in-window order `e`.
void ContributeRec(TpchQ8State &s, const OrderEntry &e, const LineRec &r,
                   int sign) {
  const uint32_t type_id = s.part_type.Get(r.partkey);
  const int32_t snk = s.supplier_nation.Get(r.suppkey);
  const int32_t cnk = s.customer_nation.Get(e.custkey);
  if (type_id == TpchQ8State::kNoType || snk == TpchQ8State::kAbsent ||
      cnk == TpchQ8State::kAbsent)
    return;
  Contribute(s, type_id, sql_date::YearOf(e.day), cnk, snk, r.volume, sign);
}

void AddRef(core::ChunkedCowArray<uint32_t> &refs, int64_t key, int sign) {
  auto &r = refs.Mutable(key);
  if (sign > 0)
    ++r;
  else if (r > 0)
    --r;
}

bool KeyOk(int64_t key) { return key >= 0 && key < (int64_t{1} << 62); }

// ---- Incremental maintenance primitives ------------------------------------
// Each returns false when the write cannot be applied exactly to this state;
// the caller then rebuilds.

bool ApplyLineitem(TpchQ8State &s, int64_t orderkey, int64_t partkey,
                   int64_t suppkey, int64_t volume, int sign) {
  if (!KeyOk(orderkey) || !KeyOk(partkey) || !KeyOk(suppkey))
    return false;
  OrderEntry e;
  if (!LoadEntry(s, orderkey, e))
    return false;
  if (e.counts_only) {
    if (sign > 0)
      ++e.nlines;
    else if (e.nlines > 0)
      --e.nlines;
    else
      return false;
    return StoreEntry(s, orderkey, e);
  }
  const LineRec rec{partkey, suppkey, volume};
  if (sign > 0) {
    e.lines.push_back(rec);
  } else {
    const auto it = std::find(e.lines.begin(), e.lines.end(), rec);
    if (it == e.lines.end())
      return false;
    e.lines.erase(it);
  }
  if (!StoreEntry(s, orderkey, e))
    return false;
  AddRef(s.part_refs, partkey, sign);
  AddRef(s.supplier_refs, suppkey, sign);
  if (e.present)
    ContributeRec(s, e, rec, sign);
  return true;
}

bool ApplyOrder(TpchQ8State &s, int64_t orderkey, int64_t custkey, int32_t day,
                int sign) {
  if (!KeyOk(orderkey) || !KeyOk(custkey))
    return false;
  OrderEntry e;
  if (!LoadEntry(s, orderkey, e))
    return false;
  if (sign > 0) {
    if (e.present)
      return false; // duplicate order key
    if (InWindow(day)) {
      if (e.counts_only) {
        // Lineitems of an out-of-window order are not retained.
        if (e.nlines > 0)
          return false;
        e = OrderEntry{};
      }
      e.present = true;
      e.custkey = custkey;
      e.day = day;
      AddRef(s.customer_refs, custkey, +1);
      for (const auto &r : e.lines)
        ContributeRec(s, e, r, +1);
    } else {
      if (!e.counts_only) {
        // Orphan lineitems of a not-yet-present order: they cannot contribute
        // out of the window; keep only their count.
        for (const auto &r : e.lines) {
          AddRef(s.part_refs, r.partkey, -1);
          AddRef(s.supplier_refs, r.suppkey, -1);
        }
        e.nlines = e.lines.size();
        e.lines.clear();
        e.counts_only = true;
      }
      e.present = true;
      e.custkey = custkey;
      e.day = day;
    }
    return StoreEntry(s, orderkey, e);
  }
  if (!e.present || e.custkey != custkey || e.day != day)
    return false;
  if (!e.counts_only) {
    for (const auto &r : e.lines)
      ContributeRec(s, e, r, -1);
    AddRef(s.customer_refs, custkey, -1);
  }
  e.present = false;
  return StoreEntry(s, orderkey, e);
}

// Dimension rows changed in one write batch: key -> value before / after
// (absent marker when the row does not exist before / after).
template <typename V> struct Change {
  V before;
  V after;
};
template <typename V> using Changes = std::map<int64_t, Change<V>>;

// Visits every record of every present in-window order. `fn(entry, record)`.
// Iterates a shallow copy, so the callback may modify the state.
template <typename F> void ForEachContributingRecord(TpchQ8State &s, F &&fn) {
  const core::ChunkedBlobMap snapshot = s.orders;
  snapshot.ForEach([&](int64_t, std::string_view v) {
    OrderEntry e;
    if (!DecodeEntry(v, e) || !e.present || e.counts_only)
      return;
    for (const auto &r : e.lines)
      fn(e, r);
  });
}

// Part types changed: move the contribution of the referenced parts' lineitems
// from the old type's cube to the new one's.
void RetypeParts(TpchQ8State &s, const Changes<uint32_t> &changes) {
  std::unordered_map<int64_t, Change<uint32_t>> hot;
  for (const auto &[partkey, change] : changes)
    if (s.part_refs.Get(partkey) > 0)
      hot.emplace(partkey, change);
  if (hot.empty())
    return;
  ForEachContributingRecord(s, [&](const OrderEntry &e, const LineRec &r) {
    const auto it = hot.find(r.partkey);
    if (it == hot.end())
      return;
    const int32_t snk = s.supplier_nation.Get(r.suppkey);
    const int32_t cnk = s.customer_nation.Get(e.custkey);
    if (snk == TpchQ8State::kAbsent || cnk == TpchQ8State::kAbsent)
      return;
    const int32_t year = sql_date::YearOf(e.day);
    if (it->second.before != TpchQ8State::kNoType)
      Contribute(s, it->second.before, year, cnk, snk, r.volume, -1);
    if (it->second.after != TpchQ8State::kNoType)
      Contribute(s, it->second.after, year, cnk, snk, r.volume, +1);
  });
}

void RepointSuppliers(TpchQ8State &s, const Changes<int32_t> &changes) {
  std::unordered_map<int64_t, Change<int32_t>> hot;
  for (const auto &[suppkey, change] : changes)
    if (s.supplier_refs.Get(suppkey) > 0)
      hot.emplace(suppkey, change);
  if (hot.empty())
    return;
  ForEachContributingRecord(s, [&](const OrderEntry &e, const LineRec &r) {
    const auto it = hot.find(r.suppkey);
    if (it == hot.end())
      return;
    const uint32_t type_id = s.part_type.Get(r.partkey);
    const int32_t cnk = s.customer_nation.Get(e.custkey);
    if (type_id == TpchQ8State::kNoType || cnk == TpchQ8State::kAbsent)
      return;
    const int32_t year = sql_date::YearOf(e.day);
    if (it->second.before != TpchQ8State::kAbsent)
      Contribute(s, type_id, year, cnk, it->second.before, r.volume, -1);
    if (it->second.after != TpchQ8State::kAbsent)
      Contribute(s, type_id, year, cnk, it->second.after, r.volume, +1);
  });
}

void RepointCustomers(TpchQ8State &s, const Changes<int32_t> &changes) {
  std::unordered_map<int64_t, Change<int32_t>> hot;
  for (const auto &[custkey, change] : changes)
    if (s.customer_refs.Get(custkey) > 0)
      hot.emplace(custkey, change);
  if (hot.empty())
    return;
  ForEachContributingRecord(s, [&](const OrderEntry &e, const LineRec &r) {
    const auto it = hot.find(e.custkey);
    if (it == hot.end())
      return;
    const uint32_t type_id = s.part_type.Get(r.partkey);
    const int32_t snk = s.supplier_nation.Get(r.suppkey);
    if (type_id == TpchQ8State::kNoType || snk == TpchQ8State::kAbsent)
      return;
    const int32_t year = sql_date::YearOf(e.day);
    if (it->second.before != TpchQ8State::kAbsent)
      Contribute(s, type_id, year, it->second.before, snk, r.volume, -1);
    if (it->second.after != TpchQ8State::kAbsent)
      Contribute(s, type_id, year, it->second.after, snk, r.volume, +1);
  });
}

// Net before / after nation key per supplier / customer key of a mutation
// (rows: [key, nationkey]). A NULL nation key never joins n_nationkey: the
// row exists but is inert (same as absent for the cube).
bool CollectNationChanges(const core::MutationBatch &m, Changes<int32_t> &out) {
  const auto add = [&](const std::vector<core::AuxRow> &rows, bool before) {
    for (const auto &row : rows) {
      int64_t key = 0;
      int32_t nation = 0;
      const Cell k = IntCell(row, 0, key);
      const Cell n = KeyCell(row, 1, nation);
      if (k == Cell::Bad || n == Cell::Bad || (k == Cell::Ok && !KeyOk(key)))
        return false;
      if (k != Cell::Ok)
        continue;
      auto &change = out.try_emplace(key, Change<int32_t>{TpchQ8State::kAbsent,
                                                           TpchQ8State::kAbsent})
                         .first->second;
      (before ? change.before : change.after) =
          n == Cell::Null ? TpchQ8State::kAbsent : nation;
    }
    return true;
  };
  return add(m.old_rows, true) && add(m.new_rows, false);
}

// Interns `type` into the working state's dictionary (cloning the shared
// dictionary first when it gets a new entry).
uint32_t InternType(TpchQ8State &s, const std::string &type) {
  const uint32_t found = s.types->Find(type);
  if (found != core::StringDict::kNone)
    return found;
  if (s.types.use_count() > 1)
    s.types = std::make_shared<core::StringDict>(*s.types);
  return s.types->Intern(type);
}

} // namespace

core::AcceleratorId TpchQ8Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ8Module::Dependencies() const {
  // Projected columns (physical attribute numbers):
  //   part     : p_partkey, p_type
  //   supplier : s_suppkey, s_nationkey
  //   customer : c_custkey, c_nationkey
  //   orders   : o_orderkey, o_custkey, o_orderdate
  //   lineitem : l_partkey, l_orderkey, l_suppkey, l_extendedprice, l_discount
  //   nation   : n_nationkey, n_name, n_regionkey
  //   region   : r_regionkey, r_name
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

std::shared_ptr<TpchQ8State> TpchQ8Module::BuildState() const {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q8 requires a typed initial scan source");
  auto state = std::make_shared<TpchQ8State>();
  TpchQ8State &s = *state;

  initial_scan->ScanNations([&](const TpchQ8NationInput &input) {
    s.nations[input.key] = {input.name, input.regionkey};
  });
  initial_scan->ScanRegions([&](const TpchQ8RegionInput &input) {
    s.regions[input.key] = input.name;
  });
  initial_scan->ScanParts([&](const TpchQ8PartInput &input) {
    if (!KeyOk(input.key))
      throw std::runtime_error("TPC-H Q8: unsupported p_partkey");
    s.part_type.Set(input.key, InternType(s, input.type));
  });
  initial_scan->ScanSuppliers([&](const TpchQ8SupplierInput &input) {
    if (!KeyOk(input.key) || input.nationkey == TpchQ8State::kAbsent)
      throw std::runtime_error("TPC-H Q8: unsupported supplier row");
    s.supplier_nation.Set(input.key, input.nationkey);
  });
  initial_scan->ScanCustomers([&](const TpchQ8CustomerInput &input) {
    if (!KeyOk(input.key) || input.nationkey == TpchQ8State::kAbsent)
      throw std::runtime_error("TPC-H Q8: unsupported customer row");
    s.customer_nation.Set(input.key, input.nationkey);
  });
  initial_scan->ScanOrders([&](const TpchQ8OrdersInput &input) {
    if (!ApplyOrder(s, input.key, input.custkey, input.orderdate_days, +1))
      throw std::runtime_error("TPC-H Q8: unsupported orders row");
  });
  initial_scan->ScanLineitems([&](const TpchQ8LineitemInput &input) {
    int64_t volume = 0;
    if (!VolumeRaw(input.extendedprice_raw, 2, input.discount_raw, 2, volume) ||
        !ApplyLineitem(s, input.orderkey, input.partkey, input.suppkey, volume,
                       +1))
      throw std::runtime_error("TPC-H Q8: unsupported lineitem row");
  });
  s.orders.Shrink();
  return state;
}

void TpchQ8Module::Build(core::AuxEngineServices &, core::StateBuilder &builder) {
  auto image = std::make_shared<TpchQ8StateImage>();
  image->state = BuildState();
  builder.Write(core::StateImage::Make(
      TpchQ8Layout::kAccelerator, kStatePartition, 0,
      std::shared_ptr<const TpchQ8StateImage>(std::move(image))));
  builder.Finish();
}

core::InvalidationPlan
TpchQ8Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.part_table ||
      mutation.table == config.supplier_table ||
      mutation.table == config.customer_table ||
      mutation.table == config.orders_table ||
      mutation.table == config.lineitem_table ||
      mutation.table == config.nation_table ||
      mutation.table == config.region_table) {
    core::InvalidationPlan plan;
    plan.scope =
        ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
    plan.partitions.insert(kStatePartition);
    return plan;
  }
  return {};
}

void TpchQ8Module::AccumulateMutation(const core::MutationBatch &mutation,
                                      core::AuxEngineServices &services,
                                      core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ8TxnBuffer &>(base_buffer);
  if (buffer.requires_rebuild)
    return;
  if (!buffer.work) {
    // Fork the published state (pointer copies) for this transaction.
    const auto snapshot = services.StateStore().ReadPartitions(
        services.CurrentVisibility(), {kStatePartition});
    for (const auto &image : snapshot.partitions) {
      const auto *typed = image.partition == kStatePartition
                              ? image.As<TpchQ8StateImage>()
                              : nullptr;
      if (typed && typed->state) {
        buffer.work = std::make_shared<TpchQ8State>(*typed->state);
        buffer.base_version = image.version;
        break;
      }
    }
    if (!buffer.work) {
      buffer.MarkRebuild();
      return;
    }
  }
  TpchQ8State &s = *buffer.work;
  buffer.pending = true;
  buffer.rows += core::MutationRowCount(mutation);

  bool ok = true;
  if (mutation.table == config.nation_table) {
    // Rows: nationkey, name, regionkey. A NULL name / region key makes the
    // nation unable to join / match: it is dropped from the map.
    for (const auto &row : mutation.old_rows) {
      int32_t key = 0;
      const Cell c = KeyCell(row, 0, key);
      if (c == Cell::Bad) {
        ok = false;
        break;
      }
      if (c == Cell::Ok)
        s.nations.erase(key);
    }
    for (const auto &row : mutation.new_rows) {
      int32_t key = 0, regionkey = 0;
      std::string name;
      const Cell k = KeyCell(row, 0, key);
      const Cell n = TextCell(row, 1, name);
      const Cell r = KeyCell(row, 2, regionkey);
      if (k == Cell::Bad || n == Cell::Bad || r == Cell::Bad) {
        ok = false;
        break;
      }
      if (k == Cell::Ok && n == Cell::Ok && r == Cell::Ok)
        s.nations[key] = {name, regionkey};
    }
  } else if (mutation.table == config.region_table) {
    for (const auto &row : mutation.old_rows) {
      int32_t key = 0;
      const Cell c = KeyCell(row, 0, key);
      if (c == Cell::Bad) {
        ok = false;
        break;
      }
      if (c == Cell::Ok)
        s.regions.erase(key);
    }
    for (const auto &row : mutation.new_rows) {
      int32_t key = 0;
      std::string name;
      const Cell k = KeyCell(row, 0, key);
      const Cell n = TextCell(row, 1, name);
      if (k == Cell::Bad || n == Cell::Bad) {
        ok = false;
        break;
      }
      if (k == Cell::Ok && n == Cell::Ok)
        s.regions[key] = name;
    }
  } else if (mutation.table == config.part_table) {
    // Rows: partkey, type. A NULL type never equals the parameter.
    Changes<uint32_t> changes;
    const auto collect = [&](const std::vector<core::AuxRow> &rows, bool before) {
      for (const auto &row : rows) {
        int64_t key = 0;
        std::string type;
        const Cell k = IntCell(row, 0, key);
        const Cell t = TextCell(row, 1, type);
        if (k == Cell::Bad || t == Cell::Bad || (k == Cell::Ok && !KeyOk(key)))
          return false;
        if (k != Cell::Ok)
          continue;
        uint32_t id = TpchQ8State::kNoType;
        if (t == Cell::Ok) {
          id = before ? s.types->Find(type) : InternType(s, type);
          if (before && id == core::StringDict::kNone)
            return false; // the state has never seen this type
        }
        auto &change = changes
                           .try_emplace(key, Change<uint32_t>{TpchQ8State::kNoType,
                                                              TpchQ8State::kNoType})
                           .first->second;
        (before ? change.before : change.after) = id;
      }
      return true;
    };
    ok = collect(mutation.old_rows, true) && collect(mutation.new_rows, false);
    for (auto it = changes.begin(); ok && it != changes.end();) {
      if (it->second.before != s.part_type.Get(it->first)) {
        ok = false;
      } else if (it->second.before == it->second.after) {
        it = changes.erase(it);
      } else {
        s.part_type.Set(it->first, it->second.after);
        ++it;
      }
    }
    if (ok)
      RetypeParts(s, changes);
  } else if (mutation.table == config.supplier_table ||
             mutation.table == config.customer_table) {
    const bool supplier = mutation.table == config.supplier_table;
    Changes<int32_t> changes;
    ok = CollectNationChanges(mutation, changes);
    auto &arr = supplier ? s.supplier_nation : s.customer_nation;
    for (auto it = changes.begin(); ok && it != changes.end();) {
      if (it->second.before != arr.Get(it->first)) {
        ok = false;
      } else if (it->second.before == it->second.after) {
        it = changes.erase(it);
      } else {
        arr.Set(it->first, it->second.after);
        ++it;
      }
    }
    if (ok) {
      if (supplier)
        RepointSuppliers(s, changes);
      else
        RepointCustomers(s, changes);
    }
  } else if (mutation.table == config.orders_table) {
    const auto apply = [&](const std::vector<core::AuxRow> &rows, int sign) {
      for (const auto &row : rows) {
        int64_t orderkey = 0, custkey = 0;
        int32_t day = 0;
        const Cell a = IntCell(row, 0, orderkey);
        const Cell b = IntCell(row, 1, custkey);
        const Cell d = DateCell(row, 2, day);
        if (a == Cell::Bad || b == Cell::Bad || d == Cell::Bad)
          return false;
        // A NULL key or date never passes the join / window predicate.
        if (a == Cell::Null || b == Cell::Null || d == Cell::Null)
          continue;
        if (!ApplyOrder(s, orderkey, custkey, day, sign))
          return false;
      }
      return true;
    };
    ok = apply(mutation.old_rows, -1) && apply(mutation.new_rows, +1);
  } else if (mutation.table == config.lineitem_table) {
    const auto apply = [&](const std::vector<core::AuxRow> &rows, int sign) {
      for (const auto &row : rows) {
        int64_t partkey = 0, orderkey = 0, suppkey = 0, price = 0, discount = 0;
        int32_t price_scale = 0, discount_scale = 0;
        const Cell cells[5] = {IntCell(row, 0, partkey),
                               IntCell(row, 1, orderkey),
                               IntCell(row, 2, suppkey),
                               DecimalCell(row, 3, price, price_scale),
                               DecimalCell(row, 4, discount, discount_scale)};
        bool null_cell = false;
        for (const Cell c : cells) {
          if (c == Cell::Bad)
            return false;
          null_cell = null_cell || c == Cell::Null;
        }
        if (null_cell)
          continue; // never joins / NULL volume
        int64_t volume = 0;
        if (!VolumeRaw(price, price_scale, discount, discount_scale, volume) ||
            !ApplyLineitem(s, orderkey, partkey, suppkey, volume, sign))
          return false;
      }
      return true;
    };
    ok = apply(mutation.old_rows, -1) && apply(mutation.new_rows, +1);
  }
  if (!ok)
    buffer.MarkRebuild();
}

core::StagedMaintenanceOutcome
TpchQ8Module::StageMaintenance(core::AuxEngineServices &services,
                               const core::InvalidationPlan &,
                               core::AuxTxnBuffer &base_buffer) {
  core::StagedMaintenanceOutcome outcome;
  auto &buffer = dynamic_cast<TpchQ8TxnBuffer &>(base_buffer);
  const auto rebuild = [&] {
    core::FinishRebuildAll(outcome);
    return outcome;
  };
  if (buffer.RequiresRebuild())
    return rebuild();
  if (!buffer.HasPendingChanges() || !buffer.work)
    return outcome;

  const auto decision = services.ChooseMaintenance(
      {std::max<uint64_t>(1, buffer.rows), 1, true, false, true, false, false,
       core::MaintenanceAction::NoOp, services.ExpectedQueriesBeforeNextWrite()});
  if (decision.action != core::MaintenanceAction::ApplyDelta)
    return rebuild();

  auto image = std::make_shared<TpchQ8StateImage>();
  image->state = std::move(buffer.work);
  outcome.action = core::MaintenanceAction::ApplyDelta;
  outcome.updates.push_back(core::StateImage::Make(
      TpchQ8Layout::kAccelerator, kStatePartition, buffer.base_version,
      std::shared_ptr<const TpchQ8StateImage>(std::move(image))));
  return outcome;
}

bool TpchQ8Module::CheckValidity(core::AuxEngineServices &services,
                                 const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void TpchQ8Module::Execute(core::AuxEngineServices &services,
                           const core::QueryArguments &arguments,
                           core::AuxRowSink &sink) {
  if (!CheckValidity(services, arguments)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {kStatePartition});
  for (const auto &image : snapshot.partitions) {
    const auto *typed = image.partition == kStatePartition
                            ? image.As<TpchQ8StateImage>()
                            : nullptr;
    if (typed && typed->state) {
      const auto hold = image.payload; // keep the state alive while emitting
      EmitResult(*typed->state, arguments, sink);
      return;
    }
  }
  ExecuteFallback(services, arguments, sink);
}

void TpchQ8Module::ExecuteFallback(core::AuxEngineServices &,
                                   const core::QueryArguments &arguments,
                                   core::AuxRowSink &sink) {
  const auto state = BuildState();
  EmitResult(*state, arguments, sink);
}

void TpchQ8Module::EmitResult(const TpchQ8State &s,
                              const core::QueryArguments &arguments,
                              core::AuxRowSink &sink) const {
  sink.Begin({{{"o_year", core::AuxType::Int64, false},
               {"mkt_share", core::AuxType::String, false}}});

  // NULL parameters make their comparison NULL: nothing matches (for the
  // nation parameter: the CASE never takes its THEN branch).
  const auto text_arg = [&](std::size_t i, std::string &out) {
    if (i >= arguments.values.size() || arguments.values[i].is_null ||
        !std::holds_alternative<std::string>(arguments.values[i].payload))
      return false;
    out = std::get<std::string>(arguments.values[i].payload);
    return true;
  };
  std::string nation, region, type;
  const bool have_nation = text_arg(0, nation);
  const bool have_region = text_arg(1, region);
  const bool have_type = text_arg(2, type);

  Int128 total[2] = {0, 0}, nation_sum[2] = {0, 0};
  int64_t total_rows[2] = {0, 0}, nation_rows[2] = {0, 0};
  const uint32_t type_id = have_type ? s.types->Find(type) : core::StringDict::kNone;
  if (have_region && type_id != core::StringDict::kNone) {
    // r_name = region; n1 (customer nation) in such a region; n2 (supplier
    // nation) is any existing nation, matched against the parameter by name.
    std::set<int32_t> region_keys;
    for (const auto &[key, name] : s.regions)
      if (name == region)
        region_keys.insert(key);
    const auto *cube = s.cubes.Find(type_id);
    if (cube && *cube) {
      for (const auto &cell : **cube) {
        const int y = cell.year - kTpchQ8MinYear;
        const auto cust = s.nations.find(cell.customer_nationkey);
        const auto supp = s.nations.find(cell.supplier_nationkey);
        if (y < 0 || y > 1 || cust == s.nations.end() ||
            supp == s.nations.end() || !region_keys.count(cust->second.regionkey))
          continue;
        total[y] += cell.volume;
        total_rows[y] += cell.rows;
        if (have_nation && supp->second.name == nation) {
          nation_sum[y] += cell.volume;
          nation_rows[y] += cell.rows;
        }
      }
    }
  }

  namespace pgn = ::accelerator_codegen::pg_numeric;
  for (int y = 0; y < 2; ++y) {
    if (total_rows[y] <= 0)
      continue; // no row in this year's group
    // SUM(CASE ... ELSE 0 END) has scale 4 iff some row took the THEN branch
    // (every other input is the integer literal 0, scale 0).
    const auto share = pgn::DivRound(nation_sum[y], nation_rows[y] > 0 ? kVolumeScale : 0,
                                     total[y], kVolumeScale);
    sink.WriteRow(
        {{core::AuxValue::Of(core::AuxType::Int64,
                             static_cast<int64_t>(kTpchQ8MinYear + y)),
          core::AuxValue::Of(core::AuxType::String, pgn::ToText(share))}});
  }
  sink.End();
}

std::vector<core::CalibrationWrite> TpchQ8Module::CalibrationWrites() const {
  return TpchQ8CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
