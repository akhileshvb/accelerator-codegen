#include "tpch_q21_vgen_verus.h"
#include "accelerator_codegen/verus/verus_kit.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace kit = accelerator_codegen::verus_kit;
namespace core = kit::core;

namespace {

constexpr char kQuerySql[] = R"SQL(-- TPC-H Q21 (catalog text), :nation as $1, LIMIT 100.
SELECT s_name, COUNT(*) AS numwait
FROM supplier, lineitem l1, orders, nation
WHERE s_suppkey = l1.l_suppkey
  AND o_orderkey = l1.l_orderkey
  AND o_orderstatus = 'F'
  AND l1.l_receiptdate > l1.l_commitdate
  AND EXISTS (SELECT * FROM lineitem l2
              WHERE l2.l_orderkey = l1.l_orderkey
                AND l2.l_suppkey <> l1.l_suppkey)
  AND NOT EXISTS (SELECT * FROM lineitem l3
                  WHERE l3.l_orderkey = l1.l_orderkey
                    AND l3.l_suppkey <> l1.l_suppkey
                    AND l3.l_receiptdate > l3.l_commitdate)
  AND s_nationkey = n_nationkey
  AND n_name = $1
GROUP BY s_name
ORDER BY numwait DESC, s_name
LIMIT 100
)SQL";

constexpr char kWaitingLinesSql[] = R"SQL(SELECT l_orderkey, l_suppkey
FROM lineitem l1
WHERE l_receiptdate > l_commitdate
  AND EXISTS (SELECT * FROM orders o WHERE o.o_orderkey = l1.l_orderkey AND o.o_orderstatus = 'F')
  AND EXISTS (SELECT * FROM lineitem l2 WHERE l2.l_orderkey = l1.l_orderkey AND l2.l_suppkey <> l1.l_suppkey)
  AND NOT EXISTS (SELECT * FROM lineitem l3 WHERE l3.l_orderkey = l1.l_orderkey AND l3.l_suppkey <> l1.l_suppkey AND l3.l_receiptdate > l3.l_commitdate))SQL";

constexpr char kFinishedOrdersSql[] = R"SQL(SELECT o_orderkey FROM orders o
WHERE o_orderstatus = 'F'
  AND EXISTS (SELECT * FROM lineitem l1
              WHERE l1.l_orderkey = o.o_orderkey AND l1.l_receiptdate > l1.l_commitdate
                AND EXISTS (SELECT * FROM lineitem l2 WHERE l2.l_orderkey = l1.l_orderkey AND l2.l_suppkey <> l1.l_suppkey)
                AND NOT EXISTS (SELECT * FROM lineitem l3 WHERE l3.l_orderkey = l1.l_orderkey AND l3.l_suppkey <> l1.l_suppkey AND l3.l_receiptdate > l3.l_commitdate)))SQL";

constexpr char kSuppliersSql[] =
    R"SQL(SELECT s_suppkey, s_name, s_nationkey FROM supplier)SQL";
constexpr char kNationsSql[] =
    R"SQL(SELECT n_nationkey, n_name FROM nation)SQL";

constexpr char kOrderLinesSql[] =
    R"SQL(SELECT l_orderkey, l_suppkey, l_commitdate, l_receiptdate FROM lineitem)SQL";
constexpr char kOrderFinishedSql[] =
    R"SQL(SELECT o_orderkey FROM orders WHERE o_orderstatus = 'F')SQL";

struct BuildRows {
  std::vector<tpchq21vgen_waiting_line> waiting_lines;
  std::vector<tpchq21vgen_finished_order> finished_orders;
  std::vector<int64_t> supplier_suppkey;
  kit::TextColumn supplier_name;
  std::vector<int64_t> supplier_nationkey;
  std::vector<int64_t> nation_nationkey;
  kit::TextColumn nation_name;
};

struct LineitemDeltas {
  std::vector<tpchq21vgen_lineitem_delta> rows;
};

struct OrdersDeltas {
  std::vector<tpchq21vgen_orders_delta> rows;
};

