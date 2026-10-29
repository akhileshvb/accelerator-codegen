#include "tpch_q3_vgen_verus.h"

#include "substrate/verus/kit/verus_kit.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace kit = accelerator_codegen::verus_kit;
namespace core = accelerator_codegen::verus_kit::core;

namespace {

const char kQuerySql[] = R"sql(-- TPC-H Q3 (catalog text), :segment as $1, :date as $2, LIMIT 10.
SELECT l_orderkey, SUM(l_extendedprice * (1 - l_discount)) AS revenue, o_orderdate, o_shippriority
FROM customer, orders, lineitem
WHERE c_mktsegment = $1
  AND c_custkey = o_custkey
  AND l_orderkey = o_orderkey
  AND o_orderdate < $2
  AND l_shipdate > $2
GROUP BY l_orderkey, o_orderdate, o_shippriority
ORDER BY revenue DESC, o_orderdate
LIMIT 10
)sql";

const char kCustomersSql[] =
    "SELECT c_custkey, c_mktsegment FROM customer;";
const char kOrdersSql[] =
    "SELECT o_orderkey, o_custkey, o_orderdate, o_shippriority FROM orders;";
const char kShipmentsSql[] =
    "SELECT l_orderkey, l_shipdate, l_extendedprice, l_discount FROM lineitem;";

struct CustomerRows {
  std::vector<int64_t> c_custkey;
  kit::TextColumn c_mktsegment;
};

struct OrderRows {
  std::vector<tpchq3vgen_order_input> rows;
};

struct ShipmentRows {
  std::vector<tpchq3vgen_shipment_input> rows;
};

struct CustomerDelta {
  std::vector<int64_t> c_custkey;
  kit::TextColumn c_mktsegment;
  std::vector<int64_t> sign;
};

struct TransactionDeltas {
  CustomerDelta customer;
  std::vector<tpchq3vgen_order_key_delta> orders;
  std::vector<tpchq3vgen_shipment_key_delta> lineitem;
};

struct OrderRescan {
  std::vector<int64_t> keys;
  OrderRows rows;
};

struct ShipmentRescan {
  std::vector<int64_t> keys;
  ShipmentRows rows;
};

// Row count of a table, to size the scan buffers exactly (a growing vector would
// hold up to twice the rows while it reallocates).
size_t CountRows(kit::Host &host, const char *sql) {
  int64_t count = 0;
  host.Query(sql, [&](const core::AuxRow &row) { count = kit::I64(row, 0, 0); });
  return count > 0 ? static_cast<size_t>(count) : 0;
}

CustomerRows ReadCustomers(kit::Host &host) {
  CustomerRows customers;
  customers.c_custkey.reserve(CountRows(host, "SELECT COUNT(*) FROM customer;"));
  host.Query(kCustomersSql, [&](const core::AuxRow &row) {
    customers.c_custkey.push_back(kit::I64(row, 0, 0));
    customers.c_mktsegment.Push(kit::Text(row, 1));
  });
  return customers;
}

OrderRows ReadOrders(kit::Host &host) {
  OrderRows orders;
  orders.rows.reserve(CountRows(host, "SELECT COUNT(*) FROM orders;"));
  host.Query(kOrdersSql, [&](const core::AuxRow &row) {
    orders.rows.push_back({
        kit::I64(row, 0, 0),
        kit::I64(row, 1, 0),
        kit::Date(row, 2),
        kit::I64(row, 3, 0),
    });
  });
  return orders;
}

ShipmentRows ReadShipments(kit::Host &host) {
  ShipmentRows shipments;
  shipments.rows.reserve(CountRows(host, "SELECT COUNT(*) FROM lineitem;"));
  host.Query(kShipmentsSql, [&](const core::AuxRow &row) {
    shipments.rows.push_back({
        kit::I64(row, 0, 0),
        kit::Date(row, 1),
        kit::I64(row, 2, 2),
        kit::I64(row, 3, 2),
    });
  });
  return shipments;
}

