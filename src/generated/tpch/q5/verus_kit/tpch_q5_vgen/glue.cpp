#include "tpch_q5_vgen_verus.h"
#include "accelerator_codegen/verus/verus_kit.hpp"

#include <memory>
#include <utility>
#include <vector>

namespace {

namespace kit = accelerator_codegen::verus_kit;
namespace core = kit::core;

const char kQuerySql[] = R"SQL(-- TPC-H Q5 (catalog text), :region_name as $1, :start_date as $2, $3 = $2 + 1 year (derived end date: sql2verus cannot add months to a non-literal date).
SELECT n_name, SUM(l_extendedprice * (1 - l_discount)) AS revenue
FROM customer, orders, lineitem, supplier, nation, region
WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey
  AND l_suppkey = s_suppkey AND c_nationkey = s_nationkey
  AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey
  AND r_name = $1
  AND o_orderdate >= $2
  AND o_orderdate < $3
GROUP BY n_name
ORDER BY revenue DESC
)SQL";

const char kCustomersSql[] = "SELECT c_custkey, c_nationkey FROM customer;";
const char kOrdersSql[] = "SELECT o_orderkey, o_custkey, o_orderdate FROM orders;";
const char kSuppliersSql[] = "SELECT s_suppkey, s_nationkey FROM supplier;";
const char kNationsSql[] = "SELECT n_nationkey, n_name, n_regionkey FROM nation;";
const char kRegionsSql[] = "SELECT r_regionkey, r_name FROM region;";
const char kLinesSql[] = R"SQL(SELECT l_orderkey, l_suppkey, l_extendedprice, l_discount
  FROM lineitem l
  WHERE EXISTS (
    SELECT 1 FROM orders o
    WHERE o.o_orderkey = l.l_orderkey
      AND EXISTS (
        SELECT 1 FROM customer c
        WHERE c.c_custkey = o.o_custkey
          AND EXISTS (
            SELECT 1 FROM supplier s
            WHERE s.s_suppkey = l.l_suppkey
              AND s.s_nationkey = c.c_nationkey
          )
      )
  );)SQL";
const char kLinesForOrdersSql[] =
    "SELECT l_orderkey, l_suppkey, l_extendedprice, l_discount FROM lineitem;";
const char kLinesForSuppliersSql[] =
    "SELECT l_orderkey, l_suppkey, l_extendedprice, l_discount FROM lineitem;";

tpchq5vgen_customer_input Customer(const core::AuxRow &row) {
  return {kit::I64(row, 0, 0), kit::I64(row, 1, 0)};
}

tpchq5vgen_orders_input Order(const core::AuxRow &row) {
  return {kit::I64(row, 0, 0), kit::I64(row, 1, 0), kit::Date(row, 2)};
}

tpchq5vgen_supplier_input Supplier(const core::AuxRow &row) {
  return {kit::I64(row, 0, 0), kit::I64(row, 1, 0)};
}

tpchq5vgen_lineitem_input Line(const core::AuxRow &row) {
  return {kit::I64(row, 0, 0), kit::I64(row, 1, 0),
          kit::I64(row, 2, 2), kit::I64(row, 3, 2)};
}

struct Nations {
  std::vector<int64_t> keys;
  kit::TextColumn names;
  std::vector<int64_t> region_keys;

  void Reserve(std::size_t count) {
    keys.reserve(count);
    names.off.reserve(count + 1);
    region_keys.reserve(count);
  }

  void Push(const core::AuxRow &row) {
    keys.push_back(kit::I64(row, 0, 0));
    names.Push(kit::Text(row, 1));
    region_keys.push_back(kit::I64(row, 2, 0));
  }
};

struct Regions {
  std::vector<int64_t> keys;
  kit::TextColumn names;

  void Reserve(std::size_t count) {
    keys.reserve(count);
    names.off.reserve(count + 1);
  }

  void Push(const core::AuxRow &row) {
    keys.push_back(kit::I64(row, 0, 0));
    names.Push(kit::Text(row, 1));
  }
};

struct BuildInputs {
  std::vector<tpchq5vgen_customer_input> customers;
  std::vector<tpchq5vgen_orders_input> orders;
  std::vector<tpchq5vgen_supplier_input> suppliers;
  Nations nations;
  Regions regions;
  std::vector<tpchq5vgen_lineitem_input> lines;

