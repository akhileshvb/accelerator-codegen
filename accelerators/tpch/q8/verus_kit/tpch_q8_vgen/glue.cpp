#include "tpch_q8_vgen_verus.h"
#include "substrate/verus/kit/verus_kit.hpp"

#include <memory>
#include <utility>

namespace {

namespace kit = accelerator_codegen::verus_kit;
namespace core = accelerator_codegen::aux_state::core;

const char kQuerySql[] = R"SQL(-- TPC-H Q8 (catalog text), :nation_name as $1, :region_name as $2, :part_type as $3.
SELECT o_year, SUM(CASE WHEN nation = $1 THEN volume ELSE 0 END) / SUM(volume) AS mkt_share
FROM (SELECT EXTRACT(year FROM o_orderdate) AS o_year,
             l_extendedprice * (1 - l_discount) AS volume, n2.n_name AS nation
      FROM part, supplier, lineitem, orders, customer, nation n1, nation n2, region
      WHERE p_partkey = l_partkey AND s_suppkey = l_suppkey
        AND l_orderkey = o_orderkey AND o_custkey = c_custkey
        AND c_nationkey = n1.n_nationkey AND n1.n_regionkey = r_regionkey
        AND r_name = $2 AND s_nationkey = n2.n_nationkey
        AND o_orderdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31'
        AND p_type = $3) AS all_nations
GROUP BY o_year
ORDER BY o_year
)SQL";

const char kPartsSql[] = "SELECT p_partkey, p_type FROM part";
const char kSuppliersSql[] = "SELECT s_suppkey, s_nationkey FROM supplier";
const char kCustomersSql[] = "SELECT c_custkey, c_nationkey FROM customer";
const char kOrdersSql[] = "SELECT o_orderkey, o_custkey, o_orderdate FROM orders WHERE o_orderdate >= DATE '1995-01-01' AND o_orderdate <= DATE '1996-12-31'";
const char kLinesSql[] = "SELECT l_orderkey, l_partkey, l_suppkey, l_extendedprice, l_discount FROM lineitem WHERE EXISTS (SELECT 1 FROM orders WHERE o_orderkey = l_orderkey AND o_orderdate >= DATE '1995-01-01' AND o_orderdate <= DATE '1996-12-31')";
const char kNationsSql[] = "SELECT n_nationkey, n_name, n_regionkey FROM nation";
const char kRegionsSql[] = "SELECT r_regionkey, r_name FROM region";
const char kRescanSql[] = "SELECT l_orderkey, l_partkey, l_suppkey, l_extendedprice, l_discount FROM lineitem";

constexpr int32_t kStart1995 = 9131;
constexpr int32_t kEnd1995 = 9495;
constexpr int32_t kStart1996 = 9496;
constexpr int32_t kEnd1996 = 9861;

tpchq8vgen_supplier_input ReadSupplier(const core::AuxRow &row) {
  return {kit::I64(row, 0, 0), kit::I64(row, 1, 0)};
}

tpchq8vgen_customer_input ReadCustomer(const core::AuxRow &row) {
  return {kit::I64(row, 0, 0), kit::I64(row, 1, 0)};
}

tpchq8vgen_line_input ReadLine(const core::AuxRow &row) {
  return {kit::I64(row, 0, 0), kit::I64(row, 1, 0), kit::I64(row, 2, 0),
          kit::I64(row, 3, 2), kit::I64(row, 4, 2)};
}

struct Parts {
  std::vector<int64_t> keys;
  kit::TextColumn types;

  void Reserve(std::size_t count) {
    keys.reserve(count);
    types.off.reserve(count + 1);
  }

  void Push(const core::AuxRow &row) {
    keys.push_back(kit::I64(row, 0, 0));
    types.Push(kit::Text(row, 1));
  }
};

struct Nations {
  std::vector<int64_t> keys;
  kit::TextColumn names;
  std::vector<int64_t> regions;

  void Reserve(std::size_t count) {
    keys.reserve(count);
    names.off.reserve(count + 1);
    regions.reserve(count);
  }

