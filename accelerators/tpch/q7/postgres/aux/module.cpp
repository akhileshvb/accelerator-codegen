#include "tpch/q7/postgres/aux/module.hpp"
#include "tpch/q7/calibration_writes.hpp"
#include "substrate/aux/core/aux_cells.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/partitioned_build.hpp"
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
using Cell = core::CellState;

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

// The supplier / customer nation of a record when the whole join resolves.
bool Joins(const TpchQ7State &s, int64_t suppkey, int64_t custkey,
           int32_t &supp_nk, int32_t &cust_nk) {
  supp_nk = s.supplier_nation.Get(suppkey);
  cust_nk = s.customer_nation.Get(custkey);
  return supp_nk != TpchQ7State::kAbsent && cust_nk != TpchQ7State::kAbsent;
}

// Build and maintenance fold whole batches of rows at once. The rows are dealt
// to owners by order-map chunk; each owner folds its orders against the
// (read-only) state, in parallel slices, into the new entries and the net
// effects on the counters and the cube. The state is then written serially.
struct LineRow {
  int64_t orderkey = 0;
  LineRec rec;
  int sign = +1; // +1 added, -1 removed
};

struct OrderRow {
  int64_t orderkey = 0;
  int64_t custkey = 0;
  int sign = +1;
};

struct FoldedOrders {
  bool ok = true; // false: a row could not be applied exactly
  std::vector<int64_t> orderkeys; // ascending
  std::vector<std::size_t> ends;  // end of each entry in `bytes` (empty: erased)
  std::vector<uint8_t> bytes;
  std::unordered_map<int64_t, int64_t> supplier_refs; // net in-window lineitems per supplier
  std::unordered_map<int64_t, int64_t> customer_refs; // net orders per customer
  Cube cube;
};

FoldedOrders Unfoldable() {
  FoldedOrders folded;
  folded.ok = false;
  return folded;
}

void AppendEntry(FoldedOrders &folded, int64_t orderkey, const OrderEntry &e) {
  thread_local std::vector<uint8_t> encoded;
  if (e.present || !e.lines.empty()) {
    EncodeEntry(e, encoded);
    folded.bytes.insert(folded.bytes.end(), encoded.begin(), encoded.end());
  }
  folded.orderkeys.push_back(orderkey);
  folded.ends.push_back(folded.bytes.size());
}

FoldedOrders FoldLines(const TpchQ7State &s, std::vector<LineRow> &lines) {
  std::sort(lines.begin(), lines.end(), [](const LineRow &l, const LineRow &r) {
    return std::tie(l.orderkey, l.rec.suppkey, l.rec.year, l.rec.volume) <
           std::tie(r.orderkey, r.rec.suppkey, r.rec.year, r.rec.volume);
  });
  FoldedOrders folded;
  for (std::size_t i = 0; i < lines.size();) {
    const int64_t orderkey = lines[i].orderkey;
    OrderEntry e;
    if (!LoadEntry(s, orderkey, e))
      return Unfoldable();
    for (; i < lines.size() && lines[i].orderkey == orderkey; ++i) {
      const LineRec &rec = lines[i].rec;
      const int sign = lines[i].sign;
      if (sign > 0) {
        e.lines.push_back(rec);
      } else {
        const auto it = std::find(e.lines.begin(), e.lines.end(), rec);
        if (it == e.lines.end())
          return Unfoldable();
        e.lines.erase(it);
      }
      folded.supplier_refs[rec.suppkey] += sign;
      int32_t snk = 0, cnk = 0;
      if (e.present && Joins(s, rec.suppkey, e.custkey, snk, cnk))
        AddCell(folded.cube, {snk, cnk, rec.year}, static_cast<Int128>(sign) * rec.volume, sign);
    }
    AppendEntry(folded, orderkey, e);
  }
  return folded;
}

