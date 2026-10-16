#include "tpch/q7/postgres/aux/module.hpp"
#include "tpch/q7/calibration_writes.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/query_module_helpers.hpp"
#include "prims/sql_date.hpp"
#include "tpch/q7/layout.hpp"

namespace accelerator_codegen::aux_state::query {

namespace {

namespace sql_date = core::sql_date;
using Int128 = ::accelerator_codegen::pg_numeric::Int128;

constexpr core::PartitionId kStatePartition = core::TpchQ7Layout::kSidePartition;
// Revenue = price * (1 - discount): both numeric(15,2) -> scale 4.
constexpr int32_t kRevenueScale = 4;
// Order-map chunks (<= 1024 orders) per slice: ~0.3 us per decoded order.
constexpr std::size_t kChunkGrain = 2;

// ---- Row decoding (mutation capture rows) ---------------------------------
// A cell is Ok, SQL NULL (the row cannot join / pass the filter), or of an
// unexpected type (the capture layout is not what this module expects: the
// state cannot follow the write, so it is rebuilt).
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

// volume = price * (1 - discount) as a scale-4 integer; false on overflow or
// when the operand scales do not give scale 4.
bool VolumeRaw(int64_t price_raw, int32_t price_scale, int64_t discount_raw,
               int32_t discount_scale, int64_t &out) {
  if (price_scale + discount_scale != kRevenueScale || discount_scale < 0 ||
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

// ---- Order entry: the one blob kept per orderkey ---------------------------
struct LineRec {
  int64_t suppkey = 0;
  int32_t year = 0; // kTpchQ7MinYear / kTpchQ7MaxYear
  int64_t volume = 0;
  bool operator==(const LineRec &o) const {
    return suppkey == o.suppkey && year == o.year && volume == o.volume;
  }
};

struct OrderEntry {
  bool present = false; // an orders row exists
  int64_t custkey = 0;
  std::vector<LineRec> lines; // in-window lineitems of the order
};

constexpr uint8_t kFlagOrderPresent = 1;

void EncodeEntry(const OrderEntry &e, std::vector<uint8_t> &out) {
  out.clear();
  out.push_back(e.present ? kFlagOrderPresent : 0);
  if (e.present)
    core::PutSignedVarint(out, e.custkey);
  for (const auto &l : e.lines) {
    core::PutSignedVarint(out, l.suppkey);
    out.push_back(l.year == kTpchQ7MaxYear ? 1 : 0);
    core::PutSignedVarint(out, l.volume);
  }
}

bool DecodeEntry(std::string_view v, OrderEntry &e) {
  e = OrderEntry{};
  const auto *p = reinterpret_cast<const uint8_t *>(v.data());
  const auto *end = p + v.size();
  if (p >= end)
    return false;
  e.present = (*p++ & kFlagOrderPresent) != 0;
  uint64_t u = 0;
  if (e.present) {
    if (!core::GetSignedVarint(p, end, e.custkey))
      return false;
  }
  while (p < end) {
    LineRec l;
    int64_t volume = 0;
    if (!core::GetSignedVarint(p, end, l.suppkey) || p >= end)
      return false;
    l.year = *p++ ? kTpchQ7MaxYear : kTpchQ7MinYear;
    if (!core::GetSignedVarint(p, end, volume))
      return false;
    l.volume = volume;
    e.lines.push_back(l);
  }
  return true;
}

bool LoadEntry(const TpchQ7State &s, int64_t orderkey, OrderEntry &e) {
  std::string_view v;
  if (!s.orders.Get(orderkey, v)) {
    e = OrderEntry{};
    return true;
  }
  return DecodeEntry(v, e);
}

bool StoreEntry(TpchQ7State &s, int64_t orderkey, const OrderEntry &e) {
  if (!e.present && e.lines.empty()) {
    s.orders.Erase(orderkey);
    return true;
  }
  thread_local std::vector<uint8_t> buf;
  EncodeEntry(e, buf);
  return s.orders.Set(orderkey, buf);
}

// ---- Cube ------------------------------------------------------------------
using Cube = std::map<TpchQ7CubeKey, TpchQ7CubeCell>;

void AddCell(Cube &cube, const TpchQ7CubeKey &key, Int128 revenue, int64_t rows) {
  auto &cell = cube[key];
  cell.revenue += revenue;
  cell.rows += rows;
  if (cell.rows == 0 && cell.revenue == 0)
    cube.erase(key);
}

void MergeCube(Cube &into, Cube &&from) {
  for (const auto &[key, cell] : from)
    AddCell(into, key, cell.revenue, cell.rows);
}

void Contribute(TpchQ7State &s, int32_t supp_nk, int32_t cust_nk, int32_t year,
                int64_t volume, int sign) {
  AddCell(s.cube, {supp_nk, cust_nk, year}, static_cast<Int128>(sign) * volume, sign);
}

// The supplier / customer nation of a record when the whole join resolves.
bool Joins(const TpchQ7State &s, int64_t suppkey, int64_t custkey,
           int32_t &supp_nk, int32_t &cust_nk) {
  supp_nk = s.supplier_nation.Get(suppkey);
  cust_nk = s.customer_nation.Get(custkey);
  return supp_nk != TpchQ7State::kAbsent && cust_nk != TpchQ7State::kAbsent;
}

// ---- Incremental maintenance primitives ------------------------------------
// Each returns false when the write cannot be applied exactly to this state
// (inconsistent row, unsupported key); the caller then rebuilds.

bool ApplyLineitem(TpchQ7State &s, int64_t orderkey, int64_t suppkey,
                   int32_t year, int64_t volume, int sign) {
  OrderEntry e;
  if (!LoadEntry(s, orderkey, e))
    return false;
  const LineRec rec{suppkey, year, volume};
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
  const uint32_t refs = s.supplier_refs.Get(suppkey);
  if (sign > 0)
    s.supplier_refs.Set(suppkey, refs + 1);
  else if (refs > 0)
    s.supplier_refs.Set(suppkey, refs - 1);
  int32_t snk = 0, cnk = 0;
  if (e.present && Joins(s, suppkey, e.custkey, snk, cnk))
    Contribute(s, snk, cnk, year, volume, sign);
  return true;
}

bool ApplyOrder(TpchQ7State &s, int64_t orderkey, int64_t custkey, int sign) {
  OrderEntry e;
  if (!LoadEntry(s, orderkey, e))
    return false;
  if (sign > 0) {
    if (e.present)
      return false; // duplicate order key: not a TPC-H orders table
    e.present = true;
    e.custkey = custkey;
  } else {
    if (!e.present || e.custkey != custkey)
      return false;
    e.present = false;
  }
  if (!StoreEntry(s, orderkey, e))
    return false;
  const uint32_t refs = s.customer_refs.Get(custkey);
  if (sign > 0)
    s.customer_refs.Set(custkey, refs + 1);
  else if (refs > 0)
    s.customer_refs.Set(custkey, refs - 1);
  for (const auto &l : e.lines) {
    int32_t snk = 0, cnk = 0;
    if (Joins(s, l.suppkey, custkey, snk, cnk))
      Contribute(s, snk, cnk, l.year, l.volume, sign);
  }
  return true;
}

// ---- Bulk build of the lineitem entries ------------------------------------
// The build folds whole orders at once instead of applying lineitem by lineitem:
// the in-window lineitems are dealt to owners by order, and each owner builds
// the entries of its orders against the (read-only) orders applied so far. The
// stores and counters are applied afterwards, serially.
struct BuildLine {
  int64_t orderkey = 0;
  LineRec rec;
};

struct FoldedOrders {
  std::vector<int64_t> orderkeys; // ascending
  std::vector<std::size_t> ends;  // end of each entry in `bytes`
  std::vector<uint8_t> bytes;
  std::unordered_map<int64_t, uint32_t> supplier_refs; // lineitems per supplier
  Cube cube;
};

FoldedOrders FoldOrders(const TpchQ7State &s, std::vector<BuildLine> &lines) {
  std::sort(lines.begin(), lines.end(), [](const BuildLine &l, const BuildLine &r) {
    return std::tie(l.orderkey, l.rec.suppkey, l.rec.year, l.rec.volume) <
           std::tie(r.orderkey, r.rec.suppkey, r.rec.year, r.rec.volume);
  });
  FoldedOrders folded;
  std::vector<uint8_t> entry_bytes;
  for (std::size_t i = 0; i < lines.size();) {
    OrderEntry e;
    if (!LoadEntry(s, lines[i].orderkey, e))
      throw std::runtime_error("TPC-H Q7: unsupported lineitem row");
    folded.orderkeys.push_back(lines[i].orderkey);
    for (; i < lines.size() && lines[i].orderkey == folded.orderkeys.back(); ++i) {
      const LineRec &rec = lines[i].rec;
      e.lines.push_back(rec);
      ++folded.supplier_refs[rec.suppkey];
      int32_t snk = 0, cnk = 0;
      if (e.present && Joins(s, rec.suppkey, e.custkey, snk, cnk))
        AddCell(folded.cube, {snk, cnk, rec.year}, rec.volume, +1);
    }
    EncodeEntry(e, entry_bytes);
    folded.bytes.insert(folded.bytes.end(), entry_bytes.begin(), entry_bytes.end());
    folded.ends.push_back(folded.bytes.size());
  }
  return folded;
}

void InstallFolded(TpchQ7State &s, FoldedOrders &&folded) {
  std::size_t begin = 0;
  for (std::size_t i = 0; i < folded.orderkeys.size(); begin = folded.ends[i++])
    s.orders.Set(folded.orderkeys[i], folded.bytes.data() + begin, folded.ends[i] - begin);
  for (const auto &[suppkey, count] : folded.supplier_refs)
    s.supplier_refs.Set(suppkey, s.supplier_refs.Get(suppkey) + count);
  MergeCube(s.cube, std::move(folded.cube));
}

// Old / new nation key of a supplier or customer changed by one write batch
// (kAbsent: the row does not exist before / after).
struct KeyChange {
  int32_t before = TpchQ7State::kAbsent;
  int32_t after = TpchQ7State::kAbsent;
};
using KeyChanges = std::map<int64_t, KeyChange>;

// Calls on_order(cube, entry) for every order with a present row: the dense
// part of the order map in slices (each into its own cube), orders at keys
// outside it serially. The slices' cubes are folded into `s.cube`.
template <typename OnOrder>
void RepointOrders(const core::AuxEngineServices &services, TpchQ7State &s, OnOrder &&on_order) {
  const core::ChunkedBlobMap &orders = s.orders;
  const auto visit = [&](Cube &cube, std::string_view v) {
    OrderEntry e;
    if (DecodeEntry(v, e) && e.present)
      on_order(cube, e);
  };
  Cube cube;
  std::atomic<std::size_t> dense_orders{0};
  core::ParallelReduce(
      services, orders.NumChunkSlots(), kChunkGrain, cube,
      [&](Cube &into, std::size_t begin, std::size_t end) {
        for (std::size_t ci = begin; ci < end; ++ci)
          orders.ForEachInChunk(ci, [&](int64_t, std::string_view v) {
            ++dense_orders;
            visit(into, v);
          });
      },
      MergeCube);
  if (dense_orders < orders.size()) { // some orders sit outside the chunked keys
    cube.clear();
    orders.ForEach([&](int64_t, std::string_view v) { visit(cube, v); });
  }
  MergeCube(s.cube, std::move(cube));
}

// Supplier rows changed: move the cube contribution of their lineitems from
// the old nation to the new one. Only suppliers that have in-window lineitems
// need the pass over the orders (a brand-new supplier has none).
void RepointSuppliers(const core::AuxEngineServices &services, TpchQ7State &s,
                      const KeyChanges &changes) {
  std::unordered_map<int64_t, KeyChange> hot;
  for (const auto &[suppkey, change] : changes)
    if (s.supplier_refs.Get(suppkey) > 0)
      hot.emplace(suppkey, change);
  if (hot.empty())
    return;
  RepointOrders(services, s, [&](Cube &cube, const OrderEntry &e) {
    const int32_t cnk = s.customer_nation.Get(e.custkey);
    if (cnk == TpchQ7State::kAbsent)
      return;
    for (const auto &l : e.lines) {
      const auto it = hot.find(l.suppkey);
      if (it == hot.end())
        continue;
      if (it->second.before != TpchQ7State::kAbsent)
        AddCell(cube, {it->second.before, cnk, l.year}, -static_cast<Int128>(l.volume), -1);
      if (it->second.after != TpchQ7State::kAbsent)
        AddCell(cube, {it->second.after, cnk, l.year}, l.volume, +1);
    }
  });
}

// Customer rows changed: same for the customers' orders.
void RepointCustomers(const core::AuxEngineServices &services, TpchQ7State &s,
                      const KeyChanges &changes) {
  std::unordered_map<int64_t, KeyChange> hot;
  for (const auto &[custkey, change] : changes)
    if (s.customer_refs.Get(custkey) > 0)
      hot.emplace(custkey, change);
  if (hot.empty())
    return;
  RepointOrders(services, s, [&](Cube &cube, const OrderEntry &e) {
    const auto it = hot.find(e.custkey);
    if (it == hot.end())
      return;
    for (const auto &l : e.lines) {
      const int32_t snk = s.supplier_nation.Get(l.suppkey);
      if (snk == TpchQ7State::kAbsent)
        continue;
      if (it->second.before != TpchQ7State::kAbsent)
        AddCell(cube, {snk, it->second.before, l.year}, -static_cast<Int128>(l.volume), -1);
      if (it->second.after != TpchQ7State::kAbsent)
        AddCell(cube, {snk, it->second.after, l.year}, l.volume, +1);
    }
  });
}

// Collects the net before/after nation key per dimension key from a mutation
// (rows: [key, nationkey]). Returns false on an unexpected row layout.
bool CollectKeyChanges(const core::MutationBatch &m, KeyChanges &out) {
  const auto read = [](const core::AuxRow &row, int64_t &key, int32_t &nk) {
    int64_t nationkey = 0;
    const Cell k = IntCell(row, 0, key);
    const Cell n = IntCell(row, 1, nationkey);
    if (k == Cell::Bad || n == Cell::Bad)
      return Cell::Bad;
    if (k == Cell::Null)
      return Cell::Null; // a NULL key never joins
    // A NULL nation key never joins n_nationkey: the row exists but is inert.
    nk = n == Cell::Null || nationkey < std::numeric_limits<int32_t>::min() + 1 ||
                 nationkey > std::numeric_limits<int32_t>::max()
             ? TpchQ7State::kAbsent
             : static_cast<int32_t>(nationkey);
    return Cell::Ok;
  };
  for (const auto &row : m.old_rows) {
    int64_t key = 0;
    int32_t nk = 0;
    const Cell c = read(row, key, nk);
    if (c == Cell::Bad)
      return false;
    if (c == Cell::Ok)
      out[key].before = nk;
  }
  for (const auto &row : m.new_rows) {
    int64_t key = 0;
    int32_t nk = 0;
    const Cell c = read(row, key, nk);
    if (c == Cell::Bad)
      return false;
    if (c == Cell::Ok)
      out[key].after = nk;
  }
  return true;
}

} // namespace

core::AcceleratorId TpchQ7Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ7Module::Dependencies() const {
  // Projected columns (physical attribute numbers):
  //   nation   : n_nationkey, n_name
  //   supplier : s_suppkey, s_nationkey
  //   customer : c_custkey, c_nationkey
  //   orders   : o_orderkey, o_custkey
  //   lineitem : l_orderkey, l_suppkey, l_extendedprice, l_discount, l_shipdate
  return {{config.nation_table, {0, 1}, {0, 1}},
          {config.supplier_table, {0, 1}, {0, 3}},
          {config.customer_table, {0, 1}, {0, 3}},
          {config.orders_table, {0, 1}, {0, 1}},
          {config.lineitem_table, {0, 1, 2, 3, 4}, {0, 2, 5, 6, 10}}};
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ7Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ7TxnBuffer>();
}

std::shared_ptr<TpchQ7State> TpchQ7Module::BuildState(const core::AuxEngineServices &services) const {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q7 requires a typed initial scan source");
  auto state = std::make_shared<TpchQ7State>();
  TpchQ7State &s = *state;

  initial_scan->ScanNations([&](const TpchQ7NationInput &input) {
    s.nations[input.key] = input.name;
  });
  initial_scan->ScanSuppliers([&](const TpchQ7SupplierInput &input) {
    s.supplier_nation.Set(input.key, input.nationkey);
  });
  initial_scan->ScanCustomers([&](const TpchQ7CustomerInput &input) {
    s.customer_nation.Set(input.key, input.nationkey);
  });
  initial_scan->ScanOrders([&](const TpchQ7OrderInput &input) {
    if (!ApplyOrder(s, input.key, input.custkey, +1))
      throw std::runtime_error("TPC-H Q7: unsupported orders row");
  });

  const int64_t first_day = sql_date::DaysFromCivil(kTpchQ7MinYear, 1, 1);
  const int64_t last_day = sql_date::DaysFromCivil(kTpchQ7MaxYear, 12, 31);
  std::vector<std::vector<BuildLine>> by_owner(std::max<std::size_t>(1, services.WorkerCount()));
  initial_scan->ScanLineitems([&](const TpchQ7LineitemInput &input) {
    if (input.shipdate_days < first_day || input.shipdate_days > last_day)
      return;
    int64_t volume = 0;
    if (!VolumeRaw(input.extended_price_raw, input.extended_price_scale,
                   input.discount_raw, input.discount_scale, volume))
      throw std::runtime_error("TPC-H Q7: unsupported price / discount");
    const std::size_t owner = static_cast<uint64_t>(input.orderkey >> core::ChunkedBlobMap::kKeyShift) % by_owner.size();
    by_owner[owner].push_back(
        {input.orderkey, {input.suppkey, sql_date::YearOf(input.shipdate_days), volume}});
  });
  std::vector<FoldedOrders> folded(by_owner.size());
  services.ParallelFor(by_owner.size(), [&](std::size_t owner) {
    folded[owner] = FoldOrders(s, by_owner[owner]);
  });
  for (FoldedOrders &part : folded)
    InstallFolded(s, std::move(part));
  s.orders.Shrink();
  return state;
}

void TpchQ7Module::Build(core::AuxEngineServices &services, core::StateBuilder &builder) {
  auto image = std::make_shared<TpchQ7StateImage>();
  image->state = BuildState(services);
  builder.Write(core::StateImage::Make(
      core::TpchQ7Layout::kAccelerator, kStatePartition, 0,
      std::shared_ptr<const TpchQ7StateImage>(std::move(image))));
  builder.Finish();
}

core::InvalidationPlan
TpchQ7Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.lineitem_table ||
      mutation.table == config.orders_table ||
      mutation.table == config.supplier_table ||
      mutation.table == config.customer_table ||
      mutation.table == config.nation_table) {
    core::InvalidationPlan plan;
    plan.scope =
        ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
    plan.partitions.insert(kStatePartition);
    return plan;
  }
  return {};
}

void TpchQ7Module::AccumulateMutation(const core::MutationBatch &mutation,
                                      core::AuxEngineServices &services,
                                      core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ7TxnBuffer &>(base_buffer);
  if (buffer.requires_rebuild)
    return;
  if (!buffer.work) {
    // Fork the published state (pointer copies) for this transaction.
    const auto snapshot = services.StateStore().ReadPartitions(
        services.CurrentVisibility(), {kStatePartition});
    for (const auto &image : snapshot.partitions) {
      const auto *typed = image.partition == kStatePartition
                              ? image.As<TpchQ7StateImage>()
                              : nullptr;
      if (typed && typed->state) {
        buffer.work = std::make_shared<TpchQ7State>(*typed->state);
        buffer.base_version = image.version;
        break;
      }
    }
    if (!buffer.work) {
      buffer.MarkRebuild();
      return;
    }
  }
  TpchQ7State &s = *buffer.work;
  buffer.pending = true;
  buffer.rows += core::MutationRowCount(mutation);

  bool ok = true;
  if (mutation.table == config.nation_table) {
    // Only the key -> name table changes; the cube is keyed by nation keys.
    for (const auto &row : mutation.old_rows) {
      int64_t key = 0;
      const Cell c = IntCell(row, 0, key);
      if (c == Cell::Bad) {
        ok = false;
        break;
      }
      if (c == Cell::Ok)
        s.nations.erase(static_cast<int32_t>(key));
    }
    for (const auto &row : mutation.new_rows) {
      int64_t key = 0;
      const Cell c = IntCell(row, 0, key);
      if (c == Cell::Bad || row.values.size() < 2 ||
          (!row.values[1].is_null &&
           !std::holds_alternative<std::string>(row.values[1].payload))) {
        ok = false;
        break;
      }
      // A NULL n_name never equals a parameter: the nation is inert.
      if (c == Cell::Ok) {
        if (row.values[1].is_null)
          s.nations.erase(static_cast<int32_t>(key));
        else
          s.nations[static_cast<int32_t>(key)] =
              std::get<std::string>(row.values[1].payload);
      }
    }
  } else if (mutation.table == config.supplier_table ||
             mutation.table == config.customer_table) {
    const bool supplier = mutation.table == config.supplier_table;
    KeyChanges changes;
    ok = CollectKeyChanges(mutation, changes);
    if (ok) {
      for (auto it = changes.begin(); it != changes.end();) {
        auto &arr = supplier ? s.supplier_nation : s.customer_nation;
        // The row must be what the state believes it is.
        if (it->second.before != arr.Get(it->first)) {
          ok = false;
          break;
        }
        if (it->second.before == it->second.after) {
          it = changes.erase(it);
          continue;
        }
        arr.Set(it->first, it->second.after);
        ++it;
      }
    }
    if (ok) {
      if (supplier)
        RepointSuppliers(services, s, changes);
      else
        RepointCustomers(services, s, changes);
    }
  } else if (mutation.table == config.orders_table) {
    const auto apply = [&](const std::vector<core::AuxRow> &rows, int sign) {
      for (const auto &row : rows) {
        int64_t orderkey = 0, custkey = 0;
        const Cell a = IntCell(row, 0, orderkey);
        const Cell b = IntCell(row, 1, custkey);
        if (a == Cell::Bad || b == Cell::Bad)
          return false;
        if (a == Cell::Null || b == Cell::Null)
          continue; // never joins
        if (!ApplyOrder(s, orderkey, custkey, sign))
          return false;
      }
      return true;
    };
    ok = apply(mutation.old_rows, -1) && apply(mutation.new_rows, +1);
  } else if (mutation.table == config.lineitem_table) {
    const int64_t first_day = sql_date::DaysFromCivil(kTpchQ7MinYear, 1, 1);
    const int64_t last_day = sql_date::DaysFromCivil(kTpchQ7MaxYear, 12, 31);
    const auto apply = [&](const std::vector<core::AuxRow> &rows, int sign) {
      for (const auto &row : rows) {
        int64_t orderkey = 0, suppkey = 0, price = 0, discount = 0;
        int32_t price_scale = 0, discount_scale = 0, shipdate = 0;
        const Cell cells[5] = {IntCell(row, 0, orderkey),
                               IntCell(row, 1, suppkey),
                               DecimalCell(row, 2, price, price_scale),
                               DecimalCell(row, 3, discount, discount_scale),
                               DateCell(row, 4, shipdate)};
        bool null_cell = false;
        for (const Cell c : cells) {
          if (c == Cell::Bad)
            return false;
          null_cell = null_cell || c == Cell::Null;
        }
        if (null_cell || shipdate < first_day || shipdate > last_day)
          continue; // not in the result
        int64_t volume = 0;
        if (!VolumeRaw(price, price_scale, discount, discount_scale, volume) ||
            !ApplyLineitem(s, orderkey, suppkey, sql_date::YearOf(shipdate),
                           volume, sign))
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
TpchQ7Module::StageMaintenance(core::AuxEngineServices &services,
                               const core::InvalidationPlan &,
                               core::AuxTxnBuffer &base_buffer) {
  core::StagedMaintenanceOutcome outcome;
  auto &buffer = dynamic_cast<TpchQ7TxnBuffer &>(base_buffer);
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

  auto image = std::make_shared<TpchQ7StateImage>();
  image->state = std::move(buffer.work);
  outcome.action = core::MaintenanceAction::ApplyDelta;
  outcome.updates.push_back(core::StateImage::Make(
      core::TpchQ7Layout::kAccelerator, kStatePartition, buffer.base_version,
      std::shared_ptr<const TpchQ7StateImage>(std::move(image))));
  return outcome;
}

bool TpchQ7Module::CheckValidity(core::AuxEngineServices &services,
                                 const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void TpchQ7Module::Execute(core::AuxEngineServices &services,
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
                            ? image.As<TpchQ7StateImage>()
                            : nullptr;
    if (typed && typed->state) {
      const auto hold = image.payload; // keep the state alive while emitting
      EmitResult(*typed->state, arguments, sink);
      return;
    }
  }
  ExecuteFallback(services, arguments, sink);
}

void TpchQ7Module::ExecuteFallback(core::AuxEngineServices &services,
                                   const core::QueryArguments &arguments,
                                   core::AuxRowSink &sink) {
  const auto state = BuildState(services);
  EmitResult(*state, arguments, sink);
}

void TpchQ7Module::EmitResult(const TpchQ7State &state,
                              const core::QueryArguments &arguments,
                              core::AuxRowSink &sink) const {
  sink.Begin({{{"supp_nation", core::AuxType::String, false},
               {"cust_nation", core::AuxType::String, false},
               {"l_year", core::AuxType::Int64, false},
               {"revenue", core::AuxType::String, false}}});

  // n1.n_name = nation1 AND n2.n_name = nation2 OR (the swapped pair). A NULL
  // parameter makes its comparison NULL (never true): that branch is empty;
  // when both names are equal the two branches are the same condition, so the
  // pair is counted once.
  const auto name_arg = [&](std::size_t i, std::string &out) {
    if (i >= arguments.values.size() || arguments.values[i].is_null ||
        !std::holds_alternative<std::string>(arguments.values[i].payload))
      return false;
    out = std::get<std::string>(arguments.values[i].payload);
    return true;
  };
  std::string a, b;
  const bool have_a = name_arg(0, a);
  const bool have_b = name_arg(1, b);
  std::vector<std::pair<std::string, std::string>> pairs; // (supplier, customer)
  if (have_a && have_b) {
    pairs.emplace_back(a, b);
    if (a != b)
      pairs.emplace_back(b, a);
  }

  struct Out {
    const std::string *supp;
    const std::string *cust;
    int32_t year;
    Int128 revenue;
  };
  std::vector<Out> rows;
  for (const auto &[supp_name, cust_name] : pairs) {
    Int128 revenue[2] = {0, 0};
    int64_t count[2] = {0, 0};
    for (const auto &[key, cell] : state.cube) {
      const auto supp = state.nations.find(key.supp_nationkey);
      const auto cust = state.nations.find(key.cust_nationkey);
      if (supp == state.nations.end() || cust == state.nations.end() ||
          supp->second != supp_name || cust->second != cust_name)
        continue;
      const int y = key.year - kTpchQ7MinYear;
      if (y < 0 || y > 1)
        continue;
      revenue[y] += cell.revenue;
      count[y] += cell.rows;
    }
    for (int y = 0; y < 2; ++y)
      if (count[y] > 0)
        rows.push_back({&supp_name, &cust_name, kTpchQ7MinYear + y, revenue[y]});
  }
  // ORDER BY supp_nation, cust_nation, l_year (text columns are C-collated).
  std::sort(rows.begin(), rows.end(), [](const Out &l, const Out &r) {
    if (*l.supp != *r.supp)
      return *l.supp < *r.supp;
    if (*l.cust != *r.cust)
      return *l.cust < *r.cust;
    return l.year < r.year;
  });
  for (const auto &row : rows) {
    core::AuxRow out;
    out.values.push_back(core::AuxValue::Of(core::AuxType::String, *row.supp));
    out.values.push_back(core::AuxValue::Of(core::AuxType::String, *row.cust));
    out.values.push_back(
        core::AuxValue::Of(core::AuxType::Int64, static_cast<int64_t>(row.year)));
    out.values.push_back(core::AuxValue::Of(
        core::AuxType::String,
        ::accelerator_codegen::pg_numeric::ToText(row.revenue, kRevenueScale)));
    sink.WriteRow(out);
  }
  sink.End();
}

std::vector<core::CalibrationWrite> TpchQ7Module::CalibrationWrites() const {
  return TpchQ7CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
