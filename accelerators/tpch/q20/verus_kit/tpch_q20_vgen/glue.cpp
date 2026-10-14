#include "tpch_q20_vgen_verus.h"
#include "substrate/verus/kit/verus_kit.hpp"

#include <malloc.h>
#include <memory>
#include <utility>

namespace {

namespace kit = accelerator_codegen::verus_kit;
namespace core = accelerator_codegen::aux_state::core;

const char kQuerySql[] = R"SQL(-- TPC-H Q20 (catalog text), :start_date as $1, :part_name_pattern (:color || '%', e.g. 'gainsboro%') as $2, :nation_name as $3, $4 = $1 + 1 year (derived end date: sql2verus cannot add months to a non-literal date).
SELECT s_name, s_address
FROM supplier, nation
WHERE s_suppkey IN (SELECT ps_suppkey FROM partsupp,
                      (SELECT l_partkey agg_partkey, l_suppkey agg_suppkey, 0.5 * SUM(l_quantity) AS agg_quantity
                       FROM lineitem WHERE l_shipdate >= $1
                         AND l_shipdate < $4
                       GROUP BY l_partkey, l_suppkey) agg_lineitem
                    WHERE agg_partkey = ps_partkey AND agg_suppkey = ps_suppkey
                      AND ps_partkey IN (SELECT p_partkey FROM part WHERE p_name LIKE $2)
                      AND ps_availqty > agg_quantity)
  AND s_nationkey = n_nationkey AND n_name = $3
ORDER BY s_name
)SQL";

const char kPartsSql[] = "SELECT p_partkey, p_name FROM part";
const char kStocksSql[] =
    "SELECT ps_partkey, ps_suppkey, ps_availqty FROM partsupp";
const char kShipmentsSql[] =
    "SELECT l_partkey, l_suppkey, l_quantity, l_shipdate FROM lineitem";
const char kSuppliersSql[] =
    "SELECT s_suppkey, s_name, s_address, s_nationkey FROM supplier";
const char kNationsSql[] = "SELECT n_nationkey, n_name FROM nation";
const char kStocksForPartsSql[] =
    "SELECT ps_partkey, ps_suppkey, ps_availqty FROM partsupp";
const char kShipmentsForPartsSql[] =
    "SELECT l_partkey, l_suppkey, l_quantity, l_shipdate FROM lineitem";

tpchq20vgen_stock_input Stock(const core::AuxRow &row) {
  return {kit::I64(row, 0, 0), kit::I64(row, 1, 0), kit::I64(row, 2, 0)};
}

tpchq20vgen_shipment_input Shipment(const core::AuxRow &row) {
  return {kit::I64(row, 0, 0), kit::I64(row, 1, 0),
          kit::I64(row, 2, 2), kit::Date(row, 3)};
}

struct Suppliers {
  std::vector<int64_t> keys;
  kit::TextColumn names;
  kit::TextColumn addresses;
  std::vector<int64_t> nation_keys;
  std::vector<int64_t> signs;

  void Reserve(std::size_t count) {
    keys.reserve(count);
    names.off.reserve(count + 1);
    addresses.off.reserve(count + 1);
    nation_keys.reserve(count);
    signs.reserve(count);
  }

  void Push(const core::AuxRow &row) {
    keys.push_back(kit::I64(row, 0, 0));
    names.Push(kit::Text(row, 1));
    addresses.Push(kit::Text(row, 2));
    nation_keys.push_back(kit::I64(row, 3, 0));
  }
};

struct Nations {
  std::vector<int64_t> keys;
  kit::TextColumn names;
  std::vector<int64_t> signs;

  void Reserve(std::size_t count) {
    keys.reserve(count);
    names.off.reserve(count + 1);
    signs.reserve(count);
  }

  void Push(const core::AuxRow &row) {
    keys.push_back(kit::I64(row, 0, 0));
    names.Push(kit::Text(row, 1));
  }
};

struct BuildInputs {
  Nations parts;
  std::vector<tpchq20vgen_stock_input> stocks;
  std::vector<tpchq20vgen_shipment_input> shipments;
  Suppliers suppliers;
  Nations nations;

