#include "tpch_q4_vgen_verus.h"
#include "substrate/verus/kit/verus_kit.hpp"

#include <memory>
#include <utility>
#include <vector>

namespace {

namespace kit = accelerator_codegen::verus_kit;
namespace core = accelerator_codegen::aux_state::core;

const char kQuerySql[] = R"SQL(-- TPC-H Q4 (catalog text), :start_date as $1, $2 = $1 + 3 months (derived end date: sql2verus cannot add months to a non-literal date).
SELECT o_orderpriority, COUNT(o_orderkey) AS order_count
FROM orders
WHERE o_orderdate >= $1
  AND o_orderdate < $2
  AND EXISTS (SELECT * FROM lineitem WHERE l_orderkey = o_orderkey AND l_commitdate < l_receiptdate)
GROUP BY o_orderpriority
ORDER BY o_orderpriority
)SQL";

const char kQualifyingOrdersSql[] = R"SQL(  SELECT o_orderdate, o_orderpriority
  FROM orders
  WHERE EXISTS (
    SELECT * FROM lineitem
    WHERE l_orderkey = o_orderkey AND l_commitdate < l_receiptdate
  );)SQL";

const char kOrdersForLineChangesSql[] =
    "  SELECT o_orderkey, o_orderdate, o_orderpriority FROM orders;";
const char kQualifyingLinesForKeysSql[] =
    "  SELECT l_orderkey FROM lineitem WHERE l_commitdate < l_receiptdate;";

struct DatePriorityRows {
  std::vector<int32_t> dates;
  kit::TextColumn priorities;

  void Reserve(std::size_t count) {
    dates.reserve(count);
    priorities.off.reserve(count + 1);
  }

  void Push(const core::AuxRow &row, std::size_t first) {
    dates.push_back(kit::Date(row, first));
    priorities.Push(kit::Text(row, first + 1));
  }
};

struct OrderRows {
  std::vector<int64_t> keys;
  DatePriorityRows values;

  void Reserve(std::size_t count) {
    keys.reserve(count);
    values.Reserve(count);
  }

  void Push(const core::AuxRow &row) {
    keys.push_back(kit::I64(row, 0, 0));
    values.Push(row, 1);
  }
};

DatePriorityRows Scan(kit::Host &host) {
  DatePriorityRows rows;
  host.Query(kQualifyingOrdersSql, [&](const core::AuxRow &row) {
    rows.Push(row, 0);
  });
  return rows;
}

void *Build(kit::Host &host) {
  const auto rows = Scan(host);
  return tpchq4vgen_build(rows.dates.size(), rows.dates.data(),
                         rows.priorities.bytes.data(), rows.priorities.off.data());
}

void *PrepareBuild(const kit::HeadFn &head_of, kit::Host &host) {
  const auto rows = Scan(host);
  return tpchq4vgens_stage_build(
      static_cast<const tpchq4vgens_head *>(head_of()), rows.dates.size(),
      rows.dates.data(), rows.priorities.bytes.data(), rows.priorities.off.data());
}

void FreeState(void *state) {
  tpchq4vgen_free(static_cast<tpchq4vgen_state *>(state));
}

struct Deltas {
  std::vector<tpchq4vgen_line_delta> lines;
  OrderRows orders;
  std::vector<int64_t> order_signs;

  explicit Deltas(const kit::Changes &changes) {
    const auto &order_changes = changes.at(0);
    const auto &line_changes = changes.at(1);
    orders.Reserve(order_changes.size());
    order_signs.reserve(order_changes.size());
    lines.reserve(line_changes.size());
    for (const auto &change : order_changes) {
      orders.Push(change.row);
      order_signs.push_back(change.sign);
    }
    for (const auto &change : line_changes) {
      lines.push_back({{kit::I64(change.row, 0, 0), kit::Date(change.row, 1),
                        kit::Date(change.row, 2)},
                       change.sign});
    }
  }
};

std::vector<int64_t> RescanOrderKeys(const tpchq4vgen_state *state,
                                     const Deltas &deltas) {
  std::vector<int64_t> keys(deltas.lines.size());
  for (;;) {
    const auto count = tpchq4vgen_rescan_orders_for_line_changes_keys(
        state, deltas.lines.size(), deltas.lines.data(), deltas.orders.keys.size(),
        deltas.orders.keys.data(), deltas.orders.values.dates.data(),
        deltas.orders.values.priorities.bytes.data(),
        deltas.orders.values.priorities.off.data(), deltas.order_signs.data(),
        keys.data(), keys.size());
    if (count <= keys.size()) {
      keys.resize(count);
      return keys;
    }
    keys.resize(count);
  }
}