TransactionDeltas ReadDeltas(const kit::Changes &changes) {
  TransactionDeltas deltas;

  const auto &customer_changes = changes.at(0);
  deltas.customer.c_custkey.reserve(customer_changes.size());
  deltas.customer.sign.reserve(customer_changes.size());
  for (const auto &change : customer_changes) {
    deltas.customer.c_custkey.push_back(kit::I64(change.row, 0, 0));
    deltas.customer.c_mktsegment.Push(kit::Text(change.row, 1));
    deltas.customer.sign.push_back(change.sign);
  }

  const auto &order_changes = changes.at(1);
  deltas.orders.reserve(order_changes.size());
  for (const auto &change : order_changes) {
    deltas.orders.push_back({
        kit::I64(change.row, 0, 0),
        change.sign,
    });
  }

  const auto &lineitem_changes = changes.at(2);
  deltas.lineitem.reserve(lineitem_changes.size());
  for (const auto &change : lineitem_changes) {
    deltas.lineitem.push_back({
        kit::I64(change.row, 0, 0),
        change.sign,
    });
  }

  return deltas;
}

using RescanKeysFn = size_t (*)(
    const tpchq3vgen_state *, size_t, const int64_t *, const uint8_t *,
    const size_t *, const int64_t *, size_t,
    const tpchq3vgen_shipment_key_delta *, size_t,
    const tpchq3vgen_order_key_delta *, int64_t *, size_t);

std::vector<int64_t> ReadRescanKeys(
    RescanKeysFn fn, const tpchq3vgen_state *state,
    const TransactionDeltas &deltas) {
  std::vector<int64_t> keys(64);
  while (true) {
    const size_t count =
        fn(state, deltas.customer.c_custkey.size(),
           deltas.customer.c_custkey.data(),
           deltas.customer.c_mktsegment.bytes.data(),
           deltas.customer.c_mktsegment.off.data(),
           deltas.customer.sign.data(), deltas.lineitem.size(),
           deltas.lineitem.data(), deltas.orders.size(), deltas.orders.data(),
           keys.data(), keys.size());
    if (count <= keys.size()) {
      keys.resize(count);
      return keys;
    }
    keys.resize(count);
  }
}

OrderRescan ReadOrdersRescan(kit::Host &host,
                             const std::vector<int64_t> &keys) {
  OrderRescan rescan;
  rescan.keys = keys;
  kit::QueryKeys(
      host, kOrdersSql, "o_orderkey", keys, [&](const core::AuxRow &row) {
        rescan.rows.rows.push_back({
            kit::I64(row, 0, 0),
            kit::I64(row, 1, 0),
            kit::Date(row, 2),
            kit::I64(row, 3, 0),
        });
      });
  return rescan;
}

ShipmentRescan ReadShipmentsRescan(kit::Host &host,
                                   const std::vector<int64_t> &keys) {
  ShipmentRescan rescan;
  rescan.keys = keys;
  kit::QueryKeys(
      host, kShipmentsSql, "l_orderkey", keys,
      [&](const core::AuxRow &row) {
        rescan.rows.rows.push_back({
            kit::I64(row, 0, 0),
            kit::Date(row, 1),
            kit::I64(row, 2, 2),
            kit::I64(row, 3, 2),
        });
      });
  return rescan;
}

void *Build(kit::Host &host) {
  CustomerRows customers = ReadCustomers(host);
  OrderRows orders = ReadOrders(host);
  ShipmentRows shipments = ReadShipments(host);
  return tpchq3vgen_build(
      customers.c_custkey.size(), customers.c_custkey.data(),
      customers.c_mktsegment.bytes.data(),
      customers.c_mktsegment.off.data(), orders.rows.size(),
      orders.rows.data(), shipments.rows.size(), shipments.rows.data());
}

void FreeState(void *state) {
  tpchq3vgen_free(static_cast<tpchq3vgen_state *>(state));
}

void *Apply(const void *state, const kit::Changes &changes, kit::Host &host,
            int32_t *err) {
  const auto *kernel_state = static_cast<const tpchq3vgen_state *>(state);
  TransactionDeltas deltas = ReadDeltas(changes);

  std::vector<int64_t> order_keys = ReadRescanKeys(
      tpchq3vgen_rescan_orders_by_key_keys, kernel_state, deltas);
  OrderRescan orders = ReadOrdersRescan(host, order_keys);

  std::vector<int64_t> shipment_keys = ReadRescanKeys(
      tpchq3vgen_rescan_shipments_by_key_keys, kernel_state, deltas);
  ShipmentRescan shipments = ReadShipmentsRescan(host, shipment_keys);

  return tpchq3vgen_apply(
      kernel_state, deltas.customer.c_custkey.size(),
      deltas.customer.c_custkey.data(),
      deltas.customer.c_mktsegment.bytes.data(),
      deltas.customer.c_mktsegment.off.data(),
      deltas.customer.sign.data(), deltas.lineitem.size(),
      deltas.lineitem.data(), deltas.orders.size(), deltas.orders.data(),
      orders.keys.size(), orders.keys.data(), orders.rows.rows.size(),
      orders.rows.rows.data(), shipments.keys.size(), shipments.keys.data(),
      shipments.rows.rows.size(), shipments.rows.rows.data(), err);
}

