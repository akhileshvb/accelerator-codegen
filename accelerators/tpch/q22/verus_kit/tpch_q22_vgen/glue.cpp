#include "tpch_q22_vgen_verus.h"
#include "substrate/verus/kit/verus_kit.hpp"

#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace kit = accelerator_codegen::verus_kit;
namespace core = accelerator_codegen::aux_state::core;

constexpr const char *kQuerySql = R"SQL(-- TPC-H Q22 (catalog text), :country_code1..:country_code7 as $1..$7.
WITH custsale AS (
  SELECT SUBSTRING(c_phone FROM 1 FOR 2) AS cntrycode, c_acctbal
  FROM customer
  WHERE SUBSTRING(c_phone FROM 1 FOR 2) IN ($1, $2, $3, $4, $5, $6, $7)
    AND c_acctbal > (SELECT AVG(c_acctbal) FROM customer WHERE c_acctbal > 0.00
                     AND SUBSTRING(c_phone FROM 1 FOR 2) IN ($1, $2, $3, $4, $5, $6, $7))
    AND NOT EXISTS (SELECT * FROM orders WHERE o_custkey = c_custkey))
SELECT cntrycode, COUNT(*) AS numcust, SUM(c_acctbal) AS totacctbal
FROM custsale
GROUP BY cntrycode
ORDER BY cntrycode
)SQL";

constexpr const char *kOrderScanSql = "SELECT o_custkey FROM orders;";
constexpr const char *kCustomerScanSql =
    "SELECT c_custkey, c_phone, c_acctbal FROM customer\n"
    "  WHERE c_acctbal > 0.00;";

struct CustomerColumns {
  std::vector<int64_t> keys;
  kit::TextColumn phones;
  std::vector<int64_t> balances;

  void Reserve(std::size_t count) {
    keys.reserve(count);
    phones.off.reserve(count + 1);
    balances.reserve(count);
  }

  void Push(const core::AuxRow &row) {
    keys.push_back(kit::I64(row, 0, 0));
    phones.Push(kit::Text(row, 1));
    balances.push_back(kit::I64(row, 2, 2));
  }
};

struct ScanInputs {
  std::vector<tpchq22vgen_order_input> orders;
  CustomerColumns customers;

  explicit ScanInputs(kit::Host &host) {
    host.Query(kOrderScanSql, [&](const core::AuxRow &row) {
      orders.push_back({kit::I64(row, 0, 0)});
    });
    host.Query(kCustomerScanSql, [&](const core::AuxRow &row) {
      customers.Push(row);
    });
  }
};

void *Build(kit::Host &host) {
  ScanInputs inputs(host);
  return tpchq22vgen_build(
      inputs.orders.size(), inputs.orders.data(), inputs.customers.keys.size(),
      inputs.customers.keys.data(), inputs.customers.phones.bytes.data(),
      inputs.customers.phones.off.data(), inputs.customers.balances.data());
}

void *PrepareBuild(const void *store, kit::Host &host) {
  ScanInputs inputs(host);
  return tpchq22vgens_prepare_build(
      static_cast<const tpchq22vgens_store *>(store), inputs.orders.size(),
      inputs.orders.data(), inputs.customers.keys.size(), inputs.customers.keys.data(),
      inputs.customers.phones.bytes.data(), inputs.customers.phones.off.data(),
      inputs.customers.balances.data());
}

void FreeState(void *state) {
  tpchq22vgen_free(static_cast<tpchq22vgen_state *>(state));
}

struct ApplyInputs {
  CustomerColumns customers;
  std::vector<int64_t> customer_signs;
  std::vector<tpchq22vgen_orders_delta> orders;
  std::vector<int64_t> rescan_keys;
  CustomerColumns rescan_customers;

