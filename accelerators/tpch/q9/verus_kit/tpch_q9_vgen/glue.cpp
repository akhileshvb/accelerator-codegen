#include "tpch_q9_vgen_verus.h"
#include "substrate/verus/kit/verus_kit.hpp"

#include <algorithm>
#include <memory>
#include <utility>

namespace {

namespace kit = accelerator_codegen::verus_kit;
namespace core = accelerator_codegen::aux_state::core;

const char kQuerySql[] = R"SQL(-- TPC-H Q9 (catalog text), :color_pattern ('%' || :color || '%', e.g. '%green%') as $1 (a text LIKE pattern parameter).
SELECT nation, o_year, SUM(amount) AS sum_profit
FROM (SELECT n_name AS nation, EXTRACT(year FROM o_orderdate) AS o_year,
             l_extendedprice * (1 - l_discount) - ps_supplycost * l_quantity AS amount
      FROM part, supplier, lineitem, partsupp, orders, nation
      WHERE s_suppkey = l_suppkey
        AND ps_suppkey = l_suppkey
        AND ps_partkey = l_partkey
        AND p_partkey = l_partkey
        AND o_orderkey = l_orderkey
        AND s_nationkey = n_nationkey
        AND p_name LIKE $1) AS profit
GROUP BY nation, o_year
ORDER BY nation, o_year DESC
)SQL";

const char kAllPartsSql[] = "SELECT p_partkey, p_name FROM part;";
const char kOrderDatesSql[] = "SELECT o_orderkey, o_orderdate FROM orders;";
const char kSupplierNationsSql[] = "SELECT s_suppkey, s_nationkey FROM supplier;";
const char kNationNamesSql[] = "SELECT n_nationkey, n_name FROM nation;";
const char kActivePartsuppSql[] = "SELECT ps_partkey, ps_suppkey, ps_supplycost FROM partsupp;";
const char kActiveLinesSql[] = R"SQL(SELECT l_orderkey, l_partkey, l_suppkey,
         l_quantity, l_extendedprice, l_discount FROM lineitem;)SQL";
const char kLinesSql[] = R"SQL(SELECT l_orderkey, l_partkey, l_suppkey,
         l_quantity, l_extendedprice, l_discount FROM lineitem;)SQL";
const char kPartsuppSql[] = "SELECT ps_partkey, ps_suppkey, ps_supplycost FROM partsupp;";
const char kOrdersSql[] = "SELECT o_orderkey, o_orderdate FROM orders;";

tpchq9vgen_order_in ReadOrder(const core::AuxRow &row) {
  return {kit::I64(row, 0, 0), kit::Date(row, 1)};
}

tpchq9vgen_supplier_in ReadSupplier(const core::AuxRow &row) {
  return {kit::I64(row, 0, 0), kit::I64(row, 1, 0)};
}

tpchq9vgen_partsupp_in ReadPartsupp(const core::AuxRow &row) {
  return {kit::I64(row, 0, 0), kit::I64(row, 1, 0), kit::I64(row, 2, 2)};
}

tpchq9vgen_line_in ReadLine(const core::AuxRow &row) {
  return {kit::I64(row, 0, 0), kit::I64(row, 1, 0), kit::I64(row, 2, 0),
          kit::I64(row, 3, 2), kit::I64(row, 4, 2), kit::I64(row, 5, 2)};
}

struct NationRows {
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
  NationRows all_parts;
  std::vector<tpchq9vgen_order_in> order_dates;
  std::vector<tpchq9vgen_supplier_in> supplier_nations;
  NationRows nation_names;
  std::vector<tpchq9vgen_partsupp_in> active_partsupp;
  std::vector<tpchq9vgen_line_in> active_lines;

  explicit BuildInputs(kit::Host &host) {
    host.Query(kAllPartsSql, [&](const core::AuxRow &row) {
      all_parts.Push(row);
    });
    host.Query(kOrderDatesSql, [&](const core::AuxRow &row) {
      order_dates.push_back(ReadOrder(row));
    });
    host.Query(kSupplierNationsSql, [&](const core::AuxRow &row) {
      supplier_nations.push_back(ReadSupplier(row));
    });
    host.Query(kNationNamesSql, [&](const core::AuxRow &row) {
      nation_names.Push(row);
    });
    host.Query(kActivePartsuppSql, [&](const core::AuxRow &row) {
      active_partsupp.push_back(ReadPartsupp(row));
    });
    host.Query(kActiveLinesSql, [&](const core::AuxRow &row) {
      active_lines.push_back(ReadLine(row));
    });
  }

