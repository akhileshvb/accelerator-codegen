#include "tpch_q7_vgen_verus.h"
#include "substrate/verus/kit/verus_kit.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace kit = accelerator_codegen::verus_kit;
namespace core = accelerator_codegen::aux_state::core;

constexpr char kQuerySql[] = R"SQL(-- TPC-H Q7 (catalog text), :nation1 as $1, :nation2 as $2.
SELECT supp_nation, cust_nation, l_year, SUM(volume) AS revenue
FROM (SELECT n1.n_name AS supp_nation, n2.n_name AS cust_nation,
             EXTRACT(year FROM l_shipdate) AS l_year,
             l_extendedprice * (1 - l_discount) AS volume
      FROM supplier, lineitem, orders, customer, nation n1, nation n2
      WHERE s_suppkey = l_suppkey AND o_orderkey = l_orderkey
        AND c_custkey = o_custkey AND s_nationkey = n1.n_nationkey
        AND c_nationkey = n2.n_nationkey
        AND ((n1.n_name = $1 AND n2.n_name = $2)
          OR (n1.n_name = $2 AND n2.n_name = $1))
        AND l_shipdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31') AS shipping
GROUP BY supp_nation, cust_nation, l_year
ORDER BY supp_nation, cust_nation, l_year
)SQL";

constexpr char kSuppliersSql[] =
    "SELECT s_suppkey, s_nationkey FROM supplier;";
constexpr char kCustomersSql[] =
    "SELECT c_custkey, c_nationkey FROM customer;";
constexpr char kNationsSql[] =
    "SELECT n_nationkey, n_name FROM nation;";
constexpr char kOrdersSql[] = R"SQL(SELECT o_orderkey, o_custkey FROM orders
  WHERE EXISTS (
    SELECT 1 FROM lineitem
    WHERE l_orderkey = o_orderkey
      AND l_shipdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31'
  );)SQL";
constexpr char kLinesSql[] = R"SQL(SELECT l_orderkey, l_suppkey, l_extendedprice, l_discount, l_shipdate
  FROM lineitem
  WHERE l_shipdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31';)SQL";

tpchq7vgen_supplier_input SupplierRow(const core::AuxRow &row) {
  return {kit::I64(row, 0, 0), kit::I64(row, 1, 0)};
}

tpchq7vgen_customer_input CustomerRow(const core::AuxRow &row) {
  return {kit::I64(row, 0, 0), kit::I64(row, 1, 0)};
}

tpchq7vgen_order_input OrderRow(const core::AuxRow &row) {
  return {kit::I64(row, 0, 0), kit::I64(row, 1, 0)};
}

tpchq7vgen_line_input LineRow(const core::AuxRow &row) {
  return {kit::I64(row, 0, 0), kit::I64(row, 1, 0),
          kit::I64(row, 2, 2), kit::I64(row, 3, 2), kit::Date(row, 4)};
}

struct BuildInputs {
  std::vector<tpchq7vgen_supplier_input> suppliers;
  std::vector<tpchq7vgen_customer_input> customers;
  std::vector<int64_t> nation_keys;
  kit::TextColumn nation_names;
  std::vector<tpchq7vgen_order_input> orders;
  std::vector<tpchq7vgen_line_input> lines;

  explicit BuildInputs(kit::Host &host) {
    host.Query(kSuppliersSql, [&](const core::AuxRow &row) {
      suppliers.push_back(SupplierRow(row));
    });
    host.Query(kCustomersSql, [&](const core::AuxRow &row) {
      customers.push_back(CustomerRow(row));
    });
    host.Query(kNationsSql, [&](const core::AuxRow &row) {
      nation_keys.push_back(kit::I64(row, 0, 0));
      nation_names.Push(kit::Text(row, 1));
    });
    host.Query(kOrdersSql, [&](const core::AuxRow &row) {
      orders.push_back(OrderRow(row));
    });
    host.Query(kLinesSql, [&](const core::AuxRow &row) {
      lines.push_back(LineRow(row));
    });
  }