template <class Call>
bool Run(const kit::core::QueryArguments &args, std::vector<kit::core::AuxRow> &out, Call call) {
  std::string segment;
  int32_t cutoff_date = 0;
  if (!kit::ArgText(args, 0, segment) ||
      !kit::ArgDate(args, 1, cutoff_date)) {
    return false;
  }

  std::unique_ptr<tpchq3vgen_result, void (*)(tpchq3vgen_result *)> result(
      call(reinterpret_cast<const uint8_t *>(segment.data()),
                         segment.size(), cutoff_date),
      tpchq3vgen_result_free);
  if (!result) {
    return false;
  }

  const size_t count = tpchq3vgen_result_len(result.get());
  out.reserve(out.size() + count);
  for (size_t i = 0; i < count; ++i) {
    int64_t c0 = 0;
    int64_t c1_hi = 0;
    uint64_t c1_lo = 0;
    int32_t c2 = 0;
    int64_t c3 = 0;
    const uint8_t present0 =
        tpchq3vgen_result_c0(result.get(), i, &c0);
    const uint8_t present1 =
        tpchq3vgen_result_c1(result.get(), i, &c1_hi, &c1_lo);
    const uint8_t present2 =
        tpchq3vgen_result_c2(result.get(), i, &c2);
    const uint8_t present3 =
        tpchq3vgen_result_c3(result.get(), i, &c3);

    core::AuxRow row;
    row.values.reserve(4);
    row.values.push_back(present0 ? kit::OutI64(c0)
                                  : kit::OutNull(core::AuxType::Int64));
    row.values.push_back(
        present1 ? kit::OutDec(c1_hi, c1_lo, 4)
                 : kit::OutNull(core::AuxType::ScaledDecimal));
    row.values.push_back(present2 ? kit::OutDate(c2)
                                  : kit::OutNull(core::AuxType::Date));
    row.values.push_back(present3 ? kit::OutI64(c3)
                                  : kit::OutNull(core::AuxType::Int64));
    out.push_back(std::move(row));
  }
  return true;
}

bool Execute(const void *state, const kit::core::QueryArguments &args,
             std::vector<kit::core::AuxRow> &out) {
  return Run(args, out, [&](auto... x) {
    return tpchq3vgen_execute(static_cast<const tpchq3vgen_state *>(state), x...);
  });
}

bool Answer(const void *state, const kit::core::QueryArguments &args,
            std::vector<kit::core::AuxRow> &out) {
  return Run(args, out, [&](auto... x) {
    return tpchq3vgens_answer(static_cast<const tpchq3vgens_reading *>(state), x...);
  });
}

void *PrepareApply(const void *base,
                   const kit::Changes &changes, kit::Host &host,
                   int32_t *err) {
  const tpchq3vgen_state *base_state = tpchq3vgens_base_state(
      static_cast<const tpchq3vgens_base *>(base));
  TransactionDeltas deltas = ReadDeltas(changes);

  std::vector<int64_t> order_keys = ReadRescanKeys(
      tpchq3vgen_rescan_orders_by_key_keys, base_state, deltas);
  OrderRescan orders = ReadOrdersRescan(host, order_keys);

  std::vector<int64_t> shipment_keys = ReadRescanKeys(
      tpchq3vgen_rescan_shipments_by_key_keys, base_state, deltas);
  ShipmentRescan shipments = ReadShipmentsRescan(host, shipment_keys);

  return tpchq3vgens_stage_apply(
      static_cast<const tpchq3vgens_base *>(base),
      deltas.customer.c_custkey.size(), deltas.customer.c_custkey.data(),
      deltas.customer.c_mktsegment.bytes.data(),
      deltas.customer.c_mktsegment.off.data(),
      deltas.customer.sign.data(), deltas.lineitem.size(),
      deltas.lineitem.data(), deltas.orders.size(), deltas.orders.data(),
      orders.keys.size(), orders.keys.data(), orders.rows.rows.size(),
      orders.rows.rows.data(), shipments.keys.size(), shipments.keys.data(),
      shipments.rows.rows.size(), shipments.rows.rows.data(), err);
}

