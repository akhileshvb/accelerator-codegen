#include "tpch_q10_vgen_verus.h"

#include "accelerator_codegen/verus/verus_kit.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace kit = accelerator_codegen::verus_kit;

namespace {

constexpr char kQuerySql[] =
    R"SQL(-- TPC-H Q10 (catalog text), :start_date as $1, $2 = $1 + 3 months (derived end date: sql2verus cannot add months to a non-literal date), LIMIT 20.
SELECT c_custkey, c_name, SUM(l_extendedprice * (1 - l_discount)) AS revenue, c_acctbal, n_name, c_address, c_phone, c_comment
FROM customer JOIN orders ON c_custkey = o_custkey
  JOIN lineitem ON l_orderkey = o_orderkey JOIN nation ON c_nationkey = n_nationkey
WHERE o_orderdate >= $1
  AND o_orderdate < $2 AND l_returnflag = 'R'
GROUP BY c_custkey, c_name, c_acctbal, c_phone, n_name, c_address, c_comment
ORDER BY revenue DESC, c_custkey
LIMIT 20
)SQL";

constexpr char kCustomerScanSql[] =
    "SELECT c_custkey, c_name, c_address, c_nationkey, c_phone, c_acctbal, "
    "c_comment FROM customer;";
constexpr char kNationScanSql[] =
    "SELECT n_nationkey, n_name FROM nation;";
constexpr char kOrderScanSql[] =
    "SELECT o_orderkey, o_custkey, o_orderdate FROM orders;";
constexpr char kReturnedLineScanSql[] =
    "SELECT l_orderkey, l_extendedprice, l_discount FROM lineitem WHERE "
    "l_returnflag = 'R';";

constexpr char kOrdersRescanSql[] =
    "SELECT o_orderkey, o_custkey, o_orderdate FROM orders;";
constexpr char kReturnedLinesRescanSql[] =
    "SELECT l_orderkey, l_extendedprice, l_discount FROM lineitem WHERE "
    "l_returnflag = 'R';";

struct Customers {
  std::vector<std::int64_t> c_custkey;
  kit::TextColumn c_name;
  kit::TextColumn c_address;
  std::vector<std::int64_t> c_nationkey;
  kit::TextColumn c_phone;
  std::vector<std::int64_t> c_acctbal;
  kit::TextColumn c_comment;
};

struct Nations {
  std::vector<std::int64_t> n_nationkey;
  kit::TextColumn n_name;
};

struct CustomerDeltas {
  std::vector<std::int64_t> c_custkey;
  kit::TextColumn c_name;
  kit::TextColumn c_address;
  std::vector<std::int64_t> c_nationkey;
  kit::TextColumn c_phone;
  std::vector<std::int64_t> c_acctbal;
  kit::TextColumn c_comment;
  std::vector<std::int64_t> sign;

  explicit CustomerDeltas(const std::vector<kit::Change> &changes) {
    c_custkey.reserve(changes.size());
    c_nationkey.reserve(changes.size());
    c_acctbal.reserve(changes.size());
    sign.reserve(changes.size());
    c_name.bytes.reserve(changes.size() * 25);
    c_address.bytes.reserve(changes.size() * 40);
    c_phone.bytes.reserve(changes.size() * 15);
    c_comment.bytes.reserve(changes.size() * 117);
    c_name.off.reserve(changes.size() + 1);
    c_address.off.reserve(changes.size() + 1);
    c_phone.off.reserve(changes.size() + 1);
    c_comment.off.reserve(changes.size() + 1);

    for (const auto &change : changes) {
      const auto &row = change.row;
      c_custkey.push_back(kit::I64(row, 0, 0));
      c_name.Push(kit::Text(row, 1));
      c_address.Push(kit::Text(row, 2));
      c_nationkey.push_back(kit::I64(row, 3, 0));
      c_phone.Push(kit::Text(row, 4));
      c_acctbal.push_back(kit::I64(row, 5, 2));
      c_comment.Push(kit::Text(row, 6));
      sign.push_back(change.sign);
    }
  }
};

struct NationDeltas {
  std::vector<std::int64_t> n_nationkey;
  kit::TextColumn n_name;
  std::vector<std::int64_t> sign;