  void Push(const core::AuxRow &row) {
    keys.push_back(kit::I64(row, 0, 0));
    names.Push(kit::Text(row, 1));
    regions.push_back(kit::I64(row, 2, 0));
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
  Parts parts;
  std::vector<tpchq8vgen_supplier_input> suppliers;
  std::vector<tpchq8vgen_customer_input> customers;
  std::vector<tpchq8vgen_order_input> orders;
  std::vector<tpchq8vgen_line_input> lines;
  Nations nations;
  Regions regions;

  explicit BuildInputs(kit::Host &host) {
    host.Query(kPartsSql, [&](const core::AuxRow &row) { parts.Push(row); });
    host.Query(kSuppliersSql, [&](const core::AuxRow &row) {
      suppliers.push_back(ReadSupplier(row));
    });
    host.Query(kCustomersSql, [&](const core::AuxRow &row) {
      customers.push_back(ReadCustomer(row));
    });
    host.Query(kOrdersSql, [&](const core::AuxRow &row) {
      orders.push_back({kit::I64(row, 0, 0), kit::I64(row, 1, 0), kit::Date(row, 2)});
    });
    host.Query(kLinesSql, [&](const core::AuxRow &row) { lines.push_back(ReadLine(row)); });
    host.Query(kNationsSql, [&](const core::AuxRow &row) { nations.Push(row); });
    host.Query(kRegionsSql, [&](const core::AuxRow &row) { regions.Push(row); });
  }

  template <typename Function>
  auto WithArgs(Function function) const {
    return function(
        parts.keys.size(), parts.keys.data(), parts.types.bytes.data(), parts.types.off.data(),
        suppliers.size(), suppliers.data(), customers.size(), customers.data(),
        orders.size(), orders.data(), lines.size(), lines.data(),
        nations.keys.size(), nations.keys.data(), nations.names.bytes.data(),
        nations.names.off.data(), nations.regions.data(),
        regions.keys.size(), regions.keys.data(), regions.names.bytes.data(), regions.names.off.data());
  }
};

struct DeltaInputs {
  Parts parts;
  std::vector<int64_t> part_signs;
  std::vector<tpchq8vgen_supplier_delta> suppliers;
  std::vector<tpchq8vgen_customer_delta> customers;
  std::vector<tpchq8vgen_order_delta> orders;
  std::vector<tpchq8vgen_line_delta> lines;
  Nations nations;
  std::vector<int64_t> nation_signs;
  Regions regions;
  std::vector<int64_t> region_signs;

  explicit DeltaInputs(const kit::Changes &changes) {
    if (changes.size() != 7) {
      throw kit::Declined("tpch_q8_vgen: expected seven change tables");
    }
    parts.Reserve(changes[0].size());
    part_signs.reserve(changes[0].size());
    for (const auto &change : changes[0]) {
      parts.Push(change.row);
      part_signs.push_back(change.sign);
    }
    suppliers.reserve(changes[1].size());
    for (const auto &change : changes[1]) {
      suppliers.push_back({ReadSupplier(change.row), change.sign});
    }
    customers.reserve(changes[2].size());
    for (const auto &change : changes[2]) {
      customers.push_back({ReadCustomer(change.row), change.sign});
    }
    orders.reserve(changes[3].size());
    for (const auto &change : changes[3]) {
      const auto &row = change.row;
      const int32_t date = kit::Date(row, 2);
      orders.push_back({kit::I64(row, 0, 0), kit::I64(row, 1, 0),
                        static_cast<uint8_t>(date >= kStart1995 && date <= kEnd1995),
                        static_cast<uint8_t>(date >= kStart1996 && date <= kEnd1996),
                        change.sign});
    }
    lines.reserve(changes[4].size());
    for (const auto &change : changes[4]) {
      lines.push_back({ReadLine(change.row), change.sign});
    }
    nations.Reserve(changes[5].size());
    nation_signs.reserve(changes[5].size());
    for (const auto &change : changes[5]) {
      nations.Push(change.row);
      nation_signs.push_back(change.sign);
    }
    regions.Reserve(changes[6].size());
    region_signs.reserve(changes[6].size());
    for (const auto &change : changes[6]) {
      regions.Push(change.row);
      region_signs.push_back(change.sign);
    }
  }

  template <typename Function>
  auto WithArgs(Function function) const {
    return function(
        customers.size(), customers.data(), lines.size(), lines.data(),
        nations.keys.size(), nations.keys.data(), nations.names.bytes.data(),
        nations.names.off.data(), nations.regions.data(), nation_signs.data(),
        orders.size(), orders.data(),
        parts.keys.size(), parts.keys.data(), parts.types.bytes.data(), parts.types.off.data(),
        part_signs.data(),
        regions.keys.size(), regions.keys.data(), regions.names.bytes.data(), regions.names.off.data(),
        region_signs.data(), suppliers.size(), suppliers.data());
  }
};

struct Rescan {
  std::vector<int64_t> keys;
  std::vector<tpchq8vgen_line_input> rows;