  ApplyInputs(const tpchq22vgen_state *state, const kit::Changes &changes,
              kit::Host &host) {
    const auto &customer_changes = changes.at(0);
    const auto &order_changes = changes.at(1);
    customers.Reserve(customer_changes.size());
    customer_signs.reserve(customer_changes.size());
    for (const auto &change : customer_changes) {
      customers.Push(change.row);
      customer_signs.push_back(change.sign);
    }
    orders.reserve(order_changes.size());
    for (const auto &change : order_changes) {
      orders.push_back({{kit::I64(change.row, 0, 0)}, change.sign});
    }

    rescan_keys.resize(orders.size());
    for (;;) {
      const std::size_t count = tpchq22vgen_rescan_positive_customers_by_key_keys(
          state, customers.keys.size(), customers.keys.data(),
          customers.phones.bytes.data(), customers.phones.off.data(),
          customers.balances.data(), customer_signs.data(), orders.size(),
          orders.data(), rescan_keys.data(), rescan_keys.size());
      if (count <= rescan_keys.size()) {
        rescan_keys.resize(count);
        break;
      }
      rescan_keys.resize(count);
    }
    rescan_customers.Reserve(rescan_keys.size());
    kit::QueryKeys(host, kCustomerScanSql, "c_custkey", rescan_keys,
                   [&](const core::AuxRow &row) { rescan_customers.Push(row); });
  }
};

void *Apply(const void *state, const kit::Changes &changes, kit::Host &host,
            int32_t *err) {
  const auto *typed_state = static_cast<const tpchq22vgen_state *>(state);
  ApplyInputs inputs(typed_state, changes, host);
  return tpchq22vgen_apply(
      typed_state, inputs.customers.keys.size(), inputs.customers.keys.data(),
      inputs.customers.phones.bytes.data(), inputs.customers.phones.off.data(),
      inputs.customers.balances.data(), inputs.customer_signs.data(),
      inputs.orders.size(), inputs.orders.data(), inputs.rescan_keys.size(),
      inputs.rescan_keys.data(), inputs.rescan_customers.keys.size(),
      inputs.rescan_customers.keys.data(), inputs.rescan_customers.phones.bytes.data(),
      inputs.rescan_customers.phones.off.data(), inputs.rescan_customers.balances.data(),
      err);
}

void *PrepareApply(const void *store, const void *base, const kit::Changes &changes,
                   kit::Host &host, int32_t *err) {
  const auto *typed_base = static_cast<const tpchq22vgens_base *>(base);
  ApplyInputs inputs(tpchq22vgens_base_state(typed_base), changes, host);
  return tpchq22vgens_prepare_apply(
      static_cast<const tpchq22vgens_store *>(store), typed_base,
      inputs.customers.keys.size(), inputs.customers.keys.data(),
      inputs.customers.phones.bytes.data(), inputs.customers.phones.off.data(),
      inputs.customers.balances.data(), inputs.customer_signs.data(),
      inputs.orders.size(), inputs.orders.data(), inputs.rescan_keys.size(),
      inputs.rescan_keys.data(), inputs.rescan_customers.keys.size(),
      inputs.rescan_customers.keys.data(), inputs.rescan_customers.phones.bytes.data(),
      inputs.rescan_customers.phones.off.data(), inputs.rescan_customers.balances.data(),
      err);
}