  // The row count of a table, only to size the buffers once (a wrong count
  // is harmless: the vectors still grow).
  static std::size_t CountRows(kit::Host &host, const char *sql) {
    std::size_t count = 0;
    host.Query(sql, [&](const core::AuxRow &row) {
      const int64_t value = kit::I64(row, 0, 0);
      count = value > 0 ? static_cast<std::size_t>(value) : 0;
    });
    return count;
  }

  explicit BuildInputs(kit::Host &host) {
    customers.reserve(CountRows(host, "SELECT count(*) FROM customer;"));
    orders.reserve(CountRows(host, "SELECT count(*) FROM orders;"));
    suppliers.reserve(CountRows(host, "SELECT count(*) FROM supplier;"));
    host.Query(kCustomersSql, [&](const core::AuxRow &row) {
      customers.push_back(Customer(row));
    });
    host.Query(kOrdersSql, [&](const core::AuxRow &row) {
      orders.push_back(Order(row));
    });
    host.Query(kSuppliersSql, [&](const core::AuxRow &row) {
      suppliers.push_back(Supplier(row));
    });
    host.Query(kNationsSql, [&](const core::AuxRow &row) { nations.Push(row); });
    host.Query(kRegionsSql, [&](const core::AuxRow &row) { regions.Push(row); });
    host.Query(kLinesSql, [&](const core::AuxRow &row) {
      lines.push_back(Line(row));
    });
  }
};

void *Build(kit::Host &host) {
  const BuildInputs inputs(host);
  return tpchq5vgen_build(
      inputs.customers.size(), inputs.customers.data(),
      inputs.orders.size(), inputs.orders.data(),
      inputs.suppliers.size(), inputs.suppliers.data(),
      inputs.nations.keys.size(), inputs.nations.keys.data(),
      inputs.nations.names.bytes.data(), inputs.nations.names.off.data(),
      inputs.nations.region_keys.data(),
      inputs.regions.keys.size(), inputs.regions.keys.data(),
      inputs.regions.names.bytes.data(), inputs.regions.names.off.data(),
      inputs.lines.size(), inputs.lines.data());
}

void *PrepareBuild(const void *store, kit::Host &host) {
  const BuildInputs inputs(host);
  return tpchq5vgens_prepare_build(
      static_cast<const tpchq5vgens_store *>(store),
      inputs.customers.size(), inputs.customers.data(),
      inputs.orders.size(), inputs.orders.data(),
      inputs.suppliers.size(), inputs.suppliers.data(),
      inputs.nations.keys.size(), inputs.nations.keys.data(),
      inputs.nations.names.bytes.data(), inputs.nations.names.off.data(),
      inputs.nations.region_keys.data(),
      inputs.regions.keys.size(), inputs.regions.keys.data(),
      inputs.regions.names.bytes.data(), inputs.regions.names.off.data(),
      inputs.lines.size(), inputs.lines.data());
}

void FreeState(void *state) {
  tpchq5vgen_free(static_cast<tpchq5vgen_state *>(state));
}

struct ApplyInputs {
  std::vector<tpchq5vgen_customer_delta> customers;
  std::vector<tpchq5vgen_orders_delta> orders;
  std::vector<tpchq5vgen_lineitem_delta> lines;
  std::vector<tpchq5vgen_supplier_delta> suppliers;
  Nations nations;
  std::vector<int64_t> nation_signs;
  Regions regions;
  std::vector<int64_t> region_signs;
  std::vector<int64_t> order_keys;
  std::vector<tpchq5vgen_lineitem_input> order_rows;
  std::vector<int64_t> supplier_keys;
  std::vector<tpchq5vgen_lineitem_input> supplier_rows;

  explicit ApplyInputs(const kit::Changes &changes) {
    customers.reserve(changes.at(0).size());
    for (const auto &change : changes.at(0)) {
      customers.push_back({Customer(change.row), change.sign});
    }
    orders.reserve(changes.at(1).size());
    for (const auto &change : changes.at(1)) {
      orders.push_back({Order(change.row), change.sign});
    }
    lines.reserve(changes.at(2).size());
    for (const auto &change : changes.at(2)) {
      lines.push_back({Line(change.row), change.sign});
    }
    suppliers.reserve(changes.at(3).size());
    for (const auto &change : changes.at(3)) {
      suppliers.push_back({Supplier(change.row), change.sign});
    }
    nations.Reserve(changes.at(4).size());
    nation_signs.reserve(changes.at(4).size());
    for (const auto &change : changes.at(4)) {
      nations.Push(change.row);
      nation_signs.push_back(change.sign);
    }
    regions.Reserve(changes.at(5).size());
    region_signs.reserve(changes.at(5).size());
    for (const auto &change : changes.at(5)) {
      regions.Push(change.row);
      region_signs.push_back(change.sign);
    }
  }