struct SupplierDeltas {
  std::vector<int64_t> suppkey;
  kit::TextColumn name;
  std::vector<int64_t> nationkey;
  std::vector<int64_t> sign;
};

struct NationDeltas {
  std::vector<int64_t> nationkey;
  kit::TextColumn name;
  std::vector<int64_t> sign;
};

struct DeltaArrays {
  LineitemDeltas lineitem;
  NationDeltas nation;
  OrdersDeltas orders;
  SupplierDeltas supplier;
};

struct ApplyInputs {
  DeltaArrays deltas;
  std::vector<int64_t> order_lines_keys;
  std::vector<tpchq21vgen_order_line> order_lines_rows;
  std::vector<int64_t> order_finished_keys;
  std::vector<tpchq21vgen_order_finished> order_finished_rows;
};

void CollectBuildRows(kit::Host &host, BuildRows &rows) {
  host.Query(kWaitingLinesSql, [&](const core::AuxRow &row) {
    tpchq21vgen_waiting_line out{};
    out.l_orderkey = kit::I64(row, 0, 0);
    out.l_suppkey = kit::I64(row, 1, 0);
    rows.waiting_lines.push_back(out);
  });

  host.Query(kFinishedOrdersSql, [&](const core::AuxRow &row) {
    tpchq21vgen_finished_order out{};
    out.o_orderkey = kit::I64(row, 0, 0);
    rows.finished_orders.push_back(out);
  });

  host.Query(kSuppliersSql, [&](const core::AuxRow &row) {
    rows.supplier_suppkey.push_back(kit::I64(row, 0, 0));
    rows.supplier_name.Push(kit::Text(row, 1));
    rows.supplier_nationkey.push_back(kit::I64(row, 2, 0));
  });

  host.Query(kNationsSql, [&](const core::AuxRow &row) {
    rows.nation_nationkey.push_back(kit::I64(row, 0, 0));
    rows.nation_name.Push(kit::Text(row, 1));
  });
}

void CollectDeltas(const kit::Changes &changes, DeltaArrays &deltas) {
  const auto &lineitem_changes = changes[0];
  deltas.lineitem.rows.reserve(lineitem_changes.size());
  for (const auto &change : lineitem_changes) {
    tpchq21vgen_lineitem_delta out{};
    out.row.l_orderkey = kit::I64(change.row, 0, 0);
    out.row.l_suppkey = kit::I64(change.row, 1, 0);
    out.row.l_commitdate = kit::Date(change.row, 2);
    out.row.l_receiptdate = kit::Date(change.row, 3);
    out.sign = change.sign;
    deltas.lineitem.rows.push_back(out);
  }

  const auto &nation_changes = changes[3];
  deltas.nation.nationkey.reserve(nation_changes.size());
  deltas.nation.sign.reserve(nation_changes.size());
  for (const auto &change : nation_changes) {
    deltas.nation.nationkey.push_back(kit::I64(change.row, 0, 0));
    deltas.nation.name.Push(kit::Text(change.row, 1));
    deltas.nation.sign.push_back(change.sign);
  }

  const auto &orders_changes = changes[1];
  deltas.orders.rows.reserve(orders_changes.size());
  for (const auto &change : orders_changes) {
    tpchq21vgen_orders_delta out{};
    out.o_orderkey = kit::I64(change.row, 0, 0);
    out.is_finished = kit::Text(change.row, 1) == "F" ? 1 : 0;
    out.sign = change.sign;
    deltas.orders.rows.push_back(out);
  }

  const auto &supplier_changes = changes[2];
  deltas.supplier.suppkey.reserve(supplier_changes.size());
  deltas.supplier.nationkey.reserve(supplier_changes.size());
  deltas.supplier.sign.reserve(supplier_changes.size());
  for (const auto &change : supplier_changes) {
    deltas.supplier.suppkey.push_back(kit::I64(change.row, 0, 0));
    deltas.supplier.name.Push(kit::Text(change.row, 1));
    deltas.supplier.nationkey.push_back(kit::I64(change.row, 2, 0));
    deltas.supplier.sign.push_back(change.sign);
  }
}