  template <typename KeyFunction>
  Rescan(const tpchq8vgen_state *state, const DeltaInputs &deltas,
         kit::Host &host, KeyFunction key_function, const char *key_column) {
    for (;;) {
      const std::size_t count = deltas.WithArgs([&](auto... args) {
        return key_function(state, args..., keys.data(), keys.size());
      });
      if (count <= keys.size()) {
        keys.resize(count);
        break;
      }
      keys.resize(count);
    }
    rows.reserve(keys.size());
    kit::QueryKeys(host, kRescanSql, key_column, keys, [&](const core::AuxRow &row) {
      rows.push_back(ReadLine(row));
    });
  }
};

struct ApplyInputs {
  DeltaInputs deltas;
  Rescan orders;
  Rescan parts;
  Rescan suppliers;

  ApplyInputs(const tpchq8vgen_state *state, const kit::Changes &changes, kit::Host &host)
      : deltas(changes),
        orders(state, deltas, host, tpchq8vgen_rescan_lines_for_orders_keys, "l_orderkey"),
        parts(state, deltas, host, tpchq8vgen_rescan_lines_for_parts_keys, "l_partkey"),
        suppliers(state, deltas, host, tpchq8vgen_rescan_lines_for_suppliers_keys, "l_suppkey") {}

  template <typename Function>
  auto WithArgs(Function function) const {
    return deltas.WithArgs([&](auto... args) {
      return function(args...,
                      orders.keys.size(), orders.keys.data(), orders.rows.size(), orders.rows.data(),
                      parts.keys.size(), parts.keys.data(), parts.rows.size(), parts.rows.data(),
                      suppliers.keys.size(), suppliers.keys.data(), suppliers.rows.size(), suppliers.rows.data());
    });
  }
};

void *Build(kit::Host &host) {
  const BuildInputs inputs(host);
  return inputs.WithArgs(tpchq8vgen_build);
}

void FreeState(void *state) {
  tpchq8vgen_free(static_cast<tpchq8vgen_state *>(state));
}

void *PrepareBuild(const kit::HeadFn &head_of, kit::Host &host) {
  const BuildInputs inputs(host);
  return inputs.WithArgs([&](auto... args) {
    return tpchq8vgens_stage_build(static_cast<const tpchq8vgens_head *>(head_of()), args...);
  });
}

void *Apply(const void *state, const kit::Changes &changes, kit::Host &host, int32_t *err) {
  const auto *kernel_state = static_cast<const tpchq8vgen_state *>(state);
  const ApplyInputs inputs(kernel_state, changes, host);
  return inputs.WithArgs([&](auto... args) {
    return tpchq8vgen_apply(kernel_state, args..., err);
  });
}

void *PrepareApply(const void *base, const kit::Changes &changes,
                   kit::Host &host, int32_t *err) {
  const auto *kernel_base = static_cast<const tpchq8vgens_base *>(base);
  const ApplyInputs inputs(tpchq8vgens_base_state(kernel_base), changes, host);
  return inputs.WithArgs([&](auto... args) {
    return tpchq8vgens_stage_apply(kernel_base, args..., err);
  });
}

template <class Call>
bool Run(const kit::core::QueryArguments &args, std::vector<kit::core::AuxRow> &out, Call call) {
  std::string nation;
  std::string region;
  std::string part_type;
  if (!kit::ArgText(args, 0, nation) || !kit::ArgText(args, 1, region) ||
      !kit::ArgText(args, 2, part_type)) {
    return false;
  }
  std::unique_ptr<tpchq8vgen_result, decltype(&tpchq8vgen_result_free)> result(
      call(reinterpret_cast<const uint8_t *>(nation.data()), nation.size(),
                         reinterpret_cast<const uint8_t *>(region.data()), region.size(),
                         reinterpret_cast<const uint8_t *>(part_type.data()), part_type.size()),
      tpchq8vgen_result_free);
  if (!result) {
    return false;
  }
  const std::size_t count = tpchq8vgen_result_len(result.get());
  out.reserve(out.size() + count);
  for (std::size_t index = 0; index < count; ++index) {
    int64_t year = 0;
    int64_t numerator_hi = 0;
    uint64_t numerator_lo = 0;
    int64_t denominator_hi = 0;
    uint64_t denominator_lo = 0;
    core::AuxRow row;
    row.values.reserve(2);
    if (tpchq8vgen_result_c0(result.get(), index, &year)) {
      row.values.push_back(kit::OutI64(year));
    } else {
      row.values.push_back(kit::OutNull(core::AuxType::Int64));
    }
    if (tpchq8vgen_result_c1(result.get(), index, &numerator_hi, &numerator_lo,
                           &denominator_hi, &denominator_lo)) {
      row.values.push_back(kit::OutRat(numerator_hi, numerator_lo, denominator_hi, denominator_lo));
    } else {
      row.values.push_back(kit::OutNull(core::AuxType::Double));
    }
    out.push_back(std::move(row));
  }
  return true;
}

bool Execute(const void *state, const kit::core::QueryArguments &args,
             std::vector<kit::core::AuxRow> &out) {
  return Run(args, out, [&](auto... x) {
    return tpchq8vgen_execute(static_cast<const tpchq8vgen_state *>(state), x...);
  });
}

bool Answer(const void *state, const kit::core::QueryArguments &args,
            std::vector<kit::core::AuxRow> &out) {
  return Run(args, out, [&](auto... x) {
    return tpchq8vgens_answer(static_cast<const tpchq8vgens_reading *>(state), x...);
  });
}

const kit::Glue kGlue = {
    "tpch_q8_vgen", "tpch", 8, kQuerySql,
    {{"part", {"p_partkey", "p_type"}},
     {"supplier", {"s_suppkey", "s_nationkey"}},
     {"customer", {"c_custkey", "c_nationkey"}},
     {"orders", {"o_orderkey", "o_custkey", "o_orderdate"}},
     {"lineitem", {"l_orderkey", "l_partkey", "l_suppkey", "l_extendedprice", "l_discount"}},
     {"nation", {"n_nationkey", "n_name", "n_regionkey"}},
     {"region", {"r_regionkey", "r_name"}}},
    {kit::Kind::Text, kit::Kind::Text, kit::Kind::Text},
    Build, FreeState, Apply, Execute, Answer, PrepareApply, PrepareBuild,
    kit::StoreOps{
        []() -> void * { return tpchq8vgens_new(); },
        [](void *store) { tpchq8vgens_free(static_cast<tpchq8vgens_store *>(store)); },
        [](const void *store) -> void * { return tpchq8vgens_newest_base(static_cast<const tpchq8vgens_store *>(store)); },
        [](const void *base) -> const void * { return tpchq8vgens_base_state(static_cast<const tpchq8vgens_base *>(base)); },
        [](void *base) { tpchq8vgens_base_free(static_cast<tpchq8vgens_base *>(base)); },
        [](const void *store) -> void * { return tpchq8vgens_newest_head(static_cast<const tpchq8vgens_store *>(store)); },
        [](void *head) { tpchq8vgens_head_free(static_cast<tpchq8vgens_head *>(head)); },
        [](const void *head) -> void * { return tpchq8vgens_stage_invalid(static_cast<const tpchq8vgens_head *>(head)); },
        [](void *staged) { tpchq8vgens_staged_free(static_cast<tpchq8vgens_staged *>(staged)); },
        [](const void *store, uint64_t from) -> int32_t { return tpchq8vgens_begin(static_cast<const tpchq8vgens_store *>(store), from); },
        [](const void *store) { tpchq8vgens_abort(static_cast<const tpchq8vgens_store *>(store)); },
        [](const void *store, void *staged, uint64_t from, uint64_t floor, int32_t end_commit) -> int32_t {
            return tpchq8vgens_install(static_cast<const tpchq8vgens_store *>(store), static_cast<tpchq8vgens_staged *>(staged), from, floor, end_commit);
        },
        [](const void *store, uint64_t at, uint64_t tries, int32_t *status) -> const void * {
            return tpchq8vgens_read(static_cast<const tpchq8vgens_store *>(store), at, tries, status);
        },
        [](const void *reading) { tpchq8vgens_reading_free(const_cast<tpchq8vgens_reading *>(static_cast<const tpchq8vgens_reading *>(reading))); },
        [](const void *store) { tpchq8vgens_invalidate_all(static_cast<const tpchq8vgens_store *>(store)); },
        [](const void *store) -> std::size_t { return tpchq8vgens_version_count(static_cast<const tpchq8vgens_store *>(store)); },
        []() -> std::size_t { return tpchq8vgens_heap_bytes(); },
        []() -> std::size_t { return tpchq8vgens_heap_peak_bytes(); },
        []() { tpchq8vgens_heap_reset_peak(); },
    },
};

const kit::Registrar kRegistrar(&kGlue);

}