  explicit NationDeltas(const std::vector<kit::Change> &changes) {
    n_nationkey.reserve(changes.size());
    sign.reserve(changes.size());
    n_name.bytes.reserve(changes.size() * 25);
    n_name.off.reserve(changes.size() + 1);

    for (const auto &change : changes) {
      n_nationkey.push_back(kit::I64(change.row, 0, 0));
      n_name.Push(kit::Text(change.row, 1));
      sign.push_back(change.sign);
    }
  }
};

struct OrderDeltas {
  std::vector<tpchq10vgen_order_delta> rows;

  explicit OrderDeltas(const std::vector<kit::Change> &changes) {
    rows.reserve(changes.size());
    for (const auto &change : changes) {
      tpchq10vgen_order_delta delta{};
      delta.row.o_orderkey = kit::I64(change.row, 0, 0);
      delta.row.o_custkey = kit::I64(change.row, 1, 0);
      delta.row.o_orderdate = kit::Date(change.row, 2);
      delta.sign = change.sign;
      rows.push_back(delta);
    }
  }
};

struct LineDeltas {
  std::vector<tpchq10vgen_line_delta> rows;

  explicit LineDeltas(const std::vector<kit::Change> &changes) {
    rows.reserve(changes.size());
    for (const auto &change : changes) {
      tpchq10vgen_line_delta delta{};
      delta.l_orderkey = kit::I64(change.row, 0, 0);
      delta.l_extendedprice = kit::I64(change.row, 1, 2);
      delta.l_discount = kit::I64(change.row, 2, 2);
      delta.is_returned = kit::Text(change.row, 3) == "R" ? 1 : 0;
      delta.sign = change.sign;
      rows.push_back(delta);
    }
  }
};

struct BuildRows {
  Customers customers;
  Nations nations;
  std::vector<tpchq10vgen_order_input> orders;
  std::vector<tpchq10vgen_returned_line_input> returned_lines;

  explicit BuildRows(kit::Host &host) {
    constexpr std::size_t kInitialRows = 1024;
    customers.c_custkey.reserve(kInitialRows);
    customers.c_nationkey.reserve(kInitialRows);
    customers.c_acctbal.reserve(kInitialRows);
    customers.c_name.off.reserve(kInitialRows + 1);
    customers.c_address.off.reserve(kInitialRows + 1);
    customers.c_phone.off.reserve(kInitialRows + 1);
    customers.c_comment.off.reserve(kInitialRows + 1);
    nations.n_nationkey.reserve(kInitialRows);
    nations.n_name.off.reserve(kInitialRows + 1);
    orders.reserve(kInitialRows);
    returned_lines.reserve(kInitialRows);

    host.Query(kCustomerScanSql, [this](const kit::core::AuxRow &row) {
      customers.c_custkey.push_back(kit::I64(row, 0, 0));
      customers.c_name.Push(kit::Text(row, 1));
      customers.c_address.Push(kit::Text(row, 2));
      customers.c_nationkey.push_back(kit::I64(row, 3, 0));
      customers.c_phone.Push(kit::Text(row, 4));
      customers.c_acctbal.push_back(kit::I64(row, 5, 2));
      customers.c_comment.Push(kit::Text(row, 6));
    });
    host.Query(kNationScanSql, [this](const kit::core::AuxRow &row) {
      nations.n_nationkey.push_back(kit::I64(row, 0, 0));
      nations.n_name.Push(kit::Text(row, 1));
    });
    host.Query(kOrderScanSql, [this](const kit::core::AuxRow &row) {
      tpchq10vgen_order_input input{};
      input.o_orderkey = kit::I64(row, 0, 0);
      input.o_custkey = kit::I64(row, 1, 0);
      input.o_orderdate = kit::Date(row, 2);
      orders.push_back(input);
    });
    host.Query(kReturnedLineScanSql, [this](const kit::core::AuxRow &row) {
      tpchq10vgen_returned_line_input input{};
      input.l_orderkey = kit::I64(row, 0, 0);
      input.l_extendedprice = kit::I64(row, 1, 2);
      input.l_discount = kit::I64(row, 2, 2);
      returned_lines.push_back(input);
    });
  }
};

struct ApplyInputs {
  CustomerDeltas customer;
  LineDeltas lineitem;
  NationDeltas nation;
  OrderDeltas orders;

  explicit ApplyInputs(const kit::Changes &changes)
      : customer(changes.at(0)), lineitem(changes.at(1)),
        nation(changes.at(2)), orders(changes.at(3)) {}
};