  void Keys(const tpchq5vgen_state *state,
            decltype(&tpchq5vgen_rescan_lines_for_orders_keys) rescan,
            std::vector<int64_t> &keys) const {
    for (;;) {
      const std::size_t count = rescan(
          state, customers.size(), customers.data(), lines.size(), lines.data(),
          nations.keys.size(), nations.keys.data(), nations.names.bytes.data(),
          nations.names.off.data(), nations.region_keys.data(), nation_signs.data(),
          orders.size(), orders.data(),
          regions.keys.size(), regions.keys.data(), regions.names.bytes.data(),
          regions.names.off.data(), region_signs.data(),
          suppliers.size(), suppliers.data(), keys.data(), keys.size());
      if (count <= keys.size()) {
        keys.resize(count);
        return;
      }
      keys.resize(count);
    }
  }

  void Rescan(const tpchq5vgen_state *state, kit::Host &host) {
    Keys(state, tpchq5vgen_rescan_lines_for_orders_keys, order_keys);
    Keys(state, tpchq5vgen_rescan_lines_for_suppliers_keys, supplier_keys);
    order_rows.reserve(order_keys.size());
    supplier_rows.reserve(supplier_keys.size());
    kit::QueryKeys(host, kLinesForOrdersSql, "l_orderkey", order_keys,
                   [&](const core::AuxRow &row) { order_rows.push_back(Line(row)); });
    kit::QueryKeys(host, kLinesForSuppliersSql, "l_suppkey", supplier_keys,
                   [&](const core::AuxRow &row) { supplier_rows.push_back(Line(row)); });
  }
};

void *Apply(const void *state, const kit::Changes &changes, kit::Host &host,
            int32_t *err) {
  const auto *kernel_state = static_cast<const tpchq5vgen_state *>(state);
  ApplyInputs inputs(changes);
  inputs.Rescan(kernel_state, host);
  return tpchq5vgen_apply(
      kernel_state,
      inputs.customers.size(), inputs.customers.data(),
      inputs.lines.size(), inputs.lines.data(),
      inputs.nations.keys.size(), inputs.nations.keys.data(),
      inputs.nations.names.bytes.data(), inputs.nations.names.off.data(),
      inputs.nations.region_keys.data(), inputs.nation_signs.data(),
      inputs.orders.size(), inputs.orders.data(),
      inputs.regions.keys.size(), inputs.regions.keys.data(),
      inputs.regions.names.bytes.data(), inputs.regions.names.off.data(),
      inputs.region_signs.data(), inputs.suppliers.size(), inputs.suppliers.data(),
      inputs.order_keys.size(), inputs.order_keys.data(),
      inputs.order_rows.size(), inputs.order_rows.data(),
      inputs.supplier_keys.size(), inputs.supplier_keys.data(),
      inputs.supplier_rows.size(), inputs.supplier_rows.data(), err);
}

void *PrepareApply(const void *store, const void *base,
                   const kit::Changes &changes, kit::Host &host, int32_t *err) {
  const auto *kernel_base = static_cast<const tpchq5vgens_base *>(base);
  ApplyInputs inputs(changes);
  inputs.Rescan(tpchq5vgens_base_state(kernel_base), host);
  return tpchq5vgens_prepare_apply(
      static_cast<const tpchq5vgens_store *>(store), kernel_base,
      inputs.customers.size(), inputs.customers.data(),
      inputs.lines.size(), inputs.lines.data(),
      inputs.nations.keys.size(), inputs.nations.keys.data(),
      inputs.nations.names.bytes.data(), inputs.nations.names.off.data(),
      inputs.nations.region_keys.data(), inputs.nation_signs.data(),
      inputs.orders.size(), inputs.orders.data(),
      inputs.regions.keys.size(), inputs.regions.keys.data(),
      inputs.regions.names.bytes.data(), inputs.regions.names.off.data(),
      inputs.region_signs.data(), inputs.suppliers.size(), inputs.suppliers.data(),
      inputs.order_keys.size(), inputs.order_keys.data(),
      inputs.order_rows.size(), inputs.order_rows.data(),
      inputs.supplier_keys.size(), inputs.supplier_keys.data(),
      inputs.supplier_rows.size(), inputs.supplier_rows.data(), err);
}