  template <typename Function, typename... Prefix>
  auto Call(Function function, Prefix... prefix) const {
    return function(prefix...,
                    suppliers.size(), suppliers.data(),
                    customers.size(), customers.data(),
                    nation_keys.size(), nation_keys.data(),
                    nation_names.bytes.data(), nation_names.off.data(),
                    orders.size(), orders.data(), lines.size(), lines.data());
  }
};

struct DeltaInputs {
  std::vector<tpchq7vgen_supplier_delta> suppliers;
  std::vector<tpchq7vgen_customer_delta> customers;
  std::vector<int64_t> nation_keys;
  kit::TextColumn nation_names;
  std::vector<int64_t> nation_signs;
  std::vector<tpchq7vgen_order_delta> orders;
  std::vector<tpchq7vgen_line_delta> lines;

  explicit DeltaInputs(const kit::Changes &changes) {
    suppliers.reserve(changes.at(0).size());
    customers.reserve(changes.at(1).size());
    nation_keys.reserve(changes.at(2).size());
    nation_names.off.reserve(changes.at(2).size() + 1);
    nation_signs.reserve(changes.at(2).size());
    orders.reserve(changes.at(3).size());
    lines.reserve(changes.at(4).size());
    for (const auto &change : changes.at(0)) {
      suppliers.push_back({SupplierRow(change.row), change.sign});
    }
    for (const auto &change : changes.at(1)) {
      customers.push_back({CustomerRow(change.row), change.sign});
    }
    for (const auto &change : changes.at(2)) {
      nation_keys.push_back(kit::I64(change.row, 0, 0));
      nation_names.Push(kit::Text(change.row, 1));
      nation_signs.push_back(change.sign);
    }
    for (const auto &change : changes.at(3)) {
      orders.push_back({kit::I64(change.row, 0, 0), change.sign});
    }
    for (const auto &change : changes.at(4)) {
      const auto &row = change.row;
      const int32_t shipdate = kit::Date(row, 4);
      lines.push_back({kit::I64(row, 0, 0), kit::I64(row, 1, 0),
                       kit::I64(row, 2, 2), kit::I64(row, 3, 2),
                       static_cast<uint8_t>(shipdate >= 9131 && shipdate <= 9495),
                       static_cast<uint8_t>(shipdate >= 9496 && shipdate <= 9861),
                       change.sign});
    }
  }

  using KeyFunction = decltype(&tpchq7vgen_rescan_relevant_orders_for_keys_keys);

  std::vector<int64_t> Keys(const tpchq7vgen_state *state,
                            KeyFunction function) const {
    std::vector<int64_t> keys(64);
    for (;;) {
      const size_t count = function(
          state, customers.size(), customers.data(), lines.size(), lines.data(),
          nation_keys.size(), nation_keys.data(), nation_names.bytes.data(),
          nation_names.off.data(), nation_signs.data(),
          orders.size(), orders.data(), suppliers.size(), suppliers.data(),
          keys.data(), keys.size());
      if (count <= keys.size()) {
        keys.resize(count);
        return keys;
      }
      keys.resize(count);
    }
  }
};

struct ApplyInputs {
  DeltaInputs deltas;
  std::vector<int64_t> order_keys;
  std::vector<tpchq7vgen_order_input> order_rows;
  std::vector<int64_t> line_order_keys;
  std::vector<tpchq7vgen_line_input> line_order_rows;
  std::vector<int64_t> line_supplier_keys;
  std::vector<tpchq7vgen_line_input> line_supplier_rows;

  ApplyInputs(const tpchq7vgen_state *state, const kit::Changes &changes,
              kit::Host &host)
      : deltas(changes) {
    order_keys = deltas.Keys(state, tpchq7vgen_rescan_relevant_orders_for_keys_keys);
    line_order_keys = deltas.Keys(state, tpchq7vgen_rescan_window_lines_for_orders_keys);
    line_supplier_keys = deltas.Keys(state, tpchq7vgen_rescan_window_lines_for_suppliers_keys);
    order_rows.reserve(order_keys.size());
    line_order_rows.reserve(line_order_keys.size());
    line_supplier_rows.reserve(line_supplier_keys.size());
    kit::QueryKeys(host, kOrdersSql, "o_orderkey", order_keys,
                   [&](const core::AuxRow &row) {
      order_rows.push_back(OrderRow(row));
    });
    kit::QueryKeys(host, kLinesSql, "l_orderkey", line_order_keys,
                   [&](const core::AuxRow &row) {
      line_order_rows.push_back(LineRow(row));
    });
    kit::QueryKeys(host, kLinesSql, "l_suppkey", line_supplier_keys,
                   [&](const core::AuxRow &row) {
      line_supplier_rows.push_back(LineRow(row));
    });
  }

