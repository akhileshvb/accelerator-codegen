#include "tpch/q10/postgres/aux/module.hpp"
#include "tpch/q10/calibration_writes.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

#include "accelerator_codegen/stateful/core/pg_numeric.hpp"
#include "accelerator_codegen/stateful/core/query_module_helpers.hpp"
#include "accelerator_codegen/stateful/core/sql_date.hpp"
#include "tpch/q10/layout.hpp"

namespace accelerator_codegen::aux_state::query {

namespace {

namespace sql_date = core::sql_date;
using Int128 = ::accelerator_codegen::pg_numeric::Int128;

constexpr core::PartitionId kStatePartition =
    core::TpchQ10Layout::kCustomerDimensions;
constexpr uint32_t kMaxBuckets = 1u << 16;

// ---- Row decoding (mutation capture rows) ---------------------------------
// A cell is Ok, SQL NULL (the row cannot join / pass the filter), or of an
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
  if (price_scale + discount_scale != kTpchQ10RevenueScale ||
      discount_scale < 0 || discount_scale > 9)
    return false;
  int64_t one = 1;
  for (int32_t i = 0; i < discount_scale; ++i)
    one *= 10;
  int64_t factor = 0;
  if (__builtin_sub_overflow(one, discount_raw, &factor))
    return false;
  return !__builtin_mul_overflow(price_raw, factor, &out);
}

// ---- Order entry: one blob per orderkey ------------------------------------
struct OrderEntry {
  bool present = false; // an orders row (with a custkey and a date) exists
  int64_t custkey = 0;
  int32_t day = 0;
  int64_t returned_rows = 0;    // returned lineitems of the order
  int64_t returned_volume = 0;  // their revenue at scale 4
};

constexpr uint8_t kFlagOrderPresent = 1;

void EncodeEntry(const OrderEntry &e, std::vector<uint8_t> &out) {
  out.clear();
  out.push_back(e.present ? kFlagOrderPresent : 0);
  if (e.present) {
    core::PutSignedVarint(out, e.custkey);
    core::PutSignedVarint(out, e.day);
  }
  core::PutVarint(out, static_cast<uint64_t>(e.returned_rows));
  core::PutSignedVarint(out, e.returned_volume);
}

bool DecodeEntry(std::string_view v, OrderEntry &e) {
  e = OrderEntry{};
  const auto *p = reinterpret_cast<const uint8_t *>(v.data());
  const auto *end = p + v.size();
  if (p >= end)
    return false;
  e.present = (*p++ & kFlagOrderPresent) != 0;
  int64_t day = 0;
  uint64_t rows = 0;
  if (e.present && (!core::GetSignedVarint(p, end, e.custkey) ||
                    !core::GetSignedVarint(p, end, day)))
    return false;
  if (!core::GetVarint(p, end, rows) ||
      !core::GetSignedVarint(p, end, e.returned_volume))
    return false;
  e.day = static_cast<int32_t>(day);
  e.returned_rows = static_cast<int64_t>(rows);
  return true;
}

bool LoadEntry(const TpchQ10State &s, int64_t orderkey, OrderEntry &e) {
  std::string_view v;
  if (!s.orders.Get(orderkey, v)) {
    e = OrderEntry{};
    return true;
  }
  return DecodeEntry(v, e);
}

bool StoreEntry(TpchQ10State &s, int64_t orderkey, const OrderEntry &e) {
  if (!e.present && e.returned_rows == 0 && e.returned_volume == 0) {
    s.orders.Erase(orderkey);
    return true;
  }
  thread_local std::vector<uint8_t> buf;
  EncodeEntry(e, buf);
  return s.orders.Set(orderkey, buf);
}

// ---- Revenue lists: (day, custkey bucket) -> [(custkey, rows, revenue)] -----
struct RevRow {
  int64_t custkey = 0;
  int64_t rows = 0;
  int64_t revenue = 0;
};

int64_t RevenueKey(uint32_t buckets, int32_t day, int64_t custkey) {
  // Zigzag: dates around the epoch get small keys (a compact directory).
  const uint64_t day_index = core::ZigZagEncode(day);
  return static_cast<int64_t>(
      day_index * buckets + (static_cast<uint64_t>(custkey) & (buckets - 1)));
}

void DecodeRevRows(std::string_view v, std::vector<RevRow> &out) {
  const auto *p = reinterpret_cast<const uint8_t *>(v.data());
  const auto *end = p + v.size();
  while (p < end) {
    RevRow r;
    uint64_t rows = 0;
    if (!core::GetSignedVarint(p, end, r.custkey) ||
        !core::GetVarint(p, end, rows) ||
        !core::GetSignedVarint(p, end, r.revenue))
      return;
    r.rows = static_cast<int64_t>(rows);
    out.push_back(r);
  }
}

// Adds (rows, revenue) to customer `custkey` on `day`.
bool AdjustRevenue(TpchQ10State &s, int32_t day, int64_t custkey,
                   int64_t d_rows, int64_t d_revenue) {
  if (d_rows == 0 && d_revenue == 0)
    return true;
  const int64_t key = RevenueKey(s.buckets, day, custkey);
  std::vector<RevRow> rows;
  std::string_view v;
  if (s.revenue.Get(key, v))
    DecodeRevRows(v, rows);
  auto it = std::find_if(rows.begin(), rows.end(), [&](const RevRow &r) {
    return r.custkey == custkey;
  });
  if (it == rows.end()) {
    rows.push_back({custkey, 0, 0});
    it = rows.end() - 1;
  }
  if (__builtin_add_overflow(it->rows, d_rows, &it->rows) ||
      __builtin_add_overflow(it->revenue, d_revenue, &it->revenue) ||
      it->rows < 0)
    return false;
  if (it->rows == 0 && it->revenue == 0)
    rows.erase(it);
  if (rows.empty()) {
    s.revenue.Erase(key);
    return true;
  }
  thread_local std::vector<uint8_t> buf;
  buf.clear();
  for (const auto &r : rows) {
    core::PutSignedVarint(buf, r.custkey);
    core::PutVarint(buf, static_cast<uint64_t>(r.rows));
    core::PutSignedVarint(buf, r.revenue);
  }
  return s.revenue.Set(key, buf);
}

// ---- Customer record --------------------------------------------------------
struct CustomerRec {
  int64_t acctbal_raw = 0;
  std::string name, address, phone, comment;
};

void PutString(std::vector<uint8_t> &out, const std::string &s) {
  core::PutVarint(out, s.size());
  out.insert(out.end(), s.begin(), s.end());
}
bool GetString(const uint8_t *&p, const uint8_t *end, std::string &s) {
  uint64_t n = 0;
  if (!core::GetVarint(p, end, n) || static_cast<uint64_t>(end - p) < n)
    return false;
  s.assign(reinterpret_cast<const char *>(p), n);
  p += n;
  return true;
}

bool StoreCustomer(TpchQ10State &s, int64_t custkey, int32_t nationkey,
                   const CustomerRec &c) {
  if (nationkey == TpchQ10State::kAbsent)
    return false;
  thread_local std::vector<uint8_t> buf;
  buf.clear();
  core::PutSignedVarint(buf, c.acctbal_raw);
  PutString(buf, c.name);
  PutString(buf, c.address);
  PutString(buf, c.phone);
  PutString(buf, c.comment);
  if (!s.customers.Set(custkey, buf))
    return false;
  s.customer_nation.Set(custkey, nationkey);
  return true;
}

bool LoadCustomer(const TpchQ10State &s, int64_t custkey, CustomerRec &c) {
  std::string_view v;
  if (!s.customers.Get(custkey, v))
    return false;
  const auto *p = reinterpret_cast<const uint8_t *>(v.data());
  const auto *end = p + v.size();
  return core::GetSignedVarint(p, end, c.acctbal_raw) &&
         GetString(p, end, c.name) && GetString(p, end, c.address) &&
         GetString(p, end, c.phone) && GetString(p, end, c.comment);
}

// ---- Incremental maintenance primitives ------------------------------------
// Each returns false when the write cannot be applied exactly to this state;
// the caller then rebuilds.

bool ApplyOrder(TpchQ10State &s, int64_t orderkey, int64_t custkey, int32_t day,
                int sign) {
  OrderEntry e;
  if (!LoadEntry(s, orderkey, e))
    return false;
  if (sign > 0) {
    if (e.present)
      return false; // duplicate order key
    e.present = true;
    e.custkey = custkey;
    e.day = day;
  } else {
    if (!e.present || e.custkey != custkey || e.day != day)
      return false;
    e.present = false;
  }
  if (!StoreEntry(s, orderkey, e))
    return false;
  return AdjustRevenue(s, day, custkey, sign * e.returned_rows,
                       sign * e.returned_volume);
}

bool ApplyReturnedLineitem(TpchQ10State &s, int64_t orderkey, int64_t volume,
                           int sign) {
  OrderEntry e;
  if (!LoadEntry(s, orderkey, e))
    return false;
  e.returned_rows += sign;
  if (e.returned_rows < 0 ||
      __builtin_add_overflow(e.returned_volume, sign * volume,
                             &e.returned_volume))
    return false;
  if (!StoreEntry(s, orderkey, e))
    return false;
  return !e.present || AdjustRevenue(s, e.day, e.custkey, sign, sign * volume);
}

} // namespace