struct RescanRows {
  std::vector<std::int64_t> orders_keys;
  std::vector<tpchq10vgen_order_input> orders_rows;
  std::vector<std::int64_t> returned_lines_keys;
  std::vector<tpchq10vgen_returned_line_input> returned_lines_rows;
};

std::vector<std::int64_t>
OrderRescanKeys(const tpchq10vgen_state *state, const ApplyInputs &input) {
  std::vector<std::int64_t> keys(
      std::max<std::size_t>(64, input.orders.rows.size()));
  std::size_t required = tpchq10vgen_rescan_orders_for_keys_keys(
      state, input.customer.c_custkey.size(), input.customer.c_custkey.data(),
      input.customer.c_name.bytes.data(), input.customer.c_name.off.data(),
      input.customer.c_address.bytes.data(),
      input.customer.c_address.off.data(), input.customer.c_nationkey.data(),
      input.customer.c_phone.bytes.data(), input.customer.c_phone.off.data(),
      input.customer.c_acctbal.data(), input.customer.c_comment.bytes.data(),
      input.customer.c_comment.off.data(), input.customer.sign.data(),
      input.lineitem.rows.size(), input.lineitem.rows.data(),
      input.nation.n_nationkey.size(), input.nation.n_nationkey.data(),
      input.nation.n_name.bytes.data(), input.nation.n_name.off.data(),
      input.nation.sign.data(), input.orders.rows.size(),
      input.orders.rows.data(), keys.data(), keys.size());
  while (required > keys.size()) {
    keys.resize(required);
    required = tpchq10vgen_rescan_orders_for_keys_keys(
        state, input.customer.c_custkey.size(), input.customer.c_custkey.data(),
        input.customer.c_name.bytes.data(), input.customer.c_name.off.data(),
        input.customer.c_address.bytes.data(),
        input.customer.c_address.off.data(), input.customer.c_nationkey.data(),
        input.customer.c_phone.bytes.data(), input.customer.c_phone.off.data(),
        input.customer.c_acctbal.data(), input.customer.c_comment.bytes.data(),
        input.customer.c_comment.off.data(), input.customer.sign.data(),
        input.lineitem.rows.size(), input.lineitem.rows.data(),
        input.nation.n_nationkey.size(), input.nation.n_nationkey.data(),
        input.nation.n_name.bytes.data(), input.nation.n_name.off.data(),
        input.nation.sign.data(), input.orders.rows.size(),
        input.orders.rows.data(), keys.data(), keys.size());
  }
  keys.resize(required);
  return keys;
}

std::vector<std::int64_t>
ReturnedLineRescanKeys(const tpchq10vgen_state *state,
                       const ApplyInputs &input) {
  std::vector<std::int64_t> keys(
      std::max<std::size_t>(64, input.lineitem.rows.size()));
  std::size_t required = tpchq10vgen_rescan_returned_lines_for_keys_keys(
      state, input.customer.c_custkey.size(), input.customer.c_custkey.data(),
      input.customer.c_name.bytes.data(), input.customer.c_name.off.data(),
      input.customer.c_address.bytes.data(),
      input.customer.c_address.off.data(), input.customer.c_nationkey.data(),
      input.customer.c_phone.bytes.data(), input.customer.c_phone.off.data(),
      input.customer.c_acctbal.data(), input.customer.c_comment.bytes.data(),
      input.customer.c_comment.off.data(), input.customer.sign.data(),
      input.lineitem.rows.size(), input.lineitem.rows.data(),
      input.nation.n_nationkey.size(), input.nation.n_nationkey.data(),
      input.nation.n_name.bytes.data(), input.nation.n_name.off.data(),
      input.nation.sign.data(), input.orders.rows.size(),
      input.orders.rows.data(), keys.data(), keys.size());
  while (required > keys.size()) {
    keys.resize(required);
    required = tpchq10vgen_rescan_returned_lines_for_keys_keys(
        state, input.customer.c_custkey.size(), input.customer.c_custkey.data(),
        input.customer.c_name.bytes.data(), input.customer.c_name.off.data(),
        input.customer.c_address.bytes.data(),
        input.customer.c_address.off.data(), input.customer.c_nationkey.data(),
        input.customer.c_phone.bytes.data(), input.customer.c_phone.off.data(),
        input.customer.c_acctbal.data(), input.customer.c_comment.bytes.data(),
        input.customer.c_comment.off.data(), input.customer.sign.data(),
        input.lineitem.rows.size(), input.lineitem.rows.data(),
        input.nation.n_nationkey.size(), input.nation.n_nationkey.data(),
        input.nation.n_name.bytes.data(), input.nation.n_name.off.data(),
        input.nation.sign.data(), input.orders.rows.size(),
        input.orders.rows.data(), keys.data(), keys.size());
  }
  keys.resize(required);
  return keys;
}