  explicit BuildInputs(kit::Host &host) {
    host.Query(kPartsSql, [&](const core::AuxRow &row) {
      parts.Push(row);
    });
    host.Query(kStocksSql, [&](const core::AuxRow &row) {
      stocks.push_back(Stock(row));
    });
    host.Query(kShipmentsSql, [&](const core::AuxRow &row) {
      shipments.push_back(Shipment(row));
    });
    host.Query(kSuppliersSql, [&](const core::AuxRow &row) {
      suppliers.Push(row);
    });
    host.Query(kNationsSql, [&](const core::AuxRow &row) {
      nations.Push(row);
    });
  }
};

void *Build(kit::Host &host) {
  void *built = nullptr;
  {
  const BuildInputs inputs(host);
  built = tpchq20vgen_build(
      inputs.parts.keys.size(), inputs.parts.keys.data(),
      inputs.parts.names.bytes.data(), inputs.parts.names.off.data(),
      inputs.stocks.size(), inputs.stocks.data(),
      inputs.shipments.size(), inputs.shipments.data(),
      inputs.suppliers.keys.size(), inputs.suppliers.keys.data(),
      inputs.suppliers.names.bytes.data(), inputs.suppliers.names.off.data(),
      inputs.suppliers.addresses.bytes.data(), inputs.suppliers.addresses.off.data(),
      inputs.suppliers.nation_keys.data(),
      inputs.nations.keys.size(), inputs.nations.keys.data(),
      inputs.nations.names.bytes.data(), inputs.nations.names.off.data());
  }
  malloc_trim(0);
  return built;
}

void *PrepareBuild(const void *store, kit::Host &host) {
  void *built = nullptr;
  {
  const BuildInputs inputs(host);
  built = tpchq20vgens_prepare_build(
      static_cast<const tpchq20vgens_store *>(store),
      inputs.parts.keys.size(), inputs.parts.keys.data(),
      inputs.parts.names.bytes.data(), inputs.parts.names.off.data(),
      inputs.stocks.size(), inputs.stocks.data(),
      inputs.shipments.size(), inputs.shipments.data(),
      inputs.suppliers.keys.size(), inputs.suppliers.keys.data(),
      inputs.suppliers.names.bytes.data(), inputs.suppliers.names.off.data(),
      inputs.suppliers.addresses.bytes.data(), inputs.suppliers.addresses.off.data(),
      inputs.suppliers.nation_keys.data(),
      inputs.nations.keys.size(), inputs.nations.keys.data(),
      inputs.nations.names.bytes.data(), inputs.nations.names.off.data());
  }
  malloc_trim(0);
  return built;
}

struct ApplyInputs {
  std::vector<tpchq20vgen_lineitem_delta> lineitem;
  Nations nation;
  Nations part;
  std::vector<tpchq20vgen_partsupp_delta> partsupp;
  Suppliers supplier;
  std::vector<int64_t> stock_keys;
  std::vector<tpchq20vgen_stock_input> stocks;
  std::vector<int64_t> shipment_keys;
  std::vector<tpchq20vgen_shipment_input> shipments;

  explicit ApplyInputs(const kit::Changes &changes) {
    part.Reserve(changes.at(0).size());
    for (const auto &change : changes.at(0)) {
      part.Push(change.row);
      part.signs.push_back(change.sign);
    }
    partsupp.reserve(changes.at(1).size());
    for (const auto &change : changes.at(1)) {
      partsupp.push_back({Stock(change.row), change.sign});
    }
    lineitem.reserve(changes.at(2).size());
    for (const auto &change : changes.at(2)) {
      lineitem.push_back({Shipment(change.row), change.sign});
    }
    supplier.Reserve(changes.at(3).size());
    for (const auto &change : changes.at(3)) {
      supplier.Push(change.row);
      supplier.signs.push_back(change.sign);
    }
    nation.Reserve(changes.at(4).size());
    for (const auto &change : changes.at(4)) {
      nation.Push(change.row);
      nation.signs.push_back(change.sign);
    }
  }

  std::vector<int64_t> Keys(
      const tpchq20vgen_state *state,
      decltype(&tpchq20vgen_rescan_stocks_for_parts_keys) rescan) const {
    std::vector<int64_t> keys;
    for (;;) {
      const std::size_t count = rescan(
          state,
          lineitem.size(), lineitem.data(),
          nation.keys.size(), nation.keys.data(),
          nation.names.bytes.data(), nation.names.off.data(), nation.signs.data(),
          part.keys.size(), part.keys.data(),
          part.names.bytes.data(), part.names.off.data(), part.signs.data(),
          partsupp.size(), partsupp.data(),
          supplier.keys.size(), supplier.keys.data(),
          supplier.names.bytes.data(), supplier.names.off.data(),
          supplier.addresses.bytes.data(), supplier.addresses.off.data(),
          supplier.nation_keys.data(), supplier.signs.data(),
          keys.data(), keys.size());
      if (count <= keys.size()) {
        keys.resize(count);
        return keys;
      }
      keys.resize(count);
    }
  }