template <typename KeyFunction>
std::vector<int64_t> CollectRescanKeys(KeyFunction key_function,
                                       const tpchq21vgen_state *state,
                                       const DeltaArrays &deltas,
                                       std::size_t reserve_count) {
  std::vector<int64_t> keys;
  keys.reserve(reserve_count == 0 ? 1 : reserve_count);
  keys.resize(keys.capacity());
  for (;;) {
    const std::size_t count =
        key_function(state, deltas.lineitem.rows.size(),
                     deltas.lineitem.rows.data(), deltas.nation.nationkey.size(),
                     deltas.nation.nationkey.data(), deltas.nation.name.bytes.data(),
                     deltas.nation.name.off.data(), deltas.nation.sign.data(),
                     deltas.orders.rows.size(), deltas.orders.rows.data(),
                     deltas.supplier.suppkey.size(), deltas.supplier.suppkey.data(),
                     deltas.supplier.name.bytes.data(), deltas.supplier.name.off.data(),
                     deltas.supplier.nationkey.data(), deltas.supplier.sign.data(),
                     keys.data(), keys.size());
    if (count <= keys.size()) {
      keys.resize(count);
      return keys;
    }
    keys.resize(count);
  }
}

void QueryOrderLines(kit::Host &host, const std::vector<int64_t> &keys,
                     std::vector<tpchq21vgen_order_line> &rows) {
  rows.reserve(keys.size());
  kit::QueryKeys(host, kOrderLinesSql, "l_orderkey", keys,
                 [&](const core::AuxRow &row) {
                   tpchq21vgen_order_line out{};
                   out.l_orderkey = kit::I64(row, 0, 0);
                   out.l_suppkey = kit::I64(row, 1, 0);
                   out.l_commitdate = kit::Date(row, 2);
                   out.l_receiptdate = kit::Date(row, 3);
                   rows.push_back(out);
                 });
}

void QueryOrderFinished(kit::Host &host, const std::vector<int64_t> &keys,
                        std::vector<tpchq21vgen_order_finished> &rows) {
  rows.reserve(keys.size());
  kit::QueryKeys(host, kOrderFinishedSql, "o_orderkey", keys,
                 [&](const core::AuxRow &row) {
                   tpchq21vgen_order_finished out{};
                   out.o_orderkey = kit::I64(row, 0, 0);
                   rows.push_back(out);
                 });
}

ApplyInputs CollectApplyInputs(const tpchq21vgen_state *state,
                               const kit::Changes &changes, kit::Host &host) {
  ApplyInputs inputs;
  CollectDeltas(changes, inputs.deltas);
  const std::size_t key_reserve =
      inputs.deltas.lineitem.rows.size() + inputs.deltas.orders.rows.size();
  inputs.order_lines_keys = CollectRescanKeys(
      tpchq21vgen_rescan_order_lines_keys, state, inputs.deltas, key_reserve);
  inputs.order_finished_keys = CollectRescanKeys(
      tpchq21vgen_rescan_order_finished_keys, state, inputs.deltas, key_reserve);
  QueryOrderLines(host, inputs.order_lines_keys, inputs.order_lines_rows);
  QueryOrderFinished(host, inputs.order_finished_keys, inputs.order_finished_rows);
  return inputs;
}

void *Build(kit::Host &host) {
  BuildRows rows;
  CollectBuildRows(host, rows);
  return static_cast<void *>(tpchq21vgen_build(
      rows.waiting_lines.size(), rows.waiting_lines.data(),
      rows.finished_orders.size(), rows.finished_orders.data(),
      rows.supplier_suppkey.size(), rows.supplier_suppkey.data(),
      rows.supplier_name.bytes.data(), rows.supplier_name.off.data(),
      rows.supplier_nationkey.data(), rows.nation_nationkey.size(),
      rows.nation_nationkey.data(), rows.nation_name.bytes.data(),
      rows.nation_name.off.data()));
}

void FreeState(void *state) {
  tpchq21vgen_free(static_cast<tpchq21vgen_state *>(state));
}