RescanRows FetchRescans(const tpchq10vgen_state *state,
                        const ApplyInputs &input, kit::Host &host) {
  RescanRows rows;
  rows.orders_keys = OrderRescanKeys(state, input);
  rows.orders_rows.reserve(rows.orders_keys.size());
  kit::QueryKeys(
      host, kOrdersRescanSql, "o_orderkey", rows.orders_keys,
      [&rows](const kit::core::AuxRow &row) {
        tpchq10vgen_order_input input{};
        input.o_orderkey = kit::I64(row, 0, 0);
        input.o_custkey = kit::I64(row, 1, 0);
        input.o_orderdate = kit::Date(row, 2);
        rows.orders_rows.push_back(input);
      });

  rows.returned_lines_keys = ReturnedLineRescanKeys(state, input);
  rows.returned_lines_rows.reserve(rows.returned_lines_keys.size());
  kit::QueryKeys(
      host, kReturnedLinesRescanSql, "l_orderkey", rows.returned_lines_keys,
      [&rows](const kit::core::AuxRow &row) {
        tpchq10vgen_returned_line_input input{};
        input.l_orderkey = kit::I64(row, 0, 0);
        input.l_extendedprice = kit::I64(row, 1, 2);
        input.l_discount = kit::I64(row, 2, 2);
        rows.returned_lines_rows.push_back(input);
      });
  return rows;
}

void *Build(kit::Host &host) {
  BuildRows rows(host);
  return tpchq10vgen_build(
      rows.customers.c_custkey.size(), rows.customers.c_custkey.data(),
      rows.customers.c_name.bytes.data(), rows.customers.c_name.off.data(),
      rows.customers.c_address.bytes.data(),
      rows.customers.c_address.off.data(), rows.customers.c_nationkey.data(),
      rows.customers.c_phone.bytes.data(), rows.customers.c_phone.off.data(),
      rows.customers.c_acctbal.data(), rows.customers.c_comment.bytes.data(),
      rows.customers.c_comment.off.data(), rows.nations.n_nationkey.size(),
      rows.nations.n_nationkey.data(), rows.nations.n_name.bytes.data(),
      rows.nations.n_name.off.data(), rows.orders.size(), rows.orders.data(),
      rows.returned_lines.size(), rows.returned_lines.data());
}

void FreeState(void *state) {
  tpchq10vgen_free(static_cast<tpchq10vgen_state *>(state));
}

void *Apply(const void *state, const kit::Changes &changes, kit::Host &host,
            std::int32_t *err) {
  ApplyInputs input(changes);
  RescanRows rescans = FetchRescans(
      static_cast<const tpchq10vgen_state *>(state), input, host);
  return tpchq10vgen_apply(
      static_cast<const tpchq10vgen_state *>(state),
      input.customer.c_custkey.size(), input.customer.c_custkey.data(),
      input.customer.c_name.bytes.data(), input.customer.c_name.off.data(),
      input.customer.c_address.bytes.data(),
      input.customer.c_address.off.data(), input.customer.c_nationkey.data(),
      input.customer.c_phone.bytes.data(), input.customer.c_phone.off.data(),
      input.customer.c_acctbal.data(), input.customer.c_comment.bytes.data(),
      input.customer.c_comment.off.data(), input.customer.sign.data(),
      input.lineitem.rows.size(), input.lineitem.rows.data(),
      input.nation.n_nationkey.size(), input.nation.n_nationkey.data(),
      input.nation.n_name.bytes.data(), input.nation.n_name.off.data(),
      input.nation.sign.data(), input.orders.rows.size(),
      input.orders.rows.data(), rescans.orders_keys.size(),
      rescans.orders_keys.data(), rescans.orders_rows.size(),
      rescans.orders_rows.data(), rescans.returned_lines_keys.size(),
      rescans.returned_lines_keys.data(), rescans.returned_lines_rows.size(),
      rescans.returned_lines_rows.data(), err);
}

