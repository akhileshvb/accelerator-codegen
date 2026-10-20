#include "tpch/q10/mysql/aux/module.hpp"
#include "tpch/q10/calibration_writes.hpp"
#include "substrate/aux/core/aux_cells.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "prims/decimal_text.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/partitioned_build.hpp"
#include "substrate/aux/core/query_module_helpers.hpp"
#include "prims/sql_date.hpp"
#include "tpch/q10/layout.hpp"

namespace accelerator_codegen::aux_state::query {

namespace {

namespace sql_date = core::sql_date;
using Int128 = ::accelerator_codegen::aux_state::core::decimal_text::Int128;

constexpr core::PartitionId kStatePartition =
    core::TpchQ10Layout::kCustomerDimensions;
constexpr uint32_t kMaxBuckets = 65536;
using Cell = core::CellState;

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
  explicit TopK(std::size_t limit = kTpchQ10ResultLimit) : limit(limit) {}
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

std::shared_ptr<TpchQ10State>
TpchQ10Module::BuildState(const core::AuxEngineServices &services) const {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q10 requires a typed initial scan source");
  auto state = std::make_shared<TpchQ10State>();
  TpchQ10State &s = *state;

  initial_scan->ScanNationsSerial([&](const TpchQ10NationInput &input) {
    s.nations[input.key] = input.name;
  });
  // The scans run in parallel; the rows are applied in partition order.
  const std::size_t partitions = core::BuildPartitions(services);
  for (const auto &input : core::CollectRows<TpchQ10CustomerInput>(
           services, [&](std::size_t parts, auto &&sink) {
             initial_scan->ScanCustomers(services, parts, sink);
           })) {
    CustomerRec c{input.acctbal_raw, input.name, input.address, input.phone,
                  input.comment};
    if (!StoreCustomer(s, input.key, input.nationkey, c))
      throw std::runtime_error("TPC-H Q10: unsupported customer row");
  }

  // Orders first (entries without revenue), then the returned lineitems add
  // their count / revenue to their order's entry, then one pass turns the
  // entries into the per-(day, customer) revenue lists.
  uint64_t order_count = 0;
  for (const auto &input : core::CollectRows<TpchQ10OrderInput>(
           services, [&](std::size_t parts, auto &&sink) {
             initial_scan->ScanOrders(services, parts, sink);
           })) {
    OrderEntry e;
    e.present = true;
    e.custkey = input.custkey;
    e.day = input.orderdate_days;
    if (!StoreEntry(s, input.key, e))
      throw std::runtime_error("TPC-H Q10: unsupported orders row");
    ++order_count;
  }
  struct ReturnedLine {
    int64_t orderkey;
    int64_t volume;
  };
  const auto returned = core::PartitionedBuild<std::vector<ReturnedLine>>(
      services, [] { return std::vector<ReturnedLine>{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanLineitems(
            services, parts, [&](std::size_t partition, const TpchQ10LineitemInput &input) {
              sink(partition, core::SingleRowBatch<TpchQ10LineitemInput>{input});
            });
      },
      [](std::vector<ReturnedLine> &into, const TpchQ10LineitemInput &input) {
        if (!input.returned)
          return;
        int64_t volume = 0;
        if (!VolumeRaw(input.extended_price_raw, input.extended_price_scale,
                       input.discount_raw, input.discount_scale, volume))
          throw std::runtime_error("TPC-H Q10: unsupported price / discount");
        into.push_back({input.orderkey, volume});
      },
      [](std::vector<ReturnedLine> &into, std::vector<ReturnedLine> &&from) {
        into.insert(into.end(), from.begin(), from.end());
      },
      partitions);
  for (const auto &line : returned) {
    OrderEntry e;
    if (!LoadEntry(s, line.orderkey, e))
      throw std::runtime_error("TPC-H Q10: corrupt order entry");
    ++e.returned_rows;
    if (__builtin_add_overflow(e.returned_volume, line.volume, &e.returned_volume) ||
        !StoreEntry(s, line.orderkey, e))
      throw std::runtime_error("TPC-H Q10: unsupported lineitem row");
  }

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

void TpchQ10Module::Build(core::AuxEngineServices &services,
                          core::StateBuilder &builder) {
  auto image = std::make_shared<TpchQ10StateImage>();
  image->state = BuildState(services);
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
      std::string name;
      const Cell k = core::NullableIntCell(row, 0, key);
      const Cell n = core::NullableStringCell(row, 1, name);
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
            core::NullableIntCell(row, 0, custkey),         core::NullableStringCell(row, 1, c.name),
            core::NullableDecimalCell(row, 2, c.acctbal_raw, scale),
            core::NullableIntCell(row, 3, nationkey),       core::NullableStringCell(row, 4, c.address),
            core::NullableStringCell(row, 5, c.phone),        core::NullableStringCell(row, 6, c.comment)};
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
        const Cell a = core::NullableIntCell(row, 0, orderkey);
        const Cell b = core::NullableIntCell(row, 1, custkey);
        const Cell d = core::NullableDateCell(row, 2, day);
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
        const Cell cells[4] = {core::NullableIntCell(row, 0, orderkey),
                               core::NullableDecimalCell(row, 1, price, price_scale),
                               core::NullableDecimalCell(row, 2, discount, discount_scale),
                               core::NullableStringCell(row, 3, flag)};
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
      EmitResult(*typed->state, arguments, services, sink);
      return;
    }
  }
  ExecuteFallback(services, arguments, sink);
}

void TpchQ10Module::EmitResult(const TpchQ10State &s,
                               const core::QueryArguments &arguments,
                               const core::AuxEngineServices &services,
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
      // About 2^18 bucket-day lookups pay for a slice.
      const std::size_t grain = std::max<std::size_t>(
          1, (std::size_t{1} << 18) / static_cast<std::size_t>(last_day - first_day + 1));
      TopK top;
      core::ParallelReduce(
          services, s.buckets, grain, top,
          [&](TopK &acc, std::size_t begin, std::size_t end) {
            ScanBuckets(s, first_day, last_day, static_cast<uint32_t>(begin),
                        static_cast<uint32_t>(end), acc);
          },
          [](TopK &into, TopK &&from) {
            for (const auto &c : from.Items())
              into.Offer(c);
          });
      best = top.Sorted();
    }
  }

  for (const auto &c : best) {
    CustomerRec rec;
    if (!LoadCustomer(s, c.custkey, rec))
      continue;
    const auto nation = s.nations.find(s.customer_nation.Get(c.custkey));
    if (nation == s.nations.end())
      continue;
    namespace dec = ::accelerator_codegen::aux_state::core::decimal_text;
    sink.WriteRow(
        {{core::AuxValue::Of(core::AuxType::Int64, c.custkey),
          core::AuxValue::Of(core::AuxType::String, rec.name),
          core::AuxValue::Of(core::AuxType::String,
                             dec::ToText(c.revenue, kTpchQ10RevenueScale)),
          core::AuxValue::Of(core::AuxType::String,
                             dec::ToText(rec.acctbal_raw, 2)),
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