core::AcceleratorId TpchQ10Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ10Module::Dependencies() const {
  // Projected columns (physical attribute numbers):
  //   customer : c_custkey, c_name, c_acctbal, c_nationkey, c_address,
  //              c_phone, c_comment
  //   nation   : n_nationkey, n_name
  //   orders   : o_orderkey, o_custkey, o_orderdate
  //   lineitem : l_orderkey, l_extendedprice, l_discount, l_returnflag
  return {{config.customer_table, {0, 1, 2, 3, 4, 5, 6}, {0, 1, 5, 3, 2, 4, 7}},
          {config.nation_table, {0, 1}, {0, 1}},
          {config.orders_table, {0, 1, 2}, {0, 1, 4}},
          {config.lineitem_table, {0, 1, 2, 3}, {0, 5, 6, 8}}};
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ10Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ10TxnBuffer>();
}

std::shared_ptr<TpchQ10State> TpchQ10Module::BuildState() const {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q10 requires a typed initial scan source");
  auto state = std::make_shared<TpchQ10State>();
  TpchQ10State &s = *state;

  initial_scan->ScanNations([&](const TpchQ10NationInput &input) {
    s.nations[input.key] = input.name;
  });
  initial_scan->ScanCustomers([&](const TpchQ10CustomerInput &input) {
    CustomerRec c{input.acctbal_raw, input.name, input.address, input.phone,
                  input.comment};
    if (!StoreCustomer(s, input.key, input.nationkey, c))
      throw std::runtime_error("TPC-H Q10: unsupported customer row");
  });

  // Orders first (entries without revenue), then the returned lineitems add
  // their count / revenue to their order's entry, then one pass turns the
  // entries into the per-(day, customer) revenue lists.
  uint64_t order_count = 0;
  initial_scan->ScanOrders([&](const TpchQ10OrderInput &input) {
    OrderEntry e;
    e.present = true;
    e.custkey = input.custkey;
    e.day = input.orderdate_days;
    if (!StoreEntry(s, input.key, e))
      throw std::runtime_error("TPC-H Q10: unsupported orders row");
    ++order_count;
  });
  initial_scan->ScanLineitems([&](const TpchQ10LineitemInput &input) {
    if (!input.returned)
      return;
    int64_t volume = 0;
    if (!VolumeRaw(input.extended_price_raw, input.extended_price_scale,
                   input.discount_raw, input.discount_scale, volume))
      throw std::runtime_error("TPC-H Q10: unsupported price / discount");
    OrderEntry e;
    if (!LoadEntry(s, input.orderkey, e))
      throw std::runtime_error("TPC-H Q10: corrupt order entry");
    ++e.returned_rows;
    if (__builtin_add_overflow(e.returned_volume, volume, &e.returned_volume) ||
        !StoreEntry(s, input.orderkey, e))
      throw std::runtime_error("TPC-H Q10: unsupported lineitem row");
  });

  // ~4 revenue rows per (day, bucket) list: scale the bucket count with the
  // order count (~2500 distinct order days).
  uint32_t buckets = 1;
  while (buckets < kMaxBuckets && order_count / 20000 >= buckets * 2ull)
    buckets *= 2;
  s.buckets = buckets;
  bool ok = true;
  s.orders.ForEach([&](int64_t, std::string_view v) {
    OrderEntry e;
    if (!DecodeEntry(v, e))
      ok = false;
    else if (e.present && e.returned_rows > 0 &&
             !AdjustRevenue(s, e.day, e.custkey, e.returned_rows,
                            e.returned_volume))
      ok = false;
  });
  if (!ok)
    throw std::runtime_error("TPC-H Q10: unsupported order entry");
  s.customers.Shrink();
  s.orders.Shrink();
  s.revenue.Shrink();
  return state;
}