bool Execute(const void *state, const kit::core::QueryArguments &args,
             std::vector<kit::core::AuxRow> &out) {
  std::int32_t start_date = 0;
  std::int32_t end_date = 0;
  if (!kit::ArgDate(args, 0, start_date) ||
      !kit::ArgDate(args, 1, end_date)) {
    return false;
  }

  using ResultPtr =
      std::unique_ptr<tpchq10vgen_result, decltype(&tpchq10vgen_result_free)>;
  ResultPtr result(
      tpchq10vgen_execute(static_cast<const tpchq10vgen_state *>(state),
                          start_date, end_date),
      &tpchq10vgen_result_free);
  if (!result) {
    return false;
  }

  const std::size_t len = tpchq10vgen_result_len(result.get());
  out.reserve(out.size() + len);
  for (std::size_t i = 0; i < len; ++i) {
    kit::core::AuxRow row;
    row.values.reserve(8);

    std::int64_t c_custkey = 0;
    if (tpchq10vgen_result_c0(result.get(), i, &c_custkey)) {
      row.values.push_back(kit::OutI64(c_custkey));
    } else {
      row.values.push_back(
          kit::OutNull(kit::core::AuxType::Int64));
    }

    const std::uint8_t *text = nullptr;
    std::size_t text_len = 0;
    if (tpchq10vgen_result_c1(result.get(), i, &text, &text_len)) {
      row.values.push_back(kit::OutText(text, text_len));
    } else {
      row.values.push_back(
          kit::OutNull(kit::core::AuxType::String));
    }

    std::int64_t hi = 0;
    std::uint64_t lo = 0;
    if (tpchq10vgen_result_c2(result.get(), i, &hi, &lo)) {
      row.values.push_back(kit::OutDec(hi, lo, 4));
    } else {
      row.values.push_back(
          kit::OutNull(kit::core::AuxType::ScaledDecimal));
    }

    hi = 0;
    lo = 0;
    if (tpchq10vgen_result_c3(result.get(), i, &hi, &lo)) {
      row.values.push_back(kit::OutDec(hi, lo, 2));
    } else {
      row.values.push_back(
          kit::OutNull(kit::core::AuxType::ScaledDecimal));
    }

    for (std::size_t column = 4; column < 8; ++column) {
      text = nullptr;
      text_len = 0;
      const std::uint8_t present =
          column == 4   ? tpchq10vgen_result_c4(result.get(), i, &text,
                                                &text_len)
          : column == 5 ? tpchq10vgen_result_c5(result.get(), i, &text,
                                                &text_len)
          : column == 6 ? tpchq10vgen_result_c6(result.get(), i, &text,
                                                &text_len)
                        : tpchq10vgen_result_c7(result.get(), i, &text,
                                                &text_len);
      if (present) {
        row.values.push_back(kit::OutText(text, text_len));
      } else {
        row.values.push_back(
            kit::OutNull(kit::core::AuxType::String));
      }
    }
    out.push_back(std::move(row));
  }
  return true;
}

void *PrepareApply(const void *store, const void *base,
                   const kit::Changes &changes, kit::Host &host,
                   std::int32_t *err) {
  auto *base_handle = static_cast<const tpchq10vgens_base *>(base);
  const tpchq10vgen_state *base_state =
      tpchq10vgens_base_state(base_handle);
  ApplyInputs input(changes);
  RescanRows rescans = FetchRescans(base_state, input, host);
  return tpchq10vgens_prepare_apply(
      static_cast<const tpchq10vgens_store *>(store), base_handle,
      input.customer.c_custkey.size(), input.customer.c_custkey.data(),
      input.customer.c_name.bytes.data(), input.customer.c_name.off.data(),
      input.customer.c_address.bytes.data(),
      input.customer.c_address.off.data(), input.customer.c_nationkey.data(),
      input.customer.c_phone.bytes.data(), input.customer.c_phone.off.data(),
      input.customer.c_acctbal.data(), input.customer.c_comment.bytes.data(),
      input.customer.c_comment.off.data(), input.customer.sign.data(),
      input.lineitem.rows.size(), input.lineitem.rows.data(),
      input.nation.n_nationkey.size(), input.nation.n_nationkey.data(),
      input.nation.n_name.bytes.data(), input.nation.n_name.off.data(),
      input.nation.sign.data(), input.orders.rows.size(),
      input.orders.rows.data(), rescans.orders_keys.size(),
      rescans.orders_keys.data(), rescans.orders_rows.size(),
      rescans.orders_rows.data(), rescans.returned_lines_keys.size(),
      rescans.returned_lines_keys.data(), rescans.returned_lines_rows.size(),
      rescans.returned_lines_rows.data(), err);
}

