#include "tpch_q3_vgen_verus.h"
#include "accelerator_codegen/verus/verus_kit.hpp"

#include <memory>
#include <utility>

namespace {

namespace kit = accelerator_codegen::verus_kit;
namespace core = accelerator_codegen::aux_state::core;

constexpr const char *kQuerySql = R"SQL(-- TPC-H Q3 (catalog text), :segment as $1, :date as $2, LIMIT 10.
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
)SQL";

constexpr const char *kCustomersSql =
    "SELECT c_custkey, c_mktsegment FROM customer";
constexpr const char *kOrdersSql =
    "SELECT o_orderkey, o_custkey, o_orderdate, o_shippriority FROM orders";
constexpr const char *kShipmentsSql =
    "SELECT l_orderkey, l_extendedprice, l_discount, l_shipdate FROM lineitem";

template <typename Row>
Row ReadOrder(const core::AuxRow &row) {
  return {kit::I64(row, 0, 0), kit::I64(row, 1, 0), kit::Date(row, 2),
          kit::I64(row, 3, 0)};
}

template <typename Row>
Row ReadShipment(const core::AuxRow &row) {
  return {kit::I64(row, 0, 0), kit::I64(row, 1, 2), kit::I64(row, 2, 2),
          kit::Date(row, 3)};
}

struct BuildInputs {
  std::vector<int64_t> customer_keys;
  kit::TextColumn customer_segments;
  std::vector<tpchq3vgen_orders_scan> orders;
  std::vector<tpchq3vgen_shipment_scan> shipments;

  explicit BuildInputs(kit::Host &host) {
    host.Query(kCustomersSql, [&](const core::AuxRow &row) {
      customer_keys.push_back(kit::I64(row, 0, 0));
      customer_segments.Push(kit::Text(row, 1));
    });
    host.Query(kOrdersSql, [&](const core::AuxRow &row) {
      orders.push_back(ReadOrder<tpchq3vgen_orders_scan>(row));
    });
    host.Query(kShipmentsSql, [&](const core::AuxRow &row) {
      shipments.push_back(ReadShipment<tpchq3vgen_shipment_scan>(row));
    });
  }
};

void *Build(kit::Host &host) {
  const BuildInputs inputs(host);
  return tpchq3vgen_build(
      inputs.customer_keys.size(), inputs.customer_keys.data(),
      inputs.customer_segments.bytes.data(), inputs.customer_segments.off.data(),
      inputs.orders.size(), inputs.orders.data(), inputs.shipments.size(),
      inputs.shipments.data());
}

void *PrepareBuild(const void *store, kit::Host &host) {
  const BuildInputs inputs(host);
  return tpchq3vgens_prepare_build(
      static_cast<const tpchq3vgens_store *>(store), inputs.customer_keys.size(),
      inputs.customer_keys.data(), inputs.customer_segments.bytes.data(),
      inputs.customer_segments.off.data(), inputs.orders.size(),
      inputs.orders.data(), inputs.shipments.size(), inputs.shipments.data());
}

struct ApplyInputs {
  std::vector<int64_t> customer_keys;
  kit::TextColumn customer_segments;
  std::vector<int64_t> customer_signs;
  std::vector<tpchq3vgen_shipment_delta> shipments;
  std::vector<tpchq3vgen_orders_delta> orders;
  std::vector<int64_t> rescan_keys;
  std::vector<tpchq3vgen_customer_orders_rescan> rescan_rows;