void TpchQ10Module::Build(core::AuxEngineServices &,
                          core::StateBuilder &builder) {
  auto image = std::make_shared<TpchQ10StateImage>();
  image->state = BuildState();
  builder.Write(core::StateImage::Make(
      core::TpchQ10Layout::kAccelerator, kStatePartition, 0,
      std::shared_ptr<const TpchQ10StateImage>(std::move(image))));
  builder.Finish();
}

core::InvalidationPlan
TpchQ10Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.lineitem_table ||
      mutation.table == config.orders_table ||
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

void TpchQ10Module::AccumulateMutation(const core::MutationBatch &mutation,
                                       core::AuxEngineServices &services,
                                       core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ10TxnBuffer &>(base_buffer);
  if (buffer.requires_rebuild)
    return;
  if (!buffer.work) {
    // Fork the published state (pointer copies) for this transaction.
    const auto snapshot = services.StateStore().ReadPartitions(
        services.CurrentVisibility(), {kStatePartition});
    for (const auto &image : snapshot.partitions) {
      const auto *typed = image.partition == kStatePartition
                              ? image.As<TpchQ10StateImage>()
                              : nullptr;
      if (typed && typed->state) {
        buffer.work = std::make_shared<TpchQ10State>(*typed->state);
        buffer.base_version = image.version;
        break;
      }
    }
    if (!buffer.work) {
      buffer.MarkRebuild();
      return;
    }
  }
  TpchQ10State &s = *buffer.work;
  buffer.pending = true;
  buffer.rows += core::MutationRowCount(mutation);

  bool ok = true;
  if (mutation.table == config.nation_table) {
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
      std::string name;
      const Cell k = IntCell(row, 0, key);
      const Cell n = TextCell(row, 1, name);
      if (k == Cell::Bad || n == Cell::Bad) {
        ok = false;
        break;
      }
      // A NULL n_name still joins, with a NULL name in the result: not
      // representable here, but TPC-H declares n_name NOT NULL.
      if (k == Cell::Ok && n == Cell::Ok)
        s.nations[static_cast<int32_t>(key)] = name;
    }
  } else if (mutation.table == config.customer_table) {
    // Row: custkey, name, acctbal, nationkey, address, phone, comment.
    const auto apply = [&](const std::vector<core::AuxRow> &rows, int sign) {
      for (const auto &row : rows) {
        int64_t custkey = 0, nationkey = 0;
        CustomerRec c;
        int32_t scale = 0;
        const Cell cells[7] = {
            IntCell(row, 0, custkey),         TextCell(row, 1, c.name),
            DecimalCell(row, 2, c.acctbal_raw, scale),
            IntCell(row, 3, nationkey),       TextCell(row, 4, c.address),
            TextCell(row, 5, c.phone),        TextCell(row, 6, c.comment)};
        bool null_cell = false;
        for (const Cell cell : cells) {
          if (cell == Cell::Bad)
            return false;
          null_cell = null_cell || cell == Cell::Null;
        }
        if (cells[0] == Cell::Null)
          continue; // never joins
        if (null_cell || scale != 2)
          return false; // NOT NULL numeric(15,2) columns in TPC-H
        if (nationkey < std::numeric_limits<int32_t>::min() + 1 ||
            nationkey > std::numeric_limits<int32_t>::max())
          return false;
        if (sign < 0) {
          if (!s.customers.Contains(custkey))
            return false;
          s.customers.Erase(custkey);
          s.customer_nation.Set(custkey, TpchQ10State::kAbsent);
        } else {
          if (s.customers.Contains(custkey) ||
              !StoreCustomer(s, custkey, static_cast<int32_t>(nationkey), c))
            return false;
        }
      }
      return true;
    };
    ok = apply(mutation.old_rows, -1) && apply(mutation.new_rows, +1);
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
        int64_t orderkey = 0, price = 0, discount = 0;
        int32_t price_scale = 0, discount_scale = 0;
        std::string flag;
        const Cell cells[4] = {IntCell(row, 0, orderkey),
                               DecimalCell(row, 1, price, price_scale),
                               DecimalCell(row, 2, discount, discount_scale),
                               TextCell(row, 3, flag)};
        bool null_cell = false;
        for (const Cell c : cells) {
          if (c == Cell::Bad)
            return false;
          null_cell = null_cell || c == Cell::Null;
        }
        if (null_cell || flag != "R")
          continue; // not a returned lineitem
        int64_t volume = 0;
        if (!VolumeRaw(price, price_scale, discount, discount_scale, volume) ||
            !ApplyReturnedLineitem(s, orderkey, volume, sign))
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
TpchQ10Module::StageMaintenance(core::AuxEngineServices &services,
                                const core::InvalidationPlan &,
                                core::AuxTxnBuffer &base_buffer) {
  core::StagedMaintenanceOutcome outcome;
  auto &buffer = dynamic_cast<TpchQ10TxnBuffer &>(base_buffer);
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

  auto image = std::make_shared<TpchQ10StateImage>();
  image->state = std::move(buffer.work);
  outcome.action = core::MaintenanceAction::ApplyDelta;
  outcome.updates.push_back(core::StateImage::Make(
      core::TpchQ10Layout::kAccelerator, kStatePartition, buffer.base_version,
      std::shared_ptr<const TpchQ10StateImage>(std::move(image))));
  return outcome;
}

bool TpchQ10Module::CheckValidity(core::AuxEngineServices &services,
                                  const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void TpchQ10Module::Execute(core::AuxEngineServices &services,
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
                            ? image.As<TpchQ10StateImage>()
                            : nullptr;
    if (typed && typed->state) {
      const auto hold = image.payload; // keep the state alive while emitting
      EmitResult(*typed->state, arguments, services.WorkerCount(), sink);
      return;
    }
  }
  ExecuteFallback(services, arguments, sink);
}

void TpchQ10Module::ExecuteFallback(core::AuxEngineServices &services,
                                    const core::QueryArguments &arguments,
                                    core::AuxRowSink &sink) {
  const auto state = BuildState();
  EmitResult(*state, arguments, services.WorkerCount(), sink);
}

namespace {

struct Candidate {
  int64_t custkey = 0;
  Int128 revenue = 0;
};

// ORDER BY revenue DESC, c_custkey.
bool Better(const Candidate &a, const Candidate &b) {
  if (a.revenue != b.revenue)
    return a.revenue > b.revenue;
  return a.custkey < b.custkey;
}

// Keeps the best `limit` candidates seen (heap top = the worst kept one).
class TopK {
public:
  explicit TopK(std::size_t limit) : limit(limit) {}
  void Offer(const Candidate &c) {
    if (heap.size() < limit) {
      heap.push_back(c);
      std::push_heap(heap.begin(), heap.end(), Better);
    } else if (limit > 0 && Better(c, heap.front())) {
      std::pop_heap(heap.begin(), heap.end(), Better);
      heap.back() = c;
      std::push_heap(heap.begin(), heap.end(), Better);
    }
  }
  std::vector<Candidate> Sorted() {
    std::sort(heap.begin(), heap.end(), Better);
    return std::move(heap);
  }
  const std::vector<Candidate> &Items() const { return heap; }

private:
  std::size_t limit;
  std::vector<Candidate> heap;
};

// Customers of buckets [begin, end) with returned revenue on days
// [first_day, last_day]: merged per bucket, joined to customer / nation.
void ScanBuckets(const TpchQ10State &s, int64_t first_day, int64_t last_day,
                 uint32_t begin, uint32_t end, TopK &top) {
  std::vector<RevRow> rows;
  for (uint32_t b = begin; b < end; ++b) {
    rows.clear();
    for (int64_t day = first_day; day <= last_day; ++day) {
      std::string_view v;
      if (s.revenue.Get(RevenueKey(s.buckets, static_cast<int32_t>(day), b), v))
        DecodeRevRows(v, rows);
    }
    if (rows.empty())
      continue;
    std::sort(rows.begin(), rows.end(), [](const RevRow &l, const RevRow &r) {
      return l.custkey < r.custkey;
    });
    for (std::size_t i = 0; i < rows.size();) {
      const int64_t custkey = rows[i].custkey;
      Int128 revenue = 0;
      int64_t count = 0;
      for (; i < rows.size() && rows[i].custkey == custkey; ++i) {
        revenue += rows[i].revenue;
        count += rows[i].rows;
      }
      if (count <= 0)
        continue;
      const int32_t nk = s.customer_nation.Get(custkey);
      if (nk == TpchQ10State::kAbsent || s.nations.find(nk) == s.nations.end())
        continue;
      top.Offer({custkey, revenue});
    }
  }
}

} // namespace

void TpchQ10Module::EmitResult(const TpchQ10State &s,
                               const core::QueryArguments &arguments,
                               std::size_t workers,
                               core::AuxRowSink &sink) const {
  sink.Begin({{{"c_custkey", core::AuxType::Int64, false},
               {"c_name", core::AuxType::String, false},
               {"revenue", core::AuxType::String, false},
               {"c_acctbal", core::AuxType::String, false},
               {"n_name", core::AuxType::String, false},
               {"c_address", core::AuxType::String, false},
               {"c_phone", core::AuxType::String, false},
               {"c_comment", core::AuxType::String, false}}});

  // o_orderdate >= date AND o_orderdate < date + INTERVAL '3' MONTH. A NULL
  // date makes the predicate NULL: no rows.
  std::vector<Candidate> best;
  int64_t first_day = 0;
  bool have_date = false;
  if (!arguments.values.empty() && !arguments.values[0].is_null) {
    const auto &payload = arguments.values[0].payload;
    if (std::holds_alternative<core::AuxDate>(payload)) {
      first_day = std::get<core::AuxDate>(payload).days_since_epoch;
      have_date = true;
    } else if (std::holds_alternative<int64_t>(payload)) {
      first_day = std::get<int64_t>(payload); // days since the epoch
      have_date = true;
    }
  }
  if (have_date) {
    const int64_t end_day = sql_date::AddMonths(first_day, 3); // exclusive
    const int64_t last_day =
        std::min<int64_t>(end_day - 1, std::numeric_limits<int32_t>::max());
    if (first_day <= last_day) {
      const uint64_t gets =
          static_cast<uint64_t>(last_day - first_day + 1) * s.buckets;
      std::size_t threads = 1;
      if (gets >= (uint64_t{1} << 18) && workers > 1)
        threads = std::min<std::size_t>(
            {workers, static_cast<std::size_t>(s.buckets), 64});
      std::vector<TopK> tops(threads, TopK(kTpchQ10ResultLimit));
      if (threads == 1) {
        ScanBuckets(s, first_day, last_day, 0, s.buckets, tops[0]);
      } else {
        std::vector<std::thread> pool;
        for (std::size_t t = 0; t < threads; ++t) {
          const uint32_t lo = static_cast<uint32_t>(s.buckets * t / threads);
          const uint32_t hi = static_cast<uint32_t>(s.buckets * (t + 1) / threads);
          pool.emplace_back([&, t, lo, hi] {
            ScanBuckets(s, first_day, last_day, lo, hi, tops[t]);
          });
        }
        for (auto &th : pool)
          th.join();
      }
      TopK merged(kTpchQ10ResultLimit);
      for (auto &t : tops)
        for (const auto &c : t.Items())
          merged.Offer(c);
      best = merged.Sorted();
    }
  }

  for (const auto &c : best) {
    CustomerRec rec;
    if (!LoadCustomer(s, c.custkey, rec))
      continue;
    const auto nation = s.nations.find(s.customer_nation.Get(c.custkey));
    if (nation == s.nations.end())
      continue;
    namespace pgn = ::accelerator_codegen::pg_numeric;
    sink.WriteRow(
        {{core::AuxValue::Of(core::AuxType::Int64, c.custkey),
          core::AuxValue::Of(core::AuxType::String, rec.name),
          core::AuxValue::Of(core::AuxType::String,
                             pgn::ToText(c.revenue, kTpchQ10RevenueScale)),
          core::AuxValue::Of(core::AuxType::String,
                             pgn::ToText(rec.acctbal_raw, 2)),
          core::AuxValue::Of(core::AuxType::String, nation->second),
          core::AuxValue::Of(core::AuxType::String, rec.address),
          core::AuxValue::Of(core::AuxType::String, rec.phone),
          core::AuxValue::Of(core::AuxType::String, rec.comment)}});
  }
  sink.End();
}

std::vector<core::CalibrationWrite> TpchQ10Module::CalibrationWrites() const {
  return TpchQ10CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