  template <typename Function, typename... Prefix>
  auto Call(Function function, int32_t *err, Prefix... prefix) const {
    return function(
        prefix...,
        deltas.customers.size(), deltas.customers.data(),
        deltas.lines.size(), deltas.lines.data(),
        deltas.nation_keys.size(), deltas.nation_keys.data(),
        deltas.nation_names.bytes.data(), deltas.nation_names.off.data(),
        deltas.nation_signs.data(),
        deltas.orders.size(), deltas.orders.data(),
        deltas.suppliers.size(), deltas.suppliers.data(),
        order_keys.size(), order_keys.data(), order_rows.size(), order_rows.data(),
        line_order_keys.size(), line_order_keys.data(),
        line_order_rows.size(), line_order_rows.data(),
        line_supplier_keys.size(), line_supplier_keys.data(),
        line_supplier_rows.size(), line_supplier_rows.data(), err);
  }
};

void *Build(kit::Host &host) {
  return BuildInputs(host).Call(tpchq7vgen_build);
}

void FreeState(void *state) {
  tpchq7vgen_free(static_cast<tpchq7vgen_state *>(state));
}

void *PrepareBuild(const kit::HeadFn &head_of, kit::Host &host) {
  return BuildInputs(host).Call(tpchq7vgens_stage_build,
                               static_cast<const tpchq7vgens_head *>(head_of()));
}

void *Apply(const void *state, const kit::Changes &changes, kit::Host &host,
            int32_t *err) {
  const auto *typed_state = static_cast<const tpchq7vgen_state *>(state);
  return ApplyInputs(typed_state, changes, host).Call(tpchq7vgen_apply, err,
                                                    typed_state);
}

void *PrepareApply(const void *base,
                   const kit::Changes &changes, kit::Host &host, int32_t *err) {
  const auto *typed_base = static_cast<const tpchq7vgens_base *>(base);
  const auto *state = tpchq7vgens_base_state(typed_base);
  return ApplyInputs(state, changes, host).Call(tpchq7vgens_stage_apply, err,
                                               typed_base);
}

template <class Call>
bool Run(const kit::core::QueryArguments &args, std::vector<kit::core::AuxRow> &out, Call call) {
  std::string nation1;
  std::string nation2;
  if (!kit::ArgText(args, 0, nation1) || !kit::ArgText(args, 1, nation2)) {
    return false;
  }
  std::unique_ptr<tpchq7vgen_result, decltype(&tpchq7vgen_result_free)> result(
      call(reinterpret_cast<const uint8_t *>(nation1.data()),
                         nation1.size(),
                         reinterpret_cast<const uint8_t *>(nation2.data()),
                         nation2.size()),
      tpchq7vgen_result_free);
  if (!result) {
    return false;
  }
  const size_t count = tpchq7vgen_result_len(result.get());
  out.reserve(out.size() + count);
  for (size_t index = 0; index < count; ++index) {
    core::AuxRow row;
    row.values.reserve(4);
    const uint8_t *text = nullptr;
    size_t length = 0;
    if (tpchq7vgen_result_c0(result.get(), index, &text, &length)) {
      row.values.push_back(kit::OutText(text, length));
    } else {
      row.values.push_back(kit::OutNull(core::AuxType::String));
    }
    if (tpchq7vgen_result_c1(result.get(), index, &text, &length)) {
      row.values.push_back(kit::OutText(text, length));
    } else {
      row.values.push_back(kit::OutNull(core::AuxType::String));
    }
    int64_t year = 0;
    if (tpchq7vgen_result_c2(result.get(), index, &year)) {
      row.values.push_back(kit::OutI64(year));
    } else {
      row.values.push_back(kit::OutNull(core::AuxType::Int64));
    }
    int64_t hi = 0;
    uint64_t lo = 0;
    if (tpchq7vgen_result_c3(result.get(), index, &hi, &lo)) {
      row.values.push_back(kit::OutDec(hi, lo, 4));
    } else {
      row.values.push_back(kit::OutNull(core::AuxType::ScaledDecimal));
    }
    out.push_back(std::move(row));
  }
  return true;
}

