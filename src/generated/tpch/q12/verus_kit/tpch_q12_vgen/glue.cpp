#include "tpch_q12_vgen_verus.h"
#include "accelerator_codegen/verus/verus_kit.hpp"

#include <memory>
#include <utility>

namespace {

namespace kit = accelerator_codegen::verus_kit;
namespace core = accelerator_codegen::aux_state::core;

constexpr const char *kQuerySql = R"SQL(-- TPC-H Q12 (catalog text), :shipmode1 as $1, :shipmode2 as $2, :start_date as $3, $4 = $3 + 1 year (derived end date: sql2verus cannot add months to a non-literal date).
SELECT l_shipmode,
       SUM(CASE WHEN o_orderpriority = '1-URGENT' OR o_orderpriority = '2-HIGH' THEN 1 ELSE 0 END) AS high_line_count,
       SUM(CASE WHEN o_orderpriority <> '1-URGENT' AND o_orderpriority <> '2-HIGH' THEN 1 ELSE 0 END) AS low_line_count
FROM orders, lineitem
WHERE o_orderkey = l_orderkey AND l_shipmode IN ($1, $2)
  AND l_commitdate < l_receiptdate AND l_shipdate < l_commitdate
  AND l_receiptdate >= $3
  AND l_receiptdate < $4
GROUP BY l_shipmode
ORDER BY l_shipmode
)SQL";

constexpr const char *kHighOrdersSql = R"SQL(SELECT o_orderkey FROM orders
WHERE o_orderpriority = '1-URGENT' OR o_orderpriority = '2-HIGH';)SQL";
constexpr const char *kLowOrdersSql = R"SQL(SELECT o_orderkey FROM orders
WHERE o_orderpriority <> '1-URGENT' AND o_orderpriority <> '2-HIGH';)SQL";
constexpr const char *kQualifyingLinesSql = R"SQL(SELECT l_orderkey, l_receiptdate, l_shipmode FROM lineitem
WHERE l_commitdate < l_receiptdate AND l_shipdate < l_commitdate;)SQL";

struct QualifyingLines {
  std::vector<int64_t> orderkey;
  std::vector<int32_t> receiptdate;
  kit::TextColumn shipmode;

  void Push(const core::AuxRow &row) {
    orderkey.push_back(kit::I64(row, 0, 0));
    receiptdate.push_back(kit::Date(row, 1));
    shipmode.Push(kit::Text(row, 2));
  }
};

struct Scans {
  std::vector<tpchq12vgen_order_key_input> high_orders;
  std::vector<tpchq12vgen_order_key_input> low_orders;
  QualifyingLines lines;

  explicit Scans(kit::Host &host) {
    host.Query(kHighOrdersSql, [&](const core::AuxRow &row) {
      high_orders.push_back({kit::I64(row, 0, 0)});
    });
    host.Query(kLowOrdersSql, [&](const core::AuxRow &row) {
      low_orders.push_back({kit::I64(row, 0, 0)});
    });
    host.Query(kQualifyingLinesSql, [&](const core::AuxRow &row) {
      lines.Push(row);
    });
  }
};

void *Build(kit::Host &host) {
  const Scans scans(host);
  return tpchq12vgen_build(
      scans.high_orders.size(), scans.high_orders.data(),
      scans.low_orders.size(), scans.low_orders.data(),
      scans.lines.orderkey.size(), scans.lines.orderkey.data(),
      scans.lines.receiptdate.data(), scans.lines.shipmode.bytes.data(),
      scans.lines.shipmode.off.data());
}

void FreeState(void *state) {
  tpchq12vgen_free(static_cast<tpchq12vgen_state *>(state));
}

void *PrepareBuild(const void *store, kit::Host &host) {
  const Scans scans(host);
  return tpchq12vgens_prepare_build(
      static_cast<const tpchq12vgens_store *>(store),
      scans.high_orders.size(), scans.high_orders.data(),
      scans.low_orders.size(), scans.low_orders.data(),
      scans.lines.orderkey.size(), scans.lines.orderkey.data(),
      scans.lines.receiptdate.data(), scans.lines.shipmode.bytes.data(),
      scans.lines.shipmode.off.data());
}

struct Deltas {
  std::vector<int64_t> orderkey;
  std::vector<int32_t> shipdate;
  std::vector<int32_t> commitdate;
  std::vector<int32_t> receiptdate;
  kit::TextColumn shipmode;
  std::vector<int64_t> sign;
  std::vector<tpchq12vgen_order_delta> orders;