  template <typename Function, typename... Prefix>
  auto Call(Function function, Prefix... prefix) const {
    return function(prefix...,
                    all_parts.keys.size(), all_parts.keys.data(),
                    all_parts.names.bytes.data(), all_parts.names.off.data(),
                    order_dates.size(), order_dates.data(),
                    supplier_nations.size(), supplier_nations.data(),
                    nation_names.keys.size(), nation_names.keys.data(),
                    nation_names.names.bytes.data(), nation_names.names.off.data(),
                    active_partsupp.size(), active_partsupp.data(),
                    active_lines.size(), active_lines.data());
  }
};

struct DeltaInputs {
  NationRows part;
  std::vector<int64_t> part_signs;
  std::vector<tpchq9vgen_order_delta> orders;
  std::vector<tpchq9vgen_supplier_delta> supplier;
  NationRows nation;
  std::vector<int64_t> nation_signs;
  std::vector<tpchq9vgen_partsupp_delta> partsupp;
  std::vector<tpchq9vgen_line_delta> lineitem;

  explicit DeltaInputs(const kit::Changes &changes) {
    part.Reserve(changes.at(0).size());
    part_signs.reserve(changes.at(0).size());
    for (const auto &change : changes.at(0)) {
      part.Push(change.row);
      part_signs.push_back(change.sign);
    }
    orders.reserve(changes.at(1).size());
    for (const auto &change : changes.at(1)) {
      orders.push_back({ReadOrder(change.row), change.sign});
    }
    supplier.reserve(changes.at(2).size());
    for (const auto &change : changes.at(2)) {
      supplier.push_back({ReadSupplier(change.row), change.sign});
    }
    nation.Reserve(changes.at(3).size());
    nation_signs.reserve(changes.at(3).size());
    for (const auto &change : changes.at(3)) {
      nation.Push(change.row);
      nation_signs.push_back(change.sign);
    }
    partsupp.reserve(changes.at(4).size());
    for (const auto &change : changes.at(4)) {
      partsupp.push_back({ReadPartsupp(change.row), change.sign});
    }
    lineitem.reserve(changes.at(5).size());
    for (const auto &change : changes.at(5)) {
      lineitem.push_back({ReadLine(change.row), change.sign});
    }
  }