FoldedOrders FoldOrderRows(const TpchQ7State &s, std::vector<OrderRow> &orders) {
  std::sort(orders.begin(), orders.end(), [](const OrderRow &l, const OrderRow &r) {
    return std::tie(l.orderkey, l.custkey) < std::tie(r.orderkey, r.custkey);
  });
  FoldedOrders folded;
  for (std::size_t i = 0; i < orders.size();) {
    const int64_t orderkey = orders[i].orderkey;
    OrderEntry e;
    if (!LoadEntry(s, orderkey, e))
      return Unfoldable();
    for (; i < orders.size() && orders[i].orderkey == orderkey; ++i) {
      const OrderRow &row = orders[i];
      if (row.sign > 0) {
        if (e.present)
          return Unfoldable(); // duplicate order key: not a TPC-H orders table
        e.present = true;
        e.custkey = row.custkey;
      } else {
        if (!e.present || e.custkey != row.custkey)
          return Unfoldable();
        e.present = false;
      }
      folded.customer_refs[row.custkey] += row.sign;
      for (const auto &l : e.lines) {
        int32_t snk = 0, cnk = 0;
        if (Joins(s, l.suppkey, row.custkey, snk, cnk))
          AddCell(folded.cube, {snk, cnk, l.year}, static_cast<Int128>(row.sign) * l.volume,
                  row.sign);
      }
    }
    AppendEntry(folded, orderkey, e);
  }
  return folded;
}

void ApplyRefs(core::AnyKeyCowArray<uint32_t> &refs,
               const std::unordered_map<int64_t, int64_t> &net) {
  for (const auto &[key, change] : net)
    refs.Set(key, static_cast<uint32_t>(std::max<int64_t>(0, int64_t{refs.Get(key)} + change)));
}

void InstallFolded(TpchQ7State &s, FoldedOrders &&folded) {
  std::size_t begin = 0;
  for (std::size_t i = 0; i < folded.orderkeys.size(); begin = folded.ends[i++]) {
    if (folded.ends[i] == begin)
      s.orders.Erase(folded.orderkeys[i]);
    else
      s.orders.Set(folded.orderkeys[i], folded.bytes.data() + begin, folded.ends[i] - begin);
  }
  ApplyRefs(s.supplier_refs, folded.supplier_refs);
  ApplyRefs(s.customer_refs, folded.customer_refs);
  MergeCube(s.cube, std::move(folded.cube));
}

using core::kMaxOwners;
using core::OwnerCount;

std::size_t OwnerOf(int64_t orderkey, std::size_t owners) {
  return static_cast<uint64_t>(orderkey >> core::ChunkedBlobMap::kKeyShift) % owners;
}

// Folds each owner's rows in parallel slices, then installs the folds in owner
// order. False (the state untouched) when a row cannot be applied exactly.
template <class Row, class Fold>
bool FoldAndInstall(const core::AuxEngineServices &services, TpchQ7State &s,
                    core::Buckets<Row> &owners, Fold &&fold) {
  std::vector<FoldedOrders> folded(owners.size());
  const TpchQ7State &before = s;
  core::ParallelEach(services, owners.size(), 1, [&](std::size_t owner) {
    folded[owner] = fold(before, owners[owner]);
    owners[owner] = {};
  });
  for (const FoldedOrders &part : folded)
    if (!part.ok)
      return false;
  for (FoldedOrders &part : folded)
    InstallFolded(s, std::move(part));
  return true;
}

// Old / new nation key of a supplier or customer changed by one write batch
// (kAbsent: the row does not exist before / after).
struct Change {
  int32_t before = TpchQ7State::kAbsent;
  int32_t after = TpchQ7State::kAbsent;
};

using Changes = std::map<int64_t, Change>;

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
  struct Visit {
    Cube cube;
    std::size_t orders = 0;
  } dense;
  core::ParallelReduce(
      services, orders.NumChunkSlots(), kChunkGrain, dense,
      [&](Visit &into, std::size_t begin, std::size_t end) {
        for (std::size_t ci = begin; ci < end; ++ci)
          orders.ForEachInChunk(ci, [&](int64_t, std::string_view v) {
            ++into.orders;
            visit(into.cube, v);
          });
      },
      [](Visit &into, Visit &&from) {
        MergeCube(into.cube, std::move(from.cube));
        into.orders += from.orders;
      });
  if (dense.orders < orders.size()) { // some orders sit outside the chunked keys
    dense.cube.clear();
    orders.ForEach([&](int64_t, std::string_view v) { visit(dense.cube, v); });
  }
  MergeCube(s.cube, std::move(dense.cube));
}