  explicit Deltas(const kit::Changes &changes) {
    const auto &line_changes = changes.at(0);
    orderkey.reserve(line_changes.size());
    shipdate.reserve(line_changes.size());
    commitdate.reserve(line_changes.size());
    receiptdate.reserve(line_changes.size());
    shipmode.off.reserve(line_changes.size() + 1);
    sign.reserve(line_changes.size());
    for (const auto &change : line_changes) {
      orderkey.push_back(kit::I64(change.row, 0, 0));
      shipdate.push_back(kit::Date(change.row, 1));
      commitdate.push_back(kit::Date(change.row, 2));
      receiptdate.push_back(kit::Date(change.row, 3));
      shipmode.Push(kit::Text(change.row, 4));
      sign.push_back(change.sign);
    }
    const auto &order_changes = changes.at(1);
    orders.reserve(order_changes.size());
    for (const auto &change : order_changes) {
      const auto &priority = kit::Text(change.row, 1);
      orders.push_back({kit::I64(change.row, 0, 0),
                        static_cast<uint8_t>(priority == "1-URGENT" ||
                                             priority == "2-HIGH"),
                        change.sign});
    }
  }
};

using KeySelector = decltype(&tpchq12vgen_rescan_high_orders_for_keys_keys);

std::vector<int64_t> RescanKeys(KeySelector selector,
                                const tpchq12vgen_state *state,
                                const Deltas &delta) {
  std::vector<int64_t> keys(delta.orderkey.size() + delta.orders.size());
  for (;;) {
    const size_t count = selector(
        state, delta.orderkey.size(), delta.orderkey.data(),
        delta.shipdate.data(), delta.commitdate.data(), delta.receiptdate.data(),
        delta.shipmode.bytes.data(), delta.shipmode.off.data(), delta.sign.data(),
        delta.orders.size(), delta.orders.data(), keys.data(), keys.size());
    if (count <= keys.size()) {
      keys.resize(count);
      return keys;
    }
    keys.resize(count);
  }
}

struct ApplyInputs {
  Deltas delta;
  std::vector<int64_t> high_keys;
  std::vector<int64_t> low_keys;
  std::vector<int64_t> line_keys;
  std::vector<tpchq12vgen_order_key_input> high_orders;
  std::vector<tpchq12vgen_order_key_input> low_orders;
  QualifyingLines lines;

  ApplyInputs(const tpchq12vgen_state *state, const kit::Changes &changes,
              kit::Host &host)
      : delta(changes),
        high_keys(RescanKeys(tpchq12vgen_rescan_high_orders_for_keys_keys,
                             state, delta)),
        low_keys(RescanKeys(tpchq12vgen_rescan_low_orders_for_keys_keys,
                            state, delta)),
        line_keys(RescanKeys(tpchq12vgen_rescan_qualifying_lines_for_keys_keys,
                             state, delta)) {
    high_orders.reserve(high_keys.size());
    low_orders.reserve(low_keys.size());
    kit::QueryKeys(host, kHighOrdersSql, "o_orderkey", high_keys,
                   [&](const core::AuxRow &row) {
                     high_orders.push_back({kit::I64(row, 0, 0)});
                   });
    kit::QueryKeys(host, kLowOrdersSql, "o_orderkey", low_keys,
                   [&](const core::AuxRow &row) {
                     low_orders.push_back({kit::I64(row, 0, 0)});
                   });
    kit::QueryKeys(host, kQualifyingLinesSql, "l_orderkey", line_keys,
                   [&](const core::AuxRow &row) { lines.Push(row); });
  }
};

void *Apply(const void *state, const kit::Changes &changes, kit::Host &host,
            int32_t *err) {
  const auto *kernel_state = static_cast<const tpchq12vgen_state *>(state);
  const ApplyInputs inputs(kernel_state, changes, host);
  const auto &delta = inputs.delta;
  return tpchq12vgen_apply(
      kernel_state, delta.orderkey.size(), delta.orderkey.data(),
      delta.shipdate.data(), delta.commitdate.data(), delta.receiptdate.data(),
      delta.shipmode.bytes.data(), delta.shipmode.off.data(), delta.sign.data(),
      delta.orders.size(), delta.orders.data(),
      inputs.high_keys.size(), inputs.high_keys.data(),
      inputs.high_orders.size(), inputs.high_orders.data(),
      inputs.low_keys.size(), inputs.low_keys.data(),
      inputs.low_orders.size(), inputs.low_orders.data(),
      inputs.line_keys.size(), inputs.line_keys.data(),
      inputs.lines.orderkey.size(), inputs.lines.orderkey.data(),
      inputs.lines.receiptdate.data(), inputs.lines.shipmode.bytes.data(),
      inputs.lines.shipmode.off.data(), err);
}

void *PrepareApply(const void *store, const void *base,
                   const kit::Changes &changes, kit::Host &host, int32_t *err) {
  const auto *kernel_base = static_cast<const tpchq12vgens_base *>(base);
  const ApplyInputs inputs(tpchq12vgens_base_state(kernel_base), changes, host);
  const auto &delta = inputs.delta;
  return tpchq12vgens_prepare_apply(
      static_cast<const tpchq12vgens_store *>(store), kernel_base,
      delta.orderkey.size(), delta.orderkey.data(),
      delta.shipdate.data(), delta.commitdate.data(), delta.receiptdate.data(),
      delta.shipmode.bytes.data(), delta.shipmode.off.data(), delta.sign.data(),
      delta.orders.size(), delta.orders.data(),
      inputs.high_keys.size(), inputs.high_keys.data(),
      inputs.high_orders.size(), inputs.high_orders.data(),
      inputs.low_keys.size(), inputs.low_keys.data(),
      inputs.low_orders.size(), inputs.low_orders.data(),
      inputs.line_keys.size(), inputs.line_keys.data(),
      inputs.lines.orderkey.size(), inputs.lines.orderkey.data(),
      inputs.lines.receiptdate.data(), inputs.lines.shipmode.bytes.data(),
      inputs.lines.shipmode.off.data(), err);
}

