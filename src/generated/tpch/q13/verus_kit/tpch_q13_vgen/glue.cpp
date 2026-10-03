#include "tpch_q13_vgen_verus.h"
#include "accelerator_codegen/verus/verus_kit.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace kit = accelerator_codegen::verus_kit;
namespace core = kit::core;

const char kQuerySql[] = R"SQL(-- TPC-H Q13 (catalog text). No parameters: sql2verus takes only a literal LIKE pattern.
SELECT c_count, COUNT(*) AS custdist
FROM (SELECT c_custkey, COUNT(o_orderkey) AS c_count
      FROM customer LEFT OUTER JOIN orders
        ON c_custkey = o_custkey AND o_comment NOT LIKE '%unusual%accounts%'
      GROUP BY c_custkey) AS c_orders
GROUP BY c_count
ORDER BY custdist DESC, c_count DESC
)SQL";

const char kCustomerScanSql[] = "SELECT c_custkey FROM customer;";
const char kOrderScanSql[] = R"SQL(SELECT o_custkey FROM orders
  WHERE o_comment NOT LIKE '%unusual%accounts%';)SQL";

struct ScanInputs {
  std::vector<tpchq13vgen_customer_scan> customers;
  std::vector<tpchq13vgen_qualifying_order_scan> qualifying_orders;

  explicit ScanInputs(kit::Host &host) {
    host.Query(kCustomerScanSql, [&](const core::AuxRow &row) {
      customers.push_back({kit::I64(row, 0, 0)});
    });
    host.Query(kOrderScanSql, [&](const core::AuxRow &row) {
      qualifying_orders.push_back({kit::I64(row, 0, 0)});
    });
  }
};

uint8_t Qualifies(const std::string &comment) {
  const std::size_t unusual = comment.find("unusual");
  if (unusual == std::string::npos) {
    return 1;
  }
  return comment.find("accounts", unusual + 7) == std::string::npos ? 1 : 0;
}

struct DeltaInputs {
  std::vector<tpchq13vgen_customer_delta> customers;
  std::vector<tpchq13vgen_order_delta> orders;

  explicit DeltaInputs(const kit::Changes &changes) {
    const auto &customer_changes = changes.at(0);
    const auto &order_changes = changes.at(1);
    customers.reserve(customer_changes.size());
    orders.reserve(order_changes.size());
    for (const auto &change : customer_changes) {
      customers.push_back({{kit::I64(change.row, 0, 0)}, change.sign});
    }
    for (const auto &change : order_changes) {
      const int64_t custkey = kit::I64(change.row, 0, 0);
      const std::string &comment = kit::Text(change.row, 1);
      orders.push_back({custkey, Qualifies(comment), change.sign});
    }
  }
};

void *Build(kit::Host &host) {
  const ScanInputs inputs(host);
  return tpchq13vgen_build(inputs.customers.size(), inputs.customers.data(),
                          inputs.qualifying_orders.size(), inputs.qualifying_orders.data());
}

void FreeState(void *state) {
  tpchq13vgen_free(static_cast<tpchq13vgen_state *>(state));
}

void *Apply(const void *state, const kit::Changes &changes, kit::Host &, int32_t *err) {
  const DeltaInputs inputs(changes);
  return tpchq13vgen_apply(static_cast<const tpchq13vgen_state *>(state),
                          inputs.customers.size(), inputs.customers.data(),
                          inputs.orders.size(), inputs.orders.data(), err);
}