bool Execute(const void *state, const kit::core::QueryArguments &args,
             std::vector<kit::core::AuxRow> &out) {
  return Run(args, out, [&](auto... x) {
    return tpchq7vgen_execute(static_cast<const tpchq7vgen_state *>(state), x...);
  });
}

bool Answer(const void *state, const kit::core::QueryArguments &args,
            std::vector<kit::core::AuxRow> &out) {
  return Run(args, out, [&](auto... x) {
    return tpchq7vgens_answer(static_cast<const tpchq7vgens_reading *>(state), x...);
  });
}

const kit::Glue kGlue = {
    "tpch_q7_vgen", "tpch", 7, kQuerySql,
    {{"supplier", {"s_suppkey", "s_nationkey"}},
     {"customer", {"c_custkey", "c_nationkey"}},
     {"nation", {"n_nationkey", "n_name"}},
     {"orders", {"o_orderkey"}},
     {"lineitem", {"l_orderkey", "l_suppkey", "l_extendedprice", "l_discount",
                   "l_shipdate"}}},
    {kit::Kind::Text, kit::Kind::Text},
    Build, FreeState, Apply, Execute, Answer, PrepareApply, PrepareBuild,
    kit::StoreOps{
        []() -> void * { return tpchq7vgens_new(); },
        [](void *store) { tpchq7vgens_free(static_cast<tpchq7vgens_store *>(store)); },
        [](const void *store) -> void * { return tpchq7vgens_newest_base(static_cast<const tpchq7vgens_store *>(store)); },
        [](const void *base) -> const void * { return tpchq7vgens_base_state(static_cast<const tpchq7vgens_base *>(base)); },
        [](void *base) { tpchq7vgens_base_free(static_cast<tpchq7vgens_base *>(base)); },
        [](const void *store) -> void * { return tpchq7vgens_newest_head(static_cast<const tpchq7vgens_store *>(store)); },
        [](void *head) { tpchq7vgens_head_free(static_cast<tpchq7vgens_head *>(head)); },
        [](const void *head) -> void * { return tpchq7vgens_stage_invalid(static_cast<const tpchq7vgens_head *>(head)); },
        [](void *staged) { tpchq7vgens_staged_free(static_cast<tpchq7vgens_staged *>(staged)); },
        [](const void *store, uint64_t from) -> int32_t { return tpchq7vgens_begin(static_cast<const tpchq7vgens_store *>(store), from); },
        [](const void *store) { tpchq7vgens_abort(static_cast<const tpchq7vgens_store *>(store)); },
        [](const void *store, void *staged, uint64_t from, uint64_t floor, int32_t end_commit) -> int32_t {
            return tpchq7vgens_install(static_cast<const tpchq7vgens_store *>(store), static_cast<tpchq7vgens_staged *>(staged), from, floor, end_commit);
        },
        [](const void *store, uint64_t at, uint64_t tries, int32_t *status) -> const void * {
            return tpchq7vgens_read(static_cast<const tpchq7vgens_store *>(store), at, tries, status);
        },
        [](const void *reading) { tpchq7vgens_reading_free(const_cast<tpchq7vgens_reading *>(static_cast<const tpchq7vgens_reading *>(reading))); },
        [](const void *store) { tpchq7vgens_invalidate_all(static_cast<const tpchq7vgens_store *>(store)); },
        [](const void *store) -> std::size_t { return tpchq7vgens_version_count(static_cast<const tpchq7vgens_store *>(store)); },
        []() -> std::size_t { return tpchq7vgens_heap_bytes(); },
        []() -> std::size_t { return tpchq7vgens_heap_peak_bytes(); },
        []() { tpchq7vgens_heap_reset_peak(); },
        [](const void *head, void *state) -> void * { return tpchq7vgens_stage_adopt(static_cast<const tpchq7vgens_head *>(head), static_cast<tpchq7vgen_state *>(state)); },
    }};

const kit::Registrar kRegistrar(&kGlue);

}