  void Rescan(const tpchq20vgen_state *state, kit::Host &host) {
    stock_keys = Keys(state, tpchq20vgen_rescan_stocks_for_parts_keys);
    kit::QueryKeys(host, kStocksForPartsSql, "ps_partkey", stock_keys,
                   [&](const core::AuxRow &row) { stocks.push_back(Stock(row)); });
    shipment_keys = Keys(state, tpchq20vgen_rescan_shipments_for_parts_keys);
    kit::QueryKeys(host, kShipmentsForPartsSql, "l_partkey", shipment_keys,
                   [&](const core::AuxRow &row) { shipments.push_back(Shipment(row)); });
  }
};

void *Apply(const void *state, const kit::Changes &changes, kit::Host &host,
            int32_t *err) {
  ApplyInputs inputs(changes);
  const auto *kernel_state = static_cast<const tpchq20vgen_state *>(state);
  inputs.Rescan(kernel_state, host);
  return tpchq20vgen_apply(
      kernel_state,
      inputs.lineitem.size(), inputs.lineitem.data(),
      inputs.nation.keys.size(), inputs.nation.keys.data(),
      inputs.nation.names.bytes.data(), inputs.nation.names.off.data(),
      inputs.nation.signs.data(),
      inputs.part.keys.size(), inputs.part.keys.data(),
      inputs.part.names.bytes.data(), inputs.part.names.off.data(),
      inputs.part.signs.data(),
      inputs.partsupp.size(), inputs.partsupp.data(),
      inputs.supplier.keys.size(), inputs.supplier.keys.data(),
      inputs.supplier.names.bytes.data(), inputs.supplier.names.off.data(),
      inputs.supplier.addresses.bytes.data(), inputs.supplier.addresses.off.data(),
      inputs.supplier.nation_keys.data(), inputs.supplier.signs.data(),
      inputs.stock_keys.size(), inputs.stock_keys.data(),
      inputs.stocks.size(), inputs.stocks.data(),
      inputs.shipment_keys.size(), inputs.shipment_keys.data(),
      inputs.shipments.size(), inputs.shipments.data(), err);
}

void *PrepareApply(const void *store, const void *base,
                   const kit::Changes &changes, kit::Host &host, int32_t *err) {
  ApplyInputs inputs(changes);
  const auto *kernel_base = static_cast<const tpchq20vgens_base *>(base);
  inputs.Rescan(tpchq20vgens_base_state(kernel_base), host);
  return tpchq20vgens_prepare_apply(
      static_cast<const tpchq20vgens_store *>(store), kernel_base,
      inputs.lineitem.size(), inputs.lineitem.data(),
      inputs.nation.keys.size(), inputs.nation.keys.data(),
      inputs.nation.names.bytes.data(), inputs.nation.names.off.data(),
      inputs.nation.signs.data(),
      inputs.part.keys.size(), inputs.part.keys.data(),
      inputs.part.names.bytes.data(), inputs.part.names.off.data(),
      inputs.part.signs.data(),
      inputs.partsupp.size(), inputs.partsupp.data(),
      inputs.supplier.keys.size(), inputs.supplier.keys.data(),
      inputs.supplier.names.bytes.data(), inputs.supplier.names.off.data(),
      inputs.supplier.addresses.bytes.data(), inputs.supplier.addresses.off.data(),
      inputs.supplier.nation_keys.data(), inputs.supplier.signs.data(),
      inputs.stock_keys.size(), inputs.stock_keys.data(),
      inputs.stocks.size(), inputs.stocks.data(),
      inputs.shipment_keys.size(), inputs.shipment_keys.data(),
      inputs.shipments.size(), inputs.shipments.data(), err);
}

void FreeState(void *state) {
  tpchq20vgen_free(static_cast<tpchq20vgen_state *>(state));
}