std::vector<int64_t> RescanLineKeys(const tpchq4vgen_state *state,
                                    const Deltas &deltas) {
  std::vector<int64_t> keys(deltas.lines.size() + deltas.orders.keys.size());
  for (;;) {
    const auto count = tpchq4vgen_rescan_qualifying_lines_for_keys_keys(
        state, deltas.lines.size(), deltas.lines.data(), deltas.orders.keys.size(),
        deltas.orders.keys.data(), deltas.orders.values.dates.data(),
        deltas.orders.values.priorities.bytes.data(),
        deltas.orders.values.priorities.off.data(), deltas.order_signs.data(),
        keys.data(), keys.size());
    if (count <= keys.size()) {
      keys.resize(count);
      return keys;
    }
    keys.resize(count);
  }
}

struct ApplyInputs {
  Deltas deltas;
  std::vector<int64_t> order_keys;
  OrderRows orders;
  std::vector<int64_t> line_keys;
  std::vector<tpchq4vgen_qualifying_line_key_input> lines;

  ApplyInputs(const tpchq4vgen_state *state, const kit::Changes &changes,
              kit::Host &host)
      : deltas(changes) {
    order_keys = RescanOrderKeys(state, deltas);
    orders.Reserve(order_keys.size());
    kit::QueryKeys(host, kOrdersForLineChangesSql, "o_orderkey", order_keys,
                   [&](const core::AuxRow &row) { orders.Push(row); });
    line_keys = RescanLineKeys(state, deltas);
    lines.reserve(line_keys.size());
    kit::QueryKeys(host, kQualifyingLinesForKeysSql, "l_orderkey", line_keys,
                   [&](const core::AuxRow &row) {
                     lines.push_back({kit::I64(row, 0, 0)});
                   });
  }
};

void *Apply(const void *state, const kit::Changes &changes, kit::Host &host,
            int32_t *err) {
  const auto *typed_state = static_cast<const tpchq4vgen_state *>(state);
  const ApplyInputs inputs(typed_state, changes, host);
  const auto &deltas = inputs.deltas;
  return tpchq4vgen_apply(
      typed_state, deltas.lines.size(), deltas.lines.data(),
      deltas.orders.keys.size(), deltas.orders.keys.data(),
      deltas.orders.values.dates.data(),
      deltas.orders.values.priorities.bytes.data(),
      deltas.orders.values.priorities.off.data(), deltas.order_signs.data(),
      inputs.order_keys.size(), inputs.order_keys.data(),
      inputs.orders.keys.size(), inputs.orders.keys.data(),
      inputs.orders.values.dates.data(), inputs.orders.values.priorities.bytes.data(),
      inputs.orders.values.priorities.off.data(), inputs.line_keys.size(),
      inputs.line_keys.data(), inputs.lines.size(), inputs.lines.data(), err);
}

void *PrepareApply(const void *base,
                   const kit::Changes &changes, kit::Host &host, int32_t *err) {
  const auto *typed_base = static_cast<const tpchq4vgens_base *>(base);
  const ApplyInputs inputs(tpchq4vgens_base_state(typed_base), changes, host);
  const auto &deltas = inputs.deltas;
  return tpchq4vgens_stage_apply(
      typed_base,
      deltas.lines.size(), deltas.lines.data(), deltas.orders.keys.size(),
      deltas.orders.keys.data(), deltas.orders.values.dates.data(),
      deltas.orders.values.priorities.bytes.data(),
      deltas.orders.values.priorities.off.data(), deltas.order_signs.data(),
      inputs.order_keys.size(), inputs.order_keys.data(),
      inputs.orders.keys.size(), inputs.orders.keys.data(),
      inputs.orders.values.dates.data(), inputs.orders.values.priorities.bytes.data(),
      inputs.orders.values.priorities.off.data(), inputs.line_keys.size(),
      inputs.line_keys.data(), inputs.lines.size(), inputs.lines.data(), err);
}