  template <typename Function>
  std::vector<int64_t> Keys(Function function, const tpchq9vgen_state *state) const {
    std::vector<int64_t> keys;
    for (;;) {
      const std::size_t count = function(
          state, lineitem.size(), lineitem.data(),
          nation.keys.size(), nation.keys.data(), nation.names.bytes.data(),
          nation.names.off.data(), nation_signs.data(),
          orders.size(), orders.data(), part.keys.size(), part.keys.data(),
          part.names.bytes.data(), part.names.off.data(), part_signs.data(),
          partsupp.size(), partsupp.data(), supplier.size(), supplier.data(),
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
  DeltaInputs delta;
  std::vector<int64_t> lines_for_orders_keys;
  std::vector<tpchq9vgen_line_in> lines_for_orders_rows;
  std::vector<int64_t> lines_for_parts_keys;
  std::vector<tpchq9vgen_line_in> lines_for_parts_rows;
  std::vector<int64_t> lines_for_suppliers_keys;
  std::vector<tpchq9vgen_line_in> lines_for_suppliers_rows;
  std::vector<int64_t> partsupp_for_parts_keys;
  std::vector<tpchq9vgen_partsupp_in> partsupp_for_parts_rows;
  std::vector<int64_t> orders_for_keys_keys;
  std::vector<tpchq9vgen_order_in> orders_for_keys_rows;

  ApplyInputs(const tpchq9vgen_state *state, const kit::Changes &changes, kit::Host &host)
      : delta(changes) {
    lines_for_orders_keys = delta.Keys(tpchq9vgen_rescan_lines_for_orders_keys, state);
    lines_for_parts_keys = delta.Keys(tpchq9vgen_rescan_lines_for_parts_keys, state);
    lines_for_suppliers_keys = delta.Keys(tpchq9vgen_rescan_lines_for_suppliers_keys, state);
    partsupp_for_parts_keys = delta.Keys(tpchq9vgen_rescan_partsupp_for_parts_keys, state);
    orders_for_keys_keys = delta.Keys(tpchq9vgen_rescan_orders_for_keys_keys, state);

    kit::QueryKeys(host, kLinesSql, "l_orderkey", lines_for_orders_keys,
                   [&](const core::AuxRow &row) {
      lines_for_orders_rows.push_back(ReadLine(row));
    });
    kit::QueryKeys(host, kLinesSql, "l_partkey", lines_for_parts_keys,
                   [&](const core::AuxRow &row) {
      lines_for_parts_rows.push_back(ReadLine(row));
    });
    kit::QueryKeys(host, kLinesSql, "l_suppkey", lines_for_suppliers_keys,
                   [&](const core::AuxRow &row) {
      lines_for_suppliers_rows.push_back(ReadLine(row));
    });
    kit::QueryKeys(host, kPartsuppSql, "ps_partkey", partsupp_for_parts_keys,
                   [&](const core::AuxRow &row) {
      partsupp_for_parts_rows.push_back(ReadPartsupp(row));
    });

    for (const auto *rows : {&lines_for_orders_rows, &lines_for_parts_rows, &lines_for_suppliers_rows}) {
      for (const auto &line : *rows) {
        orders_for_keys_keys.push_back(line.l_orderkey);
      }
    }
    std::sort(orders_for_keys_keys.begin(), orders_for_keys_keys.end());
    orders_for_keys_keys.erase(
        std::unique(orders_for_keys_keys.begin(), orders_for_keys_keys.end()),
        orders_for_keys_keys.end());
    orders_for_keys_rows.reserve(orders_for_keys_keys.size());
    kit::QueryKeys(host, kOrdersSql, "o_orderkey", orders_for_keys_keys,
                   [&](const core::AuxRow &row) {
      orders_for_keys_rows.push_back(ReadOrder(row));
    });
  }

  template <typename Function, typename... Prefix>
  auto Call(Function function, int32_t *err, Prefix... prefix) const {
    return function(
        prefix..., delta.lineitem.size(), delta.lineitem.data(),
        delta.nation.keys.size(), delta.nation.keys.data(), delta.nation.names.bytes.data(),
        delta.nation.names.off.data(), delta.nation_signs.data(),
        delta.orders.size(), delta.orders.data(), delta.part.keys.size(), delta.part.keys.data(),
        delta.part.names.bytes.data(), delta.part.names.off.data(), delta.part_signs.data(),
        delta.partsupp.size(), delta.partsupp.data(), delta.supplier.size(), delta.supplier.data(),
        lines_for_orders_keys.size(), lines_for_orders_keys.data(),
        lines_for_orders_rows.size(), lines_for_orders_rows.data(),
        lines_for_parts_keys.size(), lines_for_parts_keys.data(),
        lines_for_parts_rows.size(), lines_for_parts_rows.data(),
        lines_for_suppliers_keys.size(), lines_for_suppliers_keys.data(),
        lines_for_suppliers_rows.size(), lines_for_suppliers_rows.data(),
        partsupp_for_parts_keys.size(), partsupp_for_parts_keys.data(),
        partsupp_for_parts_rows.size(), partsupp_for_parts_rows.data(),
        orders_for_keys_keys.size(), orders_for_keys_keys.data(),
        orders_for_keys_rows.size(), orders_for_keys_rows.data(), err);
  }
};

void *Build(kit::Host &host) {
  return BuildInputs(host).Call(tpchq9vgen_build);
}

void FreeState(void *state) {
  tpchq9vgen_free(static_cast<tpchq9vgen_state *>(state));
}

void *Apply(const void *state, const kit::Changes &changes, kit::Host &host, int32_t *err) {
  const auto *typed_state = static_cast<const tpchq9vgen_state *>(state);
  return ApplyInputs(typed_state, changes, host).Call(tpchq9vgen_apply, err, typed_state);
}

void *PrepareBuild(const kit::HeadFn &head_of, kit::Host &host) {
  return BuildInputs(host).Call(tpchq9vgens_stage_build,
                                static_cast<const tpchq9vgens_head *>(head_of()));
}

void *PrepareApply(const void *base, const kit::Changes &changes,
                   kit::Host &host, int32_t *err) {
  const auto *typed_base = static_cast<const tpchq9vgens_base *>(base);
  return ApplyInputs(tpchq9vgens_base_state(typed_base), changes, host).Call(
      tpchq9vgens_stage_apply, err, typed_base);
}

template <class Call>
bool Run(const kit::core::QueryArguments &args, std::vector<kit::core::AuxRow> &out, Call call) {
  std::string pattern;
  if (!kit::ArgText(args, 0, pattern)) {
    return false;
  }
  std::unique_ptr<tpchq9vgen_result, decltype(&tpchq9vgen_result_free)> result(
      call(reinterpret_cast<const uint8_t *>(pattern.data()), pattern.size()),
      tpchq9vgen_result_free);
  if (!result) {
    return false;
  }
  const std::size_t count = tpchq9vgen_result_len(result.get());
  out.reserve(out.size() + count);
  for (std::size_t index = 0; index < count; ++index) {
    core::AuxRow row;
    row.values.reserve(3);
    const uint8_t *text = nullptr;
    std::size_t text_len = 0;
    int64_t year = 0;
    int64_t hi = 0;
    uint64_t lo = 0;
    row.values.push_back(tpchq9vgen_result_c0(result.get(), index, &text, &text_len)
                             ? kit::OutText(text, text_len)
                             : kit::OutNull(core::AuxType::String));
    row.values.push_back(tpchq9vgen_result_c1(result.get(), index, &year)
                             ? kit::OutI64(year)
                             : kit::OutNull(core::AuxType::Int64));
    row.values.push_back(tpchq9vgen_result_c2(result.get(), index, &hi, &lo)
                             ? kit::OutDec(hi, lo, 4)
                             : kit::OutNull(core::AuxType::ScaledDecimal));
    out.push_back(std::move(row));
  }
  return true;
}

bool Execute(const void *state, const kit::core::QueryArguments &args,
             std::vector<kit::core::AuxRow> &out) {
  return Run(args, out, [&](auto... x) {
    return tpchq9vgen_execute(static_cast<const tpchq9vgen_state *>(state), x...);
  });
}

bool Answer(const void *state, const kit::core::QueryArguments &args,
            std::vector<kit::core::AuxRow> &out) {
  return Run(args, out, [&](auto... x) {
    return tpchq9vgens_answer(static_cast<const tpchq9vgens_reading *>(state), x...);
  });
}

const kit::Glue kGlue = {
    "tpch_q9_vgen", "tpch", 9, kQuerySql,
    {{"part", {"p_partkey", "p_name"}},
     {"orders", {"o_orderkey", "o_orderdate"}},
     {"supplier", {"s_suppkey", "s_nationkey"}},
     {"nation", {"n_nationkey", "n_name"}},
     {"partsupp", {"ps_partkey", "ps_suppkey", "ps_supplycost"}},
     {"lineitem", {"l_orderkey", "l_partkey", "l_suppkey", "l_quantity", "l_extendedprice", "l_discount"}}},
    {kit::Kind::Text},
    Build, FreeState, Apply, Execute, Answer, PrepareApply, PrepareBuild,
    kit::StoreOps{
        []() -> void * { return tpchq9vgens_new(); },
        [](void *store) { tpchq9vgens_free(static_cast<tpchq9vgens_store *>(store)); },
        [](const void *store) -> void * { return tpchq9vgens_newest_base(static_cast<const tpchq9vgens_store *>(store)); },
        [](const void *base) -> const void * { return tpchq9vgens_base_state(static_cast<const tpchq9vgens_base *>(base)); },
        [](void *base) { tpchq9vgens_base_free(static_cast<tpchq9vgens_base *>(base)); },
        [](const void *store) -> void * { return tpchq9vgens_newest_head(static_cast<const tpchq9vgens_store *>(store)); },
        [](void *head) { tpchq9vgens_head_free(static_cast<tpchq9vgens_head *>(head)); },
        [](const void *head) -> void * { return tpchq9vgens_stage_invalid(static_cast<const tpchq9vgens_head *>(head)); },
        [](void *staged) { tpchq9vgens_staged_free(static_cast<tpchq9vgens_staged *>(staged)); },
        [](const void *store, uint64_t from) -> int32_t { return tpchq9vgens_begin(static_cast<const tpchq9vgens_store *>(store), from); },
        [](const void *store) { tpchq9vgens_abort(static_cast<const tpchq9vgens_store *>(store)); },
        [](const void *store, void *staged, uint64_t from, uint64_t floor, int32_t end_commit) -> int32_t {
            return tpchq9vgens_install(static_cast<const tpchq9vgens_store *>(store), static_cast<tpchq9vgens_staged *>(staged), from, floor, end_commit);
        },
        [](const void *store, uint64_t at, uint64_t tries, int32_t *status) -> const void * {
            return tpchq9vgens_read(static_cast<const tpchq9vgens_store *>(store), at, tries, status);
        },
        [](const void *reading) { tpchq9vgens_reading_free(const_cast<tpchq9vgens_reading *>(static_cast<const tpchq9vgens_reading *>(reading))); },
        [](const void *store) { tpchq9vgens_invalidate_all(static_cast<const tpchq9vgens_store *>(store)); },
        [](const void *store) -> std::size_t { return tpchq9vgens_version_count(static_cast<const tpchq9vgens_store *>(store)); },
        []() -> std::size_t { return tpchq9vgens_heap_bytes(); },
        []() -> std::size_t { return tpchq9vgens_heap_peak_bytes(); },
        []() { tpchq9vgens_heap_reset_peak(); },
        [](const void *head, void *state) -> void * { return tpchq9vgens_stage_adopt(static_cast<const tpchq9vgens_head *>(head), static_cast<tpchq9vgen_state *>(state)); },
    }};

const kit::Registrar kRegistrar(&kGlue);

}