bool Execute(const void *state, const core::QueryArguments &args,
             std::vector<core::AuxRow> &out) {
  std::string region_name;
  int32_t start_date;
  int32_t end_date;
  if (!kit::ArgText(args, 0, region_name) ||
      !kit::ArgDate(args, 1, start_date) || !kit::ArgDate(args, 2, end_date)) {
    return false;
  }
  std::unique_ptr<tpchq5vgen_result, decltype(&tpchq5vgen_result_free)> result(
      tpchq5vgen_execute(static_cast<const tpchq5vgen_state *>(state),
                         reinterpret_cast<const uint8_t *>(region_name.data()),
                         region_name.size(), start_date, end_date),
      tpchq5vgen_result_free);
  if (!result) {
    return false;
  }
  const std::size_t count = tpchq5vgen_result_len(result.get());
  out.reserve(out.size() + count);
  for (std::size_t index = 0; index < count; ++index) {
    core::AuxRow row;
    row.values.reserve(2);
    const uint8_t *text = nullptr;
    std::size_t text_len = 0;
    if (tpchq5vgen_result_c0(result.get(), index, &text, &text_len)) {
      row.values.push_back(kit::OutText(text, text_len));
    } else {
      row.values.push_back(kit::OutNull(core::AuxType::String));
    }
    int64_t hi = 0;
    uint64_t lo = 0;
    if (tpchq5vgen_result_c1(result.get(), index, &hi, &lo)) {
      row.values.push_back(kit::OutDec(hi, lo, 4));
    } else {
      row.values.push_back(kit::OutNull(core::AuxType::ScaledDecimal));
    }
    out.push_back(std::move(row));
  }
  return true;
}

const kit::Glue kGlue = {
    "tpch_q5_vgen", "tpch", 5, kQuerySql,
    {{"customer", {"c_custkey", "c_nationkey"}},
     {"orders", {"o_orderkey", "o_custkey", "o_orderdate"}},
     {"lineitem", {"l_orderkey", "l_suppkey", "l_extendedprice", "l_discount"}},
     {"supplier", {"s_suppkey", "s_nationkey"}},
     {"nation", {"n_nationkey", "n_name", "n_regionkey"}},
     {"region", {"r_regionkey", "r_name"}}},
    {kit::Kind::Text, kit::Kind::Date, kit::Kind::Date},
    Build, FreeState, Apply, Execute, PrepareApply, PrepareBuild,
    kit::StoreOps{
        []() -> void * { return tpchq5vgens_new(); },
        [](void *store) {
          tpchq5vgens_free(static_cast<tpchq5vgens_store *>(store));
        },
        [](const void *store) -> void * {
          return tpchq5vgens_newest_base(static_cast<const tpchq5vgens_store *>(store));
        },
        [](const void *base) -> const void * {
          return tpchq5vgens_base_state(static_cast<const tpchq5vgens_base *>(base));
        },
        [](void *base) {
          tpchq5vgens_base_free(static_cast<tpchq5vgens_base *>(base));
        },
        [](const void *store) -> void * {
          return tpchq5vgens_prepare_invalid(static_cast<const tpchq5vgens_store *>(store));
        },
        [](void *prepared) {
          tpchq5vgens_prepared_free(static_cast<tpchq5vgens_prepared *>(prepared));
        },
        [](const void *store, uint64_t from) -> int32_t {
          return tpchq5vgens_begin(static_cast<const tpchq5vgens_store *>(store), from);
        },
        [](const void *store) {
          tpchq5vgens_abort(static_cast<const tpchq5vgens_store *>(store));
        },
        [](const void *store, void *prepared, uint64_t from, uint64_t floor,
           int32_t end_commit) -> int32_t {
          return tpchq5vgens_install(static_cast<const tpchq5vgens_store *>(store),
                                    static_cast<tpchq5vgens_prepared *>(prepared),
                                    from, floor, end_commit);
        },
        [](const void *store, uint64_t at, uint64_t tries,
           int32_t *status) -> const void * {
          return tpchq5vgens_read(static_cast<const tpchq5vgens_store *>(store),
                                 at, tries, status);
        },
        [](const void *state) {
          tpchq5vgens_state_release(static_cast<const tpchq5vgen_state *>(state));
        },
        [](const void *store) {
          tpchq5vgens_invalidate_all(static_cast<const tpchq5vgens_store *>(store));
        },
        [](const void *store) -> std::size_t {
          return tpchq5vgens_version_count(static_cast<const tpchq5vgens_store *>(store));
        }}};

const kit::Registrar kRegistrar(&kGlue);

}