void *PrepareBuild(const void *store, kit::Host &host) {
  BuildRows rows(host);
  return tpchq10vgens_prepare_build(
      static_cast<const tpchq10vgens_store *>(store),
      rows.customers.c_custkey.size(), rows.customers.c_custkey.data(),
      rows.customers.c_name.bytes.data(), rows.customers.c_name.off.data(),
      rows.customers.c_address.bytes.data(),
      rows.customers.c_address.off.data(), rows.customers.c_nationkey.data(),
      rows.customers.c_phone.bytes.data(), rows.customers.c_phone.off.data(),
      rows.customers.c_acctbal.data(), rows.customers.c_comment.bytes.data(),
      rows.customers.c_comment.off.data(), rows.nations.n_nationkey.size(),
      rows.nations.n_nationkey.data(), rows.nations.n_name.bytes.data(),
      rows.nations.n_name.off.data(), rows.orders.size(), rows.orders.data(),
      rows.returned_lines.size(), rows.returned_lines.data());
}

void *NewStore() { return tpchq10vgens_new(); }
void FreeStore(void *store) {
  tpchq10vgens_free(static_cast<tpchq10vgens_store *>(store));
}
void *NewestBase(const void *store) {
  return tpchq10vgens_newest_base(
      static_cast<const tpchq10vgens_store *>(store));
}
const void *BaseState(const void *base) {
  return tpchq10vgens_base_state(
      static_cast<const tpchq10vgens_base *>(base));
}
void BaseFree(void *base) {
  tpchq10vgens_base_free(static_cast<tpchq10vgens_base *>(base));
}
void *PrepareInvalid(const void *store) {
  return tpchq10vgens_prepare_invalid(
      static_cast<const tpchq10vgens_store *>(store));
}
void PreparedFree(void *prepared) {
  tpchq10vgens_prepared_free(
      static_cast<tpchq10vgens_prepared *>(prepared));
}
std::int32_t Begin(const void *store, std::uint64_t from) {
  return tpchq10vgens_begin(static_cast<const tpchq10vgens_store *>(store),
                            from);
}
void Abort(const void *store) {
  tpchq10vgens_abort(static_cast<const tpchq10vgens_store *>(store));
}
std::int32_t Install(const void *store, void *prepared, std::uint64_t from,
                     std::uint64_t floor, std::int32_t end_commit) {
  return tpchq10vgens_install(
      static_cast<const tpchq10vgens_store *>(store),
      static_cast<tpchq10vgens_prepared *>(prepared), from, floor, end_commit);
}
const void *Read(const void *store, std::uint64_t at, std::uint64_t tries,
                 std::int32_t *status) {
  return tpchq10vgens_read(static_cast<const tpchq10vgens_store *>(store), at,
                           tries, status);
}
void StateRelease(const void *state) {
  tpchq10vgens_state_release(
      static_cast<const tpchq10vgen_state *>(state));
}
void InvalidateAll(const void *store) {
  tpchq10vgens_invalidate_all(
      static_cast<const tpchq10vgens_store *>(store));
}
std::size_t VersionCount(const void *store) {
  return tpchq10vgens_version_count(
      static_cast<const tpchq10vgens_store *>(store));
}

const kit::Glue kGlue = {
    "tpch_q10_vgen",
    "tpch",
    10,
    kQuerySql,
    {
        {"customer",
         {"c_custkey", "c_name", "c_address", "c_nationkey", "c_phone",
          "c_acctbal", "c_comment"}},
        {"lineitem",
         {"l_orderkey", "l_extendedprice", "l_discount", "l_returnflag"}},
        {"nation", {"n_nationkey", "n_name"}},
        {"orders", {"o_orderkey", "o_custkey", "o_orderdate"}},
    },
    {kit::Kind::Date, kit::Kind::Date},
    Build,
    FreeState,
    Apply,
    Execute,
    PrepareApply,
    PrepareBuild,
    kit::StoreOps{
        NewStore,
        FreeStore,
        NewestBase,
        BaseState,
        BaseFree,
        PrepareInvalid,
        PreparedFree,
        Begin,
        Abort,
        Install,
        Read,
        StateRelease,
        InvalidateAll,
        VersionCount,
    },
};

const kit::Registrar kRegistrar(&kGlue);

} // namespace