template <class Call>
bool Run(const kit::core::QueryArguments &args, std::vector<kit::core::AuxRow> &out, Call call) {
  int32_t start_date;
  int32_t end_date;
  if (!kit::ArgDate(args, 0, start_date) || !kit::ArgDate(args, 1, end_date)) {
    return false;
  }
  std::unique_ptr<tpchq4vgen_result, decltype(&tpchq4vgen_result_free)> result(
      call(start_date, end_date),
      tpchq4vgen_result_free);
  if (!result) {
    return false;
  }
  const auto count = tpchq4vgen_result_len(result.get());
  out.reserve(out.size() + count);
  for (std::size_t index = 0; index < count; ++index) {
    const uint8_t *priority = nullptr;
    std::size_t priority_len = 0;
    int64_t order_count = 0;
    core::AuxRow row;
    row.values.reserve(2);
    if (tpchq4vgen_result_c0(result.get(), index, &priority, &priority_len)) {
      row.values.push_back(kit::OutText(priority, priority_len));
    } else {
      row.values.push_back(kit::OutNull(core::AuxType::String));
    }
    if (tpchq4vgen_result_c1(result.get(), index, &order_count)) {
      row.values.push_back(kit::OutI64(order_count));
    } else {
      row.values.push_back(kit::OutNull(core::AuxType::Int64));
    }
    out.push_back(std::move(row));
  }
  return true;
}

bool Execute(const void *state, const kit::core::QueryArguments &args,
             std::vector<kit::core::AuxRow> &out) {
  return Run(args, out, [&](auto... x) {
    return tpchq4vgen_execute(static_cast<const tpchq4vgen_state *>(state), x...);
  });
}

bool Answer(const void *state, const kit::core::QueryArguments &args,
            std::vector<kit::core::AuxRow> &out) {
  return Run(args, out, [&](auto... x) {
    return tpchq4vgens_answer(static_cast<const tpchq4vgens_reading *>(state), x...);
  });
}

const kit::Glue kGlue = {
    "tpch_q4_vgen", "tpch", 4, kQuerySql,
    {{"orders", {"o_orderkey", "o_orderdate", "o_orderpriority"}},
     {"lineitem", {"l_orderkey", "l_commitdate", "l_receiptdate"}}},
    {kit::Kind::Date, kit::Kind::Date},
    Build, FreeState, Apply, Execute, Answer, PrepareApply, PrepareBuild,
    kit::StoreOps{
        []() -> void * { return tpchq4vgens_new(); },
        [](void *store) { tpchq4vgens_free(static_cast<tpchq4vgens_store *>(store)); },
        [](const void *store) -> void * { return tpchq4vgens_newest_base(static_cast<const tpchq4vgens_store *>(store)); },
        [](const void *base) -> const void * { return tpchq4vgens_base_state(static_cast<const tpchq4vgens_base *>(base)); },
        [](void *base) { tpchq4vgens_base_free(static_cast<tpchq4vgens_base *>(base)); },
        [](const void *store) -> void * { return tpchq4vgens_newest_head(static_cast<const tpchq4vgens_store *>(store)); },
        [](void *head) { tpchq4vgens_head_free(static_cast<tpchq4vgens_head *>(head)); },
        [](const void *head) -> void * { return tpchq4vgens_stage_invalid(static_cast<const tpchq4vgens_head *>(head)); },
        [](void *staged) { tpchq4vgens_staged_free(static_cast<tpchq4vgens_staged *>(staged)); },
        [](const void *store, uint64_t from) -> int32_t { return tpchq4vgens_begin(static_cast<const tpchq4vgens_store *>(store), from); },
        [](const void *store) { tpchq4vgens_abort(static_cast<const tpchq4vgens_store *>(store)); },
        [](const void *store, void *staged, uint64_t from, uint64_t floor, int32_t end_commit) -> int32_t {
            return tpchq4vgens_install(static_cast<const tpchq4vgens_store *>(store), static_cast<tpchq4vgens_staged *>(staged), from, floor, end_commit);
        },
        [](const void *store, uint64_t at, uint64_t tries, int32_t *status) -> const void * {
            return tpchq4vgens_read(static_cast<const tpchq4vgens_store *>(store), at, tries, status);
        },
        [](const void *reading) { tpchq4vgens_reading_free(const_cast<tpchq4vgens_reading *>(static_cast<const tpchq4vgens_reading *>(reading))); },
        [](const void *store) { tpchq4vgens_invalidate_all(static_cast<const tpchq4vgens_store *>(store)); },
        [](const void *store) -> std::size_t { return tpchq4vgens_version_count(static_cast<const tpchq4vgens_store *>(store)); },
        []() -> std::size_t { return tpchq4vgens_heap_bytes(); },
        []() -> std::size_t { return tpchq4vgens_heap_peak_bytes(); },
        []() { tpchq4vgens_heap_reset_peak(); },
        [](const void *head, void *state) -> void * { return tpchq4vgens_stage_adopt(static_cast<const tpchq4vgens_head *>(head), static_cast<tpchq4vgen_state *>(state)); },
    },
};

const kit::Registrar kRegistrar(&kGlue);

}