void *Apply(const void *state, const kit::Changes &changes, kit::Host &host,
            int32_t *err) {
  const auto *kernel_state = static_cast<const tpchq21vgen_state *>(state);
  ApplyInputs inputs = CollectApplyInputs(kernel_state, changes, host);
  const auto &d = inputs.deltas;
  return static_cast<void *>(tpchq21vgen_apply(
      kernel_state, d.lineitem.rows.size(), d.lineitem.rows.data(),
      d.nation.nationkey.size(), d.nation.nationkey.data(),
      d.nation.name.bytes.data(), d.nation.name.off.data(), d.nation.sign.data(),
      d.orders.rows.size(), d.orders.rows.data(), d.supplier.suppkey.size(),
      d.supplier.suppkey.data(), d.supplier.name.bytes.data(),
      d.supplier.name.off.data(), d.supplier.nationkey.data(),
      d.supplier.sign.data(), inputs.order_lines_keys.size(),
      inputs.order_lines_keys.data(), inputs.order_lines_rows.size(),
      inputs.order_lines_rows.data(), inputs.order_finished_keys.size(),
      inputs.order_finished_keys.data(), inputs.order_finished_rows.size(),
      inputs.order_finished_rows.data(), err));
}

bool Execute(const void *state, const core::QueryArguments &args,
             std::vector<core::AuxRow> &out) {
  std::string nation;
  if (!kit::ArgText(args, 0, nation)) {
    return false;
  }

  tpchq21vgen_result *result = tpchq21vgen_execute(
      static_cast<const tpchq21vgen_state *>(state),
      reinterpret_cast<const uint8_t *>(nation.data()), nation.size());
  if (result == nullptr) {
    return false;
  }

  const std::size_t row_count = tpchq21vgen_result_len(result);
  out.reserve(out.size() + row_count);
  for (std::size_t i = 0; i < row_count; ++i) {
    core::AuxRow row;
    row.values.reserve(2);

    const uint8_t *text = nullptr;
    std::size_t text_len = 0;
    if (tpchq21vgen_result_c0(result, i, &text, &text_len) != 0) {
      row.values.push_back(kit::OutText(text, text_len));
    } else {
      row.values.push_back(kit::OutNull(core::AuxType::String));
    }

    int64_t count = 0;
    if (tpchq21vgen_result_c1(result, i, &count) != 0) {
      row.values.push_back(kit::OutI64(count));
    } else {
      row.values.push_back(kit::OutNull(core::AuxType::Int64));
    }
    out.push_back(std::move(row));
  }

  tpchq21vgen_result_free(result);
  return true;
}

void *PrepareApply(const void *store, const void *base,
                   const kit::Changes &changes, kit::Host &host, int32_t *err) {
  const auto *kernel_store = static_cast<const tpchq21vgens_store *>(store);
  const auto *kernel_base = static_cast<const tpchq21vgens_base *>(base);
  const tpchq21vgen_state *base_state =
      tpchq21vgens_base_state(kernel_base);
  ApplyInputs inputs = CollectApplyInputs(base_state, changes, host);
  const auto &d = inputs.deltas;
  return static_cast<void *>(tpchq21vgens_prepare_apply(
      kernel_store, kernel_base, d.lineitem.rows.size(), d.lineitem.rows.data(),
      d.nation.nationkey.size(), d.nation.nationkey.data(),
      d.nation.name.bytes.data(), d.nation.name.off.data(), d.nation.sign.data(),
      d.orders.rows.size(), d.orders.rows.data(), d.supplier.suppkey.size(),
      d.supplier.suppkey.data(), d.supplier.name.bytes.data(),
      d.supplier.name.off.data(), d.supplier.nationkey.data(),
      d.supplier.sign.data(), inputs.order_lines_keys.size(),
      inputs.order_lines_keys.data(), inputs.order_lines_rows.size(),
      inputs.order_lines_rows.data(), inputs.order_finished_keys.size(),
      inputs.order_finished_keys.data(), inputs.order_finished_rows.size(),
      inputs.order_finished_rows.data(), err));
}

