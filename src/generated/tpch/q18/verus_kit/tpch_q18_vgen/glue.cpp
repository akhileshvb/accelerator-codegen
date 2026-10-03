#include "tpch_q18_vgen_verus.h"
#include "accelerator_codegen/verus/verus_kit.hpp"

#include <memory>
#include <utility>
#include <vector>

namespace {

namespace kit = accelerator_codegen::verus_kit;
namespace core = accelerator_codegen::aux_state::core;

constexpr char kQuerySql[] = R"SQL(-- TPC-H Q18 (catalog text), :quantity as $1 (no LIMIT, as in the catalog).
SELECT c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice, SUM(l_quantity) AS sum_qty
FROM customer, orders, lineitem
WHERE o_orderkey IN (SELECT l_orderkey FROM lineitem GROUP BY l_orderkey HAVING SUM(l_quantity) > $1)
  AND c_custkey = o_custkey
  AND o_orderkey = l_orderkey
GROUP BY c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice
ORDER BY o_totalprice DESC, o_orderdate
)SQL";

struct ScanInputs {
  std::vector<tpchq18vgen_line_quantity_scan> line_quantities;
  std::vector<tpchq18vgen_order_group_scan> order_groups;
  std::vector<int64_t> customer_keys;
  kit::TextColumn customer_names;

  explicit ScanInputs(kit::Host &host) {
    host.Query("SELECT l_orderkey, l_quantity FROM lineitem;",
               [&](const core::AuxRow &row) {
      line_quantities.push_back({kit::I64(row, 0, 0), kit::I64(row, 1, 2)});
    });
    host.Query("SELECT o_orderkey, o_custkey, o_orderdate, o_totalprice FROM orders;",
               [&](const core::AuxRow &row) {
      order_groups.push_back({kit::I64(row, 0, 0), kit::I64(row, 1, 0),
                              kit::Date(row, 2), kit::I64(row, 3, 2)});
    });
    host.Query("SELECT c_custkey, c_name FROM customer;",
               [&](const core::AuxRow &row) {
      customer_keys.push_back(kit::I64(row, 0, 0));
      customer_names.Push(kit::Text(row, 1));
    });
  }
};

struct DeltaInputs {
  std::vector<tpchq18vgen_line_quantity_delta> lineitems;
  std::vector<tpchq18vgen_order_group_delta> orders;
  std::vector<int64_t> customer_keys;
  kit::TextColumn customer_names;
  std::vector<int64_t> customer_signs;

  explicit DeltaInputs(const kit::Changes &changes) {
    lineitems.reserve(changes.at(0).size());
    for (const auto &change : changes.at(0)) {
      lineitems.push_back({{kit::I64(change.row, 0, 0),
                            kit::I64(change.row, 1, 2)}, change.sign});
    }
    orders.reserve(changes.at(1).size());
    for (const auto &change : changes.at(1)) {
      orders.push_back({{kit::I64(change.row, 0, 0),
                         kit::I64(change.row, 1, 0),
                         kit::Date(change.row, 2),
                         kit::I64(change.row, 3, 2)}, change.sign});
    }
    const auto &customers = changes.at(2);
    customer_keys.reserve(customers.size());
    customer_names.off.reserve(customers.size() + 1);
    customer_signs.reserve(customers.size());
    for (const auto &change : customers) {
      customer_keys.push_back(kit::I64(change.row, 0, 0));
      customer_names.Push(kit::Text(change.row, 1));
      customer_signs.push_back(change.sign);
    }
  }
};

void *Build(kit::Host &host) {
  const ScanInputs inputs(host);
  return tpchq18vgen_build(
      inputs.line_quantities.size(), inputs.line_quantities.data(),
      inputs.order_groups.size(), inputs.order_groups.data(),
      inputs.customer_keys.size(), inputs.customer_keys.data(),
      inputs.customer_names.bytes.data(), inputs.customer_names.off.data());
}

void FreeState(void *state) {
  tpchq18vgen_free(static_cast<tpchq18vgen_state *>(state));
}

void *Apply(const void *state, const kit::Changes &changes, kit::Host &,
            int32_t *err) {
  const DeltaInputs inputs(changes);
  return tpchq18vgen_apply(
      static_cast<const tpchq18vgen_state *>(state),
      inputs.customer_keys.size(), inputs.customer_keys.data(),
      inputs.customer_names.bytes.data(), inputs.customer_names.off.data(),
      inputs.customer_signs.data(), inputs.lineitems.size(), inputs.lineitems.data(),
      inputs.orders.size(), inputs.orders.data(), err);
}

void *PrepareBuild(const void *store, kit::Host &host) {
  const ScanInputs inputs(host);
  return tpchq18vgens_prepare_build(
      static_cast<const tpchq18vgens_store *>(store),
      inputs.line_quantities.size(), inputs.line_quantities.data(),
      inputs.order_groups.size(), inputs.order_groups.data(),
      inputs.customer_keys.size(), inputs.customer_keys.data(),
      inputs.customer_names.bytes.data(), inputs.customer_names.off.data());
}