  ApplyInputs(const tpchq3vgen_state *state, const kit::Changes &changes,
              kit::Host &host) {
    const auto &customer_changes = changes.at(0);
    const auto &shipment_changes = changes.at(1);
    const auto &order_changes = changes.at(2);
    customer_keys.reserve(customer_changes.size());
    customer_segments.off.reserve(customer_changes.size() + 1);
    customer_signs.reserve(customer_changes.size());
    shipments.reserve(shipment_changes.size());
    orders.reserve(order_changes.size());

    for (const auto &change : customer_changes) {
      customer_keys.push_back(kit::I64(change.row, 0, 0));
      customer_segments.Push(kit::Text(change.row, 1));
      customer_signs.push_back(change.sign);
    }
    for (const auto &change : shipment_changes) {
      shipments.push_back(
          {ReadShipment<tpchq3vgen_shipment_input>(change.row), change.sign});
    }
    for (const auto &change : order_changes) {
      orders.push_back(
          {ReadOrder<tpchq3vgen_orders_input>(change.row), change.sign});
    }

    rescan_keys.resize(customer_changes.size());
    for (;;) {
      const std::size_t count = tpchq3vgen_rescan_customer_orders_keys(
          state, customer_keys.size(), customer_keys.data(),
          customer_segments.bytes.data(), customer_segments.off.data(),
          customer_signs.data(), shipments.size(), shipments.data(),
          orders.size(), orders.data(), rescan_keys.data(), rescan_keys.size());
      if (count <= rescan_keys.size()) {
        rescan_keys.resize(count);
        break;
      }
      rescan_keys.resize(count);
    }
    kit::QueryKeys(host, kOrdersSql, "o_custkey", rescan_keys,
                   [&](const core::AuxRow &row) {
                     rescan_rows.push_back(
                         ReadOrder<tpchq3vgen_customer_orders_rescan>(row));
                   });
  }
};

void *Apply(const void *state, const kit::Changes &changes, kit::Host &host,
            int32_t *err) {
  const auto *kernel_state = static_cast<const tpchq3vgen_state *>(state);
  const ApplyInputs inputs(kernel_state, changes, host);
  return tpchq3vgen_apply(
      kernel_state, inputs.customer_keys.size(), inputs.customer_keys.data(),
      inputs.customer_segments.bytes.data(), inputs.customer_segments.off.data(),
      inputs.customer_signs.data(), inputs.shipments.size(),
      inputs.shipments.data(), inputs.orders.size(), inputs.orders.data(),
      inputs.rescan_keys.size(), inputs.rescan_keys.data(),
      inputs.rescan_rows.size(), inputs.rescan_rows.data(), err);
}

void *PrepareApply(const void *store, const void *base,
                   const kit::Changes &changes, kit::Host &host, int32_t *err) {
  const auto *kernel_base = static_cast<const tpchq3vgens_base *>(base);
  const ApplyInputs inputs(tpchq3vgens_base_state(kernel_base), changes, host);
  return tpchq3vgens_prepare_apply(
      static_cast<const tpchq3vgens_store *>(store), kernel_base,
      inputs.customer_keys.size(), inputs.customer_keys.data(),
      inputs.customer_segments.bytes.data(), inputs.customer_segments.off.data(),
      inputs.customer_signs.data(), inputs.shipments.size(),
      inputs.shipments.data(), inputs.orders.size(), inputs.orders.data(),
      inputs.rescan_keys.size(), inputs.rescan_keys.data(),
      inputs.rescan_rows.size(), inputs.rescan_rows.data(), err);
}

void FreeState(void *state) {
  tpchq3vgen_free(static_cast<tpchq3vgen_state *>(state));
}