void *PrepareBuild(const void *store, kit::Host &host) {
  BuildRows rows;
  CollectBuildRows(host, rows);
  return static_cast<void *>(tpchq21vgens_prepare_build(
      static_cast<const tpchq21vgens_store *>(store), rows.waiting_lines.size(),
      rows.waiting_lines.data(), rows.finished_orders.size(),
      rows.finished_orders.data(), rows.supplier_suppkey.size(),
      rows.supplier_suppkey.data(), rows.supplier_name.bytes.data(),
      rows.supplier_name.off.data(), rows.supplier_nationkey.data(),
      rows.nation_nationkey.size(), rows.nation_nationkey.data(),
      rows.nation_name.bytes.data(), rows.nation_name.off.data()));
}

void *StoreNew() { return static_cast<void *>(tpchq21vgens_new()); }

void StoreFree(void *store) {
  tpchq21vgens_free(static_cast<tpchq21vgens_store *>(store));
}

void *StoreNewestBase(const void *store) {
  return static_cast<void *>(tpchq21vgens_newest_base(
      static_cast<const tpchq21vgens_store *>(store)));
}

const void *StoreBaseState(const void *base) {
  return static_cast<const void *>(
      tpchq21vgens_base_state(static_cast<const tpchq21vgens_base *>(base)));
}

void StoreBaseFree(void *base) {
  tpchq21vgens_base_free(static_cast<tpchq21vgens_base *>(base));
}

void *StorePrepareInvalid(const void *store) {
  return static_cast<void *>(tpchq21vgens_prepare_invalid(
      static_cast<const tpchq21vgens_store *>(store)));
}

void StorePreparedFree(void *prepared) {
  tpchq21vgens_prepared_free(
      static_cast<tpchq21vgens_prepared *>(prepared));
}

int32_t StoreBegin(const void *store, uint64_t from) {
  return tpchq21vgens_begin(static_cast<const tpchq21vgens_store *>(store),
                            from);
}

void StoreAbort(const void *store) {
  tpchq21vgens_abort(static_cast<const tpchq21vgens_store *>(store));
}

int32_t StoreInstall(const void *store, void *prepared, uint64_t from,
                     uint64_t floor, int32_t end_commit) {
  return tpchq21vgens_install(
      static_cast<const tpchq21vgens_store *>(store),
      static_cast<tpchq21vgens_prepared *>(prepared), from, floor, end_commit);
}

const void *StoreRead(const void *store, uint64_t at, uint64_t tries,
                      int32_t *status) {
  return static_cast<const void *>(tpchq21vgens_read(
      static_cast<const tpchq21vgens_store *>(store), at, tries, status));
}

void StoreStateRelease(const void *state) {
  tpchq21vgens_state_release(
      static_cast<const tpchq21vgen_state *>(state));
}

void StoreInvalidateAll(const void *store) {
  tpchq21vgens_invalidate_all(
      static_cast<const tpchq21vgens_store *>(store));
}

std::size_t StoreVersionCount(const void *store) {
  return tpchq21vgens_version_count(
      static_cast<const tpchq21vgens_store *>(store));
}

} // namespace

const kit::Glue kGlue = {
    "tpch_q21_vgen",
    "tpch",
    21,
    kQuerySql,
    {
        {"lineitem", {"l_orderkey", "l_suppkey", "l_commitdate", "l_receiptdate"}},
        {"orders", {"o_orderkey", "o_orderstatus"}},
        {"supplier", {"s_suppkey", "s_name", "s_nationkey"}},
        {"nation", {"n_nationkey", "n_name"}},
    },
    {kit::Kind::Text},
    Build,
    FreeState,
    Apply,
    Execute,
    PrepareApply,
    PrepareBuild,
    {
        StoreNew,
        StoreFree,
        StoreNewestBase,
        StoreBaseState,
        StoreBaseFree,
        StorePrepareInvalid,
        StorePreparedFree,
        StoreBegin,
        StoreAbort,
        StoreInstall,
        StoreRead,
        StoreStateRelease,
        StoreInvalidateAll,
        StoreVersionCount,
    },
};

const kit::Registrar kRegistrar(&kGlue);