bool Execute(const void *state, const core::QueryArguments &args,
             std::vector<core::AuxRow> &out) {
  int32_t start_date;
  std::string pattern;
  std::string nation_name;
  int32_t end_date;
  if (!kit::ArgDate(args, 0, start_date) ||
      !kit::ArgText(args, 1, pattern) ||
      !kit::ArgText(args, 2, nation_name) ||
      !kit::ArgDate(args, 3, end_date)) {
    return false;
  }
  std::unique_ptr<tpchq20vgen_result, decltype(&tpchq20vgen_result_free)> result(
      tpchq20vgen_execute(static_cast<const tpchq20vgen_state *>(state),
                         start_date,
                         reinterpret_cast<const uint8_t *>(pattern.data()),
                         pattern.size(),
                         reinterpret_cast<const uint8_t *>(nation_name.data()),
                         nation_name.size(), end_date),
      tpchq20vgen_result_free);
  if (!result) {
    return false;
  }
  const std::size_t count = tpchq20vgen_result_len(result.get());
  std::vector<core::AuxRow> rows;
  rows.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    core::AuxRow row;
    row.values.reserve(2);
    const uint8_t *bytes = nullptr;
    std::size_t length = 0;
    if (tpchq20vgen_result_c0(result.get(), index, &bytes, &length)) {
      row.values.push_back(kit::OutText(bytes, length));
    } else {
      row.values.push_back(kit::OutNull(core::AuxType::String));
    }
    if (tpchq20vgen_result_c1(result.get(), index, &bytes, &length)) {
      row.values.push_back(kit::OutText(bytes, length));
    } else {
      row.values.push_back(kit::OutNull(core::AuxType::String));
    }
    rows.push_back(std::move(row));
  }
  out.swap(rows);
  return true;
}

const kit::Glue kGlue = {
    "tpch_q20_vgen", "tpch", 20, kQuerySql,
    {{"part", {"p_partkey", "p_name"}},
     {"partsupp", {"ps_partkey", "ps_suppkey", "ps_availqty"}},
     {"lineitem", {"l_partkey", "l_suppkey", "l_quantity", "l_shipdate"}},
     {"supplier", {"s_suppkey", "s_name", "s_address", "s_nationkey"}},
     {"nation", {"n_nationkey", "n_name"}}},
    {kit::Kind::Date, kit::Kind::Text, kit::Kind::Text, kit::Kind::Date},
    Build, FreeState, Apply, Execute, PrepareApply, PrepareBuild,
    kit::StoreOps{
        []() -> void * { return tpchq20vgens_new(); },
        [](void *store) {
          tpchq20vgens_free(static_cast<tpchq20vgens_store *>(store));
        },
        [](const void *store) -> void * {
          return tpchq20vgens_newest_base(static_cast<const tpchq20vgens_store *>(store));
        },
        [](const void *base) -> const void * {
          return tpchq20vgens_base_state(static_cast<const tpchq20vgens_base *>(base));
        },
        [](void *base) {
          tpchq20vgens_base_free(static_cast<tpchq20vgens_base *>(base));
        },
        [](const void *store) -> void * {
          return tpchq20vgens_prepare_invalid(static_cast<const tpchq20vgens_store *>(store));
        },
        [](void *prepared) {
          tpchq20vgens_prepared_free(static_cast<tpchq20vgens_prepared *>(prepared));
        },
        [](const void *store, uint64_t from) -> int32_t {
          return tpchq20vgens_begin(static_cast<const tpchq20vgens_store *>(store), from);
        },
        [](const void *store) {
          tpchq20vgens_abort(static_cast<const tpchq20vgens_store *>(store));
        },
        [](const void *store, void *prepared, uint64_t from, uint64_t floor,
           int32_t end_commit) -> int32_t {
          return tpchq20vgens_install(static_cast<const tpchq20vgens_store *>(store),
                                     static_cast<tpchq20vgens_prepared *>(prepared),
                                     from, floor, end_commit);
        },
        [](const void *store, uint64_t at, uint64_t tries,
           int32_t *status) -> const void * {
          return tpchq20vgens_read(static_cast<const tpchq20vgens_store *>(store),
                                  at, tries, status);
        },
        [](const void *state) {
          tpchq20vgens_state_release(static_cast<const tpchq20vgen_state *>(state));
        },
        [](const void *store) {
          tpchq20vgens_invalidate_all(static_cast<const tpchq20vgens_store *>(store));
        },
        [](const void *store) -> std::size_t {
          return tpchq20vgens_version_count(static_cast<const tpchq20vgens_store *>(store));
        }}};

const kit::Registrar kRegistrar(&kGlue);

}