void *PrepareBuild(const kit::HeadFn &head_of, kit::Host &host) {
  CustomerRows customers = ReadCustomers(host);
  OrderRows orders = ReadOrders(host);
  ShipmentRows shipments = ReadShipments(host);
  return tpchq3vgens_stage_build(
      static_cast<const tpchq3vgens_head *>(head_of()),
      customers.c_custkey.size(), customers.c_custkey.data(),
      customers.c_mktsegment.bytes.data(),
      customers.c_mktsegment.off.data(), orders.rows.size(),
      orders.rows.data(), shipments.rows.size(), shipments.rows.data());
}

const kit::Glue kGlue = {
    "tpch_q3_vgen",
    "tpch",
    3,
    kQuerySql,
    {
        {"customer", {"c_custkey", "c_mktsegment"}},
        {"orders", {"o_orderkey"}},
        {"lineitem", {"l_orderkey"}},
    },
    {kit::Kind::Text, kit::Kind::Date},
    Build,
    FreeState,
    Apply,
    Execute,
    Answer,
    PrepareApply,
    PrepareBuild,
    kit::StoreOps{
        []() -> void * { return tpchq3vgens_new(); },
        [](void *store) { tpchq3vgens_free(static_cast<tpchq3vgens_store *>(store)); },
        [](const void *store) -> void * { return tpchq3vgens_newest_base(static_cast<const tpchq3vgens_store *>(store)); },
        [](const void *base) -> const void * { return tpchq3vgens_base_state(static_cast<const tpchq3vgens_base *>(base)); },
        [](void *base) { tpchq3vgens_base_free(static_cast<tpchq3vgens_base *>(base)); },
        [](const void *store) -> void * { return tpchq3vgens_newest_head(static_cast<const tpchq3vgens_store *>(store)); },
        [](void *head) { tpchq3vgens_head_free(static_cast<tpchq3vgens_head *>(head)); },
        [](const void *head) -> void * { return tpchq3vgens_stage_invalid(static_cast<const tpchq3vgens_head *>(head)); },
        [](void *staged) { tpchq3vgens_staged_free(static_cast<tpchq3vgens_staged *>(staged)); },
        [](const void *store, uint64_t from) -> int32_t { return tpchq3vgens_begin(static_cast<const tpchq3vgens_store *>(store), from); },
        [](const void *store) { tpchq3vgens_abort(static_cast<const tpchq3vgens_store *>(store)); },
        [](const void *store, void *staged, uint64_t from, uint64_t floor, int32_t end_commit) -> int32_t {
            return tpchq3vgens_install(static_cast<const tpchq3vgens_store *>(store), static_cast<tpchq3vgens_staged *>(staged), from, floor, end_commit);
        },
        [](const void *store, uint64_t at, uint64_t tries, int32_t *status) -> const void * {
            return tpchq3vgens_read(static_cast<const tpchq3vgens_store *>(store), at, tries, status);
        },
        [](const void *reading) { tpchq3vgens_reading_free(const_cast<tpchq3vgens_reading *>(static_cast<const tpchq3vgens_reading *>(reading))); },
        [](const void *store) { tpchq3vgens_invalidate_all(static_cast<const tpchq3vgens_store *>(store)); },
        [](const void *store) -> std::size_t { return tpchq3vgens_version_count(static_cast<const tpchq3vgens_store *>(store)); },
        []() -> std::size_t { return tpchq3vgens_heap_bytes(); },
        []() -> std::size_t { return tpchq3vgens_heap_peak_bytes(); },
        []() { tpchq3vgens_heap_reset_peak(); },
        [](const void *head, void *state) -> void * { return tpchq3vgens_stage_adopt(static_cast<const tpchq3vgens_head *>(head), static_cast<tpchq3vgen_state *>(state)); },
    },
};

const kit::Registrar kRegistrar(&kGlue);

} // namespace