void *PrepareApply(const void *store, const void *base,
                   const kit::Changes &changes, kit::Host &, int32_t *err) {
  const DeltaInputs inputs(changes);
  return tpchq18vgens_prepare_apply(
      static_cast<const tpchq18vgens_store *>(store),
      static_cast<const tpchq18vgens_base *>(base),
      inputs.customer_keys.size(), inputs.customer_keys.data(),
      inputs.customer_names.bytes.data(), inputs.customer_names.off.data(),
      inputs.customer_signs.data(), inputs.lineitems.size(), inputs.lineitems.data(),
      inputs.orders.size(), inputs.orders.data(), err);
}

bool Execute(const void *state, const core::QueryArguments &args,
             std::vector<core::AuxRow> &out) {
  int64_t threshold;
  if (!kit::ArgI64(args, 0, 2, threshold)) {
    return false;
  }
  std::unique_ptr<tpchq18vgen_result, decltype(&tpchq18vgen_result_free)> result(
      tpchq18vgen_execute(static_cast<const tpchq18vgen_state *>(state), threshold),
      &tpchq18vgen_result_free);
  if (!result) {
    return false;
  }
  const size_t count = tpchq18vgen_result_len(result.get());
  out.clear();
  out.reserve(count);
  for (size_t index = 0; index < count; ++index) {
    core::AuxRow row;
    row.values.reserve(6);
    const uint8_t *text;
    size_t text_length;
    int64_t integer;
    int32_t date;
    int64_t decimal_hi;
    uint64_t decimal_lo;
    row.values.push_back(tpchq18vgen_result_c0(result.get(), index, &text, &text_length)
                             ? kit::OutText(text, text_length)
                             : kit::OutNull(core::AuxType::String));
    row.values.push_back(tpchq18vgen_result_c1(result.get(), index, &integer)
                             ? kit::OutI64(integer)
                             : kit::OutNull(core::AuxType::Int64));
    row.values.push_back(tpchq18vgen_result_c2(result.get(), index, &integer)
                             ? kit::OutI64(integer)
                             : kit::OutNull(core::AuxType::Int64));
    row.values.push_back(tpchq18vgen_result_c3(result.get(), index, &date)
                             ? kit::OutDate(date)
                             : kit::OutNull(core::AuxType::Date));
    row.values.push_back(tpchq18vgen_result_c4(result.get(), index, &decimal_hi, &decimal_lo)
                             ? kit::OutDec(decimal_hi, decimal_lo, 2)
                             : kit::OutNull(core::AuxType::ScaledDecimal));
    row.values.push_back(tpchq18vgen_result_c5(result.get(), index, &decimal_hi, &decimal_lo)
                             ? kit::OutDec(decimal_hi, decimal_lo, 2)
                             : kit::OutNull(core::AuxType::ScaledDecimal));
    out.push_back(std::move(row));
  }
  return true;
}

const kit::Glue kGlue = {
    "tpch_q18_vgen", "tpch", 18, kQuerySql,
    {{"lineitem", {"l_orderkey", "l_quantity"}},
     {"orders", {"o_orderkey", "o_custkey", "o_orderdate", "o_totalprice"}},
     {"customer", {"c_custkey", "c_name"}}},
    {kit::Kind::Dec},
    Build, FreeState, Apply, Execute, PrepareApply, PrepareBuild,
    kit::StoreOps{
        []() -> void * { return tpchq18vgens_new(); },
        [](void *store) { tpchq18vgens_free(static_cast<tpchq18vgens_store *>(store)); },
        [](const void *store) -> void * {
          return tpchq18vgens_newest_base(static_cast<const tpchq18vgens_store *>(store));
        },
        [](const void *base) -> const void * {
          return tpchq18vgens_base_state(static_cast<const tpchq18vgens_base *>(base));
        },
        [](void *base) { tpchq18vgens_base_free(static_cast<tpchq18vgens_base *>(base)); },
        [](const void *store) -> void * {
          return tpchq18vgens_prepare_invalid(static_cast<const tpchq18vgens_store *>(store));
        },
        [](void *prepared) {
          tpchq18vgens_prepared_free(static_cast<tpchq18vgens_prepared *>(prepared));
        },
        [](const void *store, uint64_t from) -> int32_t {
          return tpchq18vgens_begin(static_cast<const tpchq18vgens_store *>(store), from);
        },
        [](const void *store) {
          tpchq18vgens_abort(static_cast<const tpchq18vgens_store *>(store));
        },
        [](const void *store, void *prepared, uint64_t from, uint64_t floor,
           int32_t end_commit) -> int32_t {
          return tpchq18vgens_install(static_cast<const tpchq18vgens_store *>(store),
                                    static_cast<tpchq18vgens_prepared *>(prepared),
                                    from, floor, end_commit);
        },
        [](const void *store, uint64_t at, uint64_t tries, int32_t *status) -> const void * {
          return tpchq18vgens_read(static_cast<const tpchq18vgens_store *>(store),
                                 at, tries, status);
        },
        [](const void *state) {
          tpchq18vgens_state_release(static_cast<const tpchq18vgen_state *>(state));
        },
        [](const void *store) {
          tpchq18vgens_invalidate_all(static_cast<const tpchq18vgens_store *>(store));
        },
        [](const void *store) -> size_t {
          return tpchq18vgens_version_count(static_cast<const tpchq18vgens_store *>(store));
        },
    },
};

const kit::Registrar kRegistrar(&kGlue);

}