// Supplier rows changed: move the cube contribution of their lineitems from
// the old nation to the new one. Only suppliers that have in-window lineitems
// need the pass over the orders (a brand-new supplier has none).
void RepointSuppliers(const core::AuxEngineServices &services, TpchQ7State &s,
                      const Changes &changes) {
  std::unordered_map<int64_t, Change> hot;
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
                      const Changes &changes) {
  std::unordered_map<int64_t, Change> hot;
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
bool CollectKeyChanges(const core::MutationBatch &m, Changes &out) {
  const auto read = [](const core::AuxRow &row, int64_t &key, int32_t &nk) {
    int64_t nationkey = 0;
    const Cell k = core::NullableIntCell(row, 0, key);
    const Cell n = core::NullableIntCell(row, 1, nationkey);
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

// Loads a dimension's (key -> nation key) column from its partitioned scan.
template <class Input, class Scan>
void LoadNationColumn(const core::AuxEngineServices &services, Scan &&scan,
                      core::AnyKeyCowArray<int32_t> &column) {
  using Rows = std::vector<std::pair<int64_t, int32_t>>;
  const Rows rows = core::PartitionedBuild<Rows>(
      services, [] { return Rows{}; },
      [&](std::size_t partitions, auto &&sink) {
        scan(partitions, [&](std::size_t partition, const Input &input) {
          sink(partition, core::SingleRowBatch<Input>{input});
        });
      },
      [](Rows &into, const Input &input) { into.emplace_back(input.key, input.nationkey); },
      [](Rows &into, Rows &&from) { into.insert(into.end(), from.begin(), from.end()); },
      core::BuildPartitions(services));
  for (const auto &[key, nationkey] : rows)
    column.Set(key, nationkey);
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

  initial_scan->ScanNationsSerial([&](const TpchQ7NationInput &input) {
    s.nations[input.key] = input.name;
  });
  LoadNationColumn<TpchQ7SupplierInput>(
      services,
      [&](std::size_t partitions, const auto &sink) {
        initial_scan->ScanSuppliers(services, partitions, sink);
      },
      s.supplier_nation);
  LoadNationColumn<TpchQ7CustomerInput>(
      services,
      [&](std::size_t partitions, const auto &sink) {
        initial_scan->ScanCustomers(services, partitions, sink);
      },
      s.customer_nation);

  // Orders first: the lineitems fold against them.
  auto orders = core::PartitionedBuild<core::Buckets<OrderRow>>(
      services, [] { return core::Buckets<OrderRow>(kMaxOwners); },
      [&](std::size_t partitions, auto &&sink) {
        initial_scan->ScanOrders(
            services, partitions, [&](std::size_t partition, const TpchQ7OrderInput &input) {
              sink(partition, core::SingleRowBatch<TpchQ7OrderInput>{input});
            });
      },
      [](core::Buckets<OrderRow> &into, const TpchQ7OrderInput &input) {
        into[OwnerOf(input.key, kMaxOwners)].push_back({input.key, input.custkey});
      },
      core::AppendBuckets<OrderRow>, core::BuildPartitions(services));
  if (!FoldAndInstall(services, s, orders, FoldOrderRows))
    throw std::runtime_error("TPC-H Q7: unsupported orders row");

  const int64_t first_day = sql_date::DaysFromCivil(kTpchQ7MinYear, 1, 1);
  const int64_t last_day = sql_date::DaysFromCivil(kTpchQ7MaxYear, 12, 31);
  auto lines = core::PartitionedBuild<core::Buckets<LineRow>>(
      services, [] { return core::Buckets<LineRow>(kMaxOwners); },
      [&](std::size_t partitions, auto &&sink) {
        initial_scan->ScanLineitems(
            services, partitions, [&](std::size_t partition, const TpchQ7LineitemInput &input) {
              sink(partition, core::SingleRowBatch<TpchQ7LineitemInput>{input});
            });
      },
      [&](core::Buckets<LineRow> &into, const TpchQ7LineitemInput &input) {
        if (input.shipdate_days < first_day || input.shipdate_days > last_day)
          return;
        int64_t volume = 0;
        if (!VolumeRaw(input.extended_price_raw, input.extended_price_scale,
                       input.discount_raw, input.discount_scale, volume))
          throw std::runtime_error("TPC-H Q7: unsupported price / discount");
        into[OwnerOf(input.orderkey, kMaxOwners)].push_back(
            {input.orderkey, {input.suppkey, sql_date::YearOf(input.shipdate_days), volume}});
      },
      core::AppendBuckets<LineRow>, core::BuildPartitions(services));
  if (!FoldAndInstall(services, s, lines, FoldLines))
    throw std::runtime_error("TPC-H Q7: unsupported lineitem row");
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
      const Cell c = core::NullableIntCell(row, 0, key);
      if (c == Cell::Bad) {
        ok = false;
        break;
      }
      if (c == Cell::Ok)
        s.nations.erase(static_cast<int32_t>(key));
    }
    for (const auto &row : mutation.new_rows) {
      int64_t key = 0;
      const Cell c = core::NullableIntCell(row, 0, key);
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
    Changes changes;
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
      core::Buckets<OrderRow> owners(OwnerCount(rows.size()));
      for (const auto &row : rows) {
        int64_t orderkey = 0, custkey = 0;
        const Cell a = core::NullableIntCell(row, 0, orderkey);
        const Cell b = core::NullableIntCell(row, 1, custkey);
        if (a == Cell::Bad || b == Cell::Bad)
          return false;
        if (a == Cell::Null || b == Cell::Null)
          continue; // never joins
        owners[OwnerOf(orderkey, owners.size())].push_back({orderkey, custkey, sign});
      }
      return FoldAndInstall(services, s, owners, FoldOrderRows);
    };
    ok = apply(mutation.old_rows, -1) && apply(mutation.new_rows, +1);
  } else if (mutation.table == config.lineitem_table) {
    const int64_t first_day = sql_date::DaysFromCivil(kTpchQ7MinYear, 1, 1);
    const int64_t last_day = sql_date::DaysFromCivil(kTpchQ7MaxYear, 12, 31);
    const auto apply = [&](const std::vector<core::AuxRow> &rows, int sign) {
      core::Buckets<LineRow> owners(OwnerCount(rows.size()));
      for (const auto &row : rows) {
        int64_t orderkey = 0, suppkey = 0, price = 0, discount = 0;
        int32_t price_scale = 0, discount_scale = 0, shipdate = 0;
        const Cell cells[5] = {core::NullableIntCell(row, 0, orderkey),
                               core::NullableIntCell(row, 1, suppkey),
                               core::NullableDecimalCell(row, 2, price, price_scale),
                               core::NullableDecimalCell(row, 3, discount, discount_scale),
                               core::NullableDateCell(row, 4, shipdate)};
        bool null_cell = false;
        for (const Cell c : cells) {
          if (c == Cell::Bad)
            return false;
          null_cell = null_cell || c == Cell::Null;
        }
        if (null_cell || shipdate < first_day || shipdate > last_day)
          continue; // not in the result
        int64_t volume = 0;
        if (!VolumeRaw(price, price_scale, discount, discount_scale, volume))
          return false;
        owners[OwnerOf(orderkey, owners.size())].push_back(
            {orderkey, {suppkey, sql_date::YearOf(shipdate), volume}, sign});
      }
      return FoldAndInstall(services, s, owners, FoldLines);
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
  if (buffer.RequiresRebuild()) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }
  if (!buffer.HasPendingChanges() || !buffer.work)
    return outcome;

  const auto decision = services.ChooseMaintenance(
      core::MaintenanceRequest::IncrementalSingleState(
          std::max<uint64_t>(1, buffer.rows), services.ExpectedQueriesBeforeNextWrite()));
  if (decision.action != core::MaintenanceAction::ApplyDelta) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }

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