bool Execute(const void *state, const core::QueryArguments &args,
             std::vector<core::AuxRow> &out) {
  if (!args.values.empty()) {
    return false;
  }
  const std::unique_ptr<tpchq13vgen_result, decltype(&tpchq13vgen_result_free)> result(
      tpchq13vgen_execute(static_cast<const tpchq13vgen_state *>(state)),
      tpchq13vgen_result_free);
  if (!result) {
    return false;
  }
  const std::size_t count = tpchq13vgen_result_len(result.get());
  out.reserve(out.size() + count);
  for (std::size_t index = 0; index < count; ++index) {
    int64_t c_count = 0;
    int64_t custdist = 0;
    const uint8_t has_count = tpchq13vgen_result_c0(result.get(), index, &c_count);
    const uint8_t has_dist = tpchq13vgen_result_c1(result.get(), index, &custdist);
    core::AuxRow row;
    row.values.reserve(2);
    row.values.push_back(has_count ? kit::OutI64(c_count) : kit::OutNull(core::AuxType::Int64));
    row.values.push_back(has_dist ? kit::OutI64(custdist) : kit::OutNull(core::AuxType::Int64));
    out.push_back(std::move(row));
  }
  return true;
}

void *PrepareApply(const void *store, const void *base, const kit::Changes &changes,
                   kit::Host &, int32_t *err) {
  const DeltaInputs inputs(changes);
  return tpchq13vgens_prepare_apply(static_cast<const tpchq13vgens_store *>(store),
                                   static_cast<const tpchq13vgens_base *>(base),
                                   inputs.customers.size(), inputs.customers.data(),
                                   inputs.orders.size(), inputs.orders.data(), err);
}

void *PrepareBuild(const void *store, kit::Host &host) {
  const ScanInputs inputs(host);
  return tpchq13vgens_prepare_build(static_cast<const tpchq13vgens_store *>(store),
                                   inputs.customers.size(), inputs.customers.data(),
                                   inputs.qualifying_orders.size(), inputs.qualifying_orders.data());
}

const kit::Glue kGlue = {
    "tpch_q13_vgen", "tpch", 13, kQuerySql,
    {{"customer", {"c_custkey"}}, {"orders", {"o_custkey", "o_comment"}}},
    {},
    Build, FreeState, Apply, Execute, PrepareApply, PrepareBuild,
    kit::StoreOps{
        []() -> void * { return tpchq13vgens_new(); },
        [](void *store) { tpchq13vgens_free(static_cast<tpchq13vgens_store *>(store)); },
        [](const void *store) -> void * {
          return tpchq13vgens_newest_base(static_cast<const tpchq13vgens_store *>(store));
        },
        [](const void *base) -> const void * {
          return tpchq13vgens_base_state(static_cast<const tpchq13vgens_base *>(base));
        },
        [](void *base) { tpchq13vgens_base_free(static_cast<tpchq13vgens_base *>(base)); },
        [](const void *store) -> void * {
          return tpchq13vgens_prepare_invalid(static_cast<const tpchq13vgens_store *>(store));
        },
        [](void *prepared) {
          tpchq13vgens_prepared_free(static_cast<tpchq13vgens_prepared *>(prepared));
        },
        [](const void *store, uint64_t from) -> int32_t {
          return tpchq13vgens_begin(static_cast<const tpchq13vgens_store *>(store), from);
        },
        [](const void *store) { tpchq13vgens_abort(static_cast<const tpchq13vgens_store *>(store)); },
        [](const void *store, void *prepared, uint64_t from, uint64_t floor,
           int32_t end_commit) -> int32_t {
          return tpchq13vgens_install(static_cast<const tpchq13vgens_store *>(store),
                                     static_cast<tpchq13vgens_prepared *>(prepared),
                                     from, floor, end_commit);
        },
        [](const void *store, uint64_t at, uint64_t tries, int32_t *status) -> const void * {
          return tpchq13vgens_read(static_cast<const tpchq13vgens_store *>(store),
                                  at, tries, status);
        },
        [](const void *state) {
          tpchq13vgens_state_release(static_cast<const tpchq13vgen_state *>(state));
        },
        [](const void *store) {
          tpchq13vgens_invalidate_all(static_cast<const tpchq13vgens_store *>(store));
        },
        [](const void *store) -> std::size_t {
          return tpchq13vgens_version_count(static_cast<const tpchq13vgens_store *>(store));
        },
    },
};

const kit::Registrar kRegistrar(&kGlue);

}