bool Execute(const void *state, const core::QueryArguments &args,
             std::vector<core::AuxRow> &out) {
  std::string segment;
  int32_t date;
  if (!kit::ArgText(args, 0, segment) || !kit::ArgDate(args, 1, date)) {
    return false;
  }
  std::unique_ptr<tpchq3vgen_result, decltype(&tpchq3vgen_result_free)> result(
      tpchq3vgen_execute(static_cast<const tpchq3vgen_state *>(state),
                        reinterpret_cast<const uint8_t *>(segment.data()),
                        segment.size(), date),
      &tpchq3vgen_result_free);
  if (!result) {
    return false;
  }
  const std::size_t count = tpchq3vgen_result_len(result.get());
  out.reserve(out.size() + count);
  for (std::size_t index = 0; index < count; ++index) {
    core::AuxRow row;
    row.values.reserve(4);
    int64_t order_key;
    int64_t revenue_hi;
    uint64_t revenue_lo;
    int32_t order_date;
    int64_t ship_priority;
    row.values.push_back(
        tpchq3vgen_result_c0(result.get(), index, &order_key)
            ? kit::OutI64(order_key)
            : kit::OutNull(core::AuxType::Int64));
    row.values.push_back(
        tpchq3vgen_result_c1(result.get(), index, &revenue_hi, &revenue_lo)
            ? kit::OutDec(revenue_hi, revenue_lo, 4)
            : kit::OutNull(core::AuxType::ScaledDecimal));
    row.values.push_back(
        tpchq3vgen_result_c2(result.get(), index, &order_date)
            ? kit::OutDate(order_date)
            : kit::OutNull(core::AuxType::Date));
    row.values.push_back(
        tpchq3vgen_result_c3(result.get(), index, &ship_priority)
            ? kit::OutI64(ship_priority)
            : kit::OutNull(core::AuxType::Int64));
    out.push_back(std::move(row));
  }
  return true;
}

const kit::Glue kGlue = {
    "tpch_q3_vgen", "tpch", 3, kQuerySql,
    {{"customer", {"c_custkey", "c_mktsegment"}},
     {"lineitem", {"l_orderkey", "l_extendedprice", "l_discount", "l_shipdate"}},
     {"orders", {"o_orderkey", "o_custkey", "o_orderdate", "o_shippriority"}}},
    {kit::Kind::Text, kit::Kind::Date},
    Build, FreeState, Apply, Execute, PrepareApply, PrepareBuild,
    kit::StoreOps{
        []() -> void * { return tpchq3vgens_new(); },
        [](void *store) {
          tpchq3vgens_free(static_cast<tpchq3vgens_store *>(store));
        },
        [](const void *store) -> void * {
          return tpchq3vgens_newest_base(
              static_cast<const tpchq3vgens_store *>(store));
        },
        [](const void *base) -> const void * {
          return tpchq3vgens_base_state(
              static_cast<const tpchq3vgens_base *>(base));
        },
        [](void *base) {
          tpchq3vgens_base_free(static_cast<tpchq3vgens_base *>(base));
        },
        [](const void *store) -> void * {
          return tpchq3vgens_prepare_invalid(
              static_cast<const tpchq3vgens_store *>(store));
        },
        [](void *prepared) {
          tpchq3vgens_prepared_free(
              static_cast<tpchq3vgens_prepared *>(prepared));
        },
        [](const void *store, uint64_t from) -> int32_t {
          return tpchq3vgens_begin(
              static_cast<const tpchq3vgens_store *>(store), from);
        },
        [](const void *store) {
          tpchq3vgens_abort(static_cast<const tpchq3vgens_store *>(store));
        },
        [](const void *store, void *prepared, uint64_t from, uint64_t floor,
           int32_t end_commit) -> int32_t {
          return tpchq3vgens_install(
              static_cast<const tpchq3vgens_store *>(store),
              static_cast<tpchq3vgens_prepared *>(prepared), from, floor,
              end_commit);
        },
        [](const void *store, uint64_t at, uint64_t tries,
           int32_t *status) -> const void * {
          return tpchq3vgens_read(
              static_cast<const tpchq3vgens_store *>(store), at, tries, status);
        },
        [](const void *state) {
          tpchq3vgens_state_release(
              static_cast<const tpchq3vgen_state *>(state));
        },
        [](const void *store) {
          tpchq3vgens_invalidate_all(
              static_cast<const tpchq3vgens_store *>(store));
        },
        [](const void *store) -> std::size_t {
          return tpchq3vgens_version_count(
              static_cast<const tpchq3vgens_store *>(store));
        },
    },
};

const kit::Registrar kRegistrar(&kGlue);

}