bool Execute(const void *state, const core::QueryArguments &args,
             std::vector<core::AuxRow> &out) {
  std::string mode1;
  std::string mode2;
  int32_t start_date;
  int32_t end_date;
  if (!kit::ArgText(args, 0, mode1) || !kit::ArgText(args, 1, mode2) ||
      !kit::ArgDate(args, 2, start_date) || !kit::ArgDate(args, 3, end_date)) {
    return false;
  }
  std::unique_ptr<tpchq12vgen_result, decltype(&tpchq12vgen_result_free)> result(
      tpchq12vgen_execute(static_cast<const tpchq12vgen_state *>(state),
                         reinterpret_cast<const uint8_t *>(mode1.data()), mode1.size(),
                         reinterpret_cast<const uint8_t *>(mode2.data()), mode2.size(),
                         start_date, end_date),
      tpchq12vgen_result_free);
  if (!result) {
    return false;
  }
  const size_t count = tpchq12vgen_result_len(result.get());
  out.reserve(out.size() + count);
  for (size_t index = 0; index < count; ++index) {
    core::AuxRow row;
    row.values.reserve(3);
    const uint8_t *text = nullptr;
    size_t text_len = 0;
    int64_t high = 0;
    int64_t low = 0;
    row.values.push_back(tpchq12vgen_result_c0(result.get(), index, &text, &text_len)
                             ? kit::OutText(text, text_len)
                             : kit::OutNull(core::AuxType::String));
    row.values.push_back(tpchq12vgen_result_c1(result.get(), index, &high)
                             ? kit::OutI64(high)
                             : kit::OutNull(core::AuxType::Int64));
    row.values.push_back(tpchq12vgen_result_c2(result.get(), index, &low)
                             ? kit::OutI64(low)
                             : kit::OutNull(core::AuxType::Int64));
    out.push_back(std::move(row));
  }
  return true;
}

const kit::Glue kGlue = {
    "tpch_q12_vgen", "tpch", 12, kQuerySql,
    {{"lineitem", {"l_orderkey", "l_shipdate", "l_commitdate", "l_receiptdate", "l_shipmode"}},
     {"orders", {"o_orderkey", "o_orderpriority"}}},
    {kit::Kind::Text, kit::Kind::Text, kit::Kind::Date, kit::Kind::Date},
    Build, FreeState, Apply, Execute, PrepareApply, PrepareBuild,
    kit::StoreOps{
        []() -> void * { return tpchq12vgens_new(); },
        [](void *store) { tpchq12vgens_free(static_cast<tpchq12vgens_store *>(store)); },
        [](const void *store) -> void * {
          return tpchq12vgens_newest_base(static_cast<const tpchq12vgens_store *>(store));
        },
        [](const void *base) -> const void * {
          return tpchq12vgens_base_state(static_cast<const tpchq12vgens_base *>(base));
        },
        [](void *base) { tpchq12vgens_base_free(static_cast<tpchq12vgens_base *>(base)); },
        [](const void *store) -> void * {
          return tpchq12vgens_prepare_invalid(static_cast<const tpchq12vgens_store *>(store));
        },
        [](void *prepared) {
          tpchq12vgens_prepared_free(static_cast<tpchq12vgens_prepared *>(prepared));
        },
        [](const void *store, uint64_t from) {
          return tpchq12vgens_begin(static_cast<const tpchq12vgens_store *>(store), from);
        },
        [](const void *store) { tpchq12vgens_abort(static_cast<const tpchq12vgens_store *>(store)); },
        [](const void *store, void *prepared, uint64_t from, uint64_t floor, int32_t end_commit) {
          return tpchq12vgens_install(static_cast<const tpchq12vgens_store *>(store),
                                     static_cast<tpchq12vgens_prepared *>(prepared),
                                     from, floor, end_commit);
        },
        [](const void *store, uint64_t at, uint64_t tries, int32_t *status) -> const void * {
          return tpchq12vgens_read(static_cast<const tpchq12vgens_store *>(store),
                                  at, tries, status);
        },
        [](const void *state) {
          tpchq12vgens_state_release(static_cast<const tpchq12vgen_state *>(state));
        },
        [](const void *store) {
          tpchq12vgens_invalidate_all(static_cast<const tpchq12vgens_store *>(store));
        },
        [](const void *store) {
          return tpchq12vgens_version_count(static_cast<const tpchq12vgens_store *>(store));
        }}};

const kit::Registrar kRegistrar(&kGlue);

}