bool Execute(const void *state, const core::QueryArguments &args,
             std::vector<core::AuxRow> &out) {
  std::array<std::string, 7> params;
  for (std::size_t index = 0; index < params.size(); ++index) {
    if (!kit::ArgText(args, index, params[index])) {
      return false;
    }
  }
  std::unique_ptr<tpchq22vgen_result, decltype(&tpchq22vgen_result_free)> result(
      tpchq22vgen_execute(
          static_cast<const tpchq22vgen_state *>(state),
          reinterpret_cast<const uint8_t *>(params[0].data()), params[0].size(),
          reinterpret_cast<const uint8_t *>(params[1].data()), params[1].size(),
          reinterpret_cast<const uint8_t *>(params[2].data()), params[2].size(),
          reinterpret_cast<const uint8_t *>(params[3].data()), params[3].size(),
          reinterpret_cast<const uint8_t *>(params[4].data()), params[4].size(),
          reinterpret_cast<const uint8_t *>(params[5].data()), params[5].size(),
          reinterpret_cast<const uint8_t *>(params[6].data()), params[6].size()),
      &tpchq22vgen_result_free);
  if (!result) {
    return false;
  }

  const std::size_t count = tpchq22vgen_result_len(result.get());
  out.reserve(out.size() + count);
  for (std::size_t index = 0; index < count; ++index) {
    core::AuxRow row;
    row.values.reserve(3);
    const uint8_t *text = nullptr;
    std::size_t text_len = 0;
    int64_t numcust = 0;
    int64_t total_hi = 0;
    uint64_t total_lo = 0;
    row.values.push_back(tpchq22vgen_result_c0(result.get(), index, &text, &text_len)
                             ? kit::OutText(text, text_len)
                             : kit::OutNull(core::AuxType::String));
    row.values.push_back(tpchq22vgen_result_c1(result.get(), index, &numcust)
                             ? kit::OutI64(numcust)
                             : kit::OutNull(core::AuxType::Int64));
    row.values.push_back(tpchq22vgen_result_c2(result.get(), index, &total_hi, &total_lo)
                             ? kit::OutDec(total_hi, total_lo, 2)
                             : kit::OutNull(core::AuxType::ScaledDecimal));
    out.push_back(std::move(row));
  }
  return true;
}

const kit::Glue kGlue = {
    "tpch_q22_vgen", "tpch", 22, kQuerySql,
    {{"customer", {"c_custkey", "c_phone", "c_acctbal"}},
     {"orders", {"o_custkey"}}},
    {kit::Kind::Text, kit::Kind::Text, kit::Kind::Text, kit::Kind::Text,
     kit::Kind::Text, kit::Kind::Text, kit::Kind::Text},
    Build, FreeState, Apply, Execute, PrepareApply, PrepareBuild,
    kit::StoreOps{
        []() -> void * { return tpchq22vgens_new(); },
        [](void *store) { tpchq22vgens_free(static_cast<tpchq22vgens_store *>(store)); },
        [](const void *store) -> void * {
          return tpchq22vgens_newest_base(static_cast<const tpchq22vgens_store *>(store));
        },
        [](const void *base) -> const void * {
          return tpchq22vgens_base_state(static_cast<const tpchq22vgens_base *>(base));
        },
        [](void *base) { tpchq22vgens_base_free(static_cast<tpchq22vgens_base *>(base)); },
        [](const void *store) -> void * {
          return tpchq22vgens_prepare_invalid(static_cast<const tpchq22vgens_store *>(store));
        },
        [](void *prepared) {
          tpchq22vgens_prepared_free(static_cast<tpchq22vgens_prepared *>(prepared));
        },
        [](const void *store, uint64_t from) {
          return tpchq22vgens_begin(static_cast<const tpchq22vgens_store *>(store), from);
        },
        [](const void *store) { tpchq22vgens_abort(static_cast<const tpchq22vgens_store *>(store)); },
        [](const void *store, void *prepared, uint64_t from, uint64_t floor, int32_t end_commit) {
          return tpchq22vgens_install(static_cast<const tpchq22vgens_store *>(store),
                                     static_cast<tpchq22vgens_prepared *>(prepared),
                                     from, floor, end_commit);
        },
        [](const void *store, uint64_t at, uint64_t tries, int32_t *status) -> const void * {
          return tpchq22vgens_read(static_cast<const tpchq22vgens_store *>(store), at, tries, status);
        },
        [](const void *state) {
          tpchq22vgens_state_release(static_cast<const tpchq22vgen_state *>(state));
        },
        [](const void *store) {
          tpchq22vgens_invalidate_all(static_cast<const tpchq22vgens_store *>(store));
        },
        [](const void *store) {
          return tpchq22vgens_version_count(static_cast<const tpchq22vgens_store *>(store));
        },
    }};

const kit::Registrar kRegistrar(&kGlue);

}
