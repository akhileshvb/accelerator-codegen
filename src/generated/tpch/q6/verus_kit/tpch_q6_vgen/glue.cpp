#include "tpch_q6_vgen_verus.h"
#include "accelerator_codegen/verus/verus_kit.hpp"

#include <memory>
#include <vector>

namespace {

namespace kit = accelerator_codegen::verus_kit;
namespace core = accelerator_codegen::aux_state::core;

const char kQuerySql[] = R"SQL(-- TPC-H Q6 (catalog text), :start_date as $1, :discount as $2 (BETWEEN $2 - 0.01 AND $2 + 0.01), :quantity as $3, $4 = $1 + 1 year (derived end date: sql2verus cannot add months to a non-literal date).
SELECT SUM(l_extendedprice * l_discount) AS revenue
FROM lineitem
WHERE l_shipdate >= $1 AND l_shipdate < $4
  AND l_discount BETWEEN $2 - 0.01 AND $2 + 0.01 AND l_quantity < $3
)SQL";

const char kScanSql[] =
    "SELECT l_quantity, l_extendedprice, l_discount, l_shipdate FROM lineitem;";

tpchq6vgen_lineitem_input ReadLineitem(const core::AuxRow &row) {
  return {kit::I64(row, 0, 2), kit::I64(row, 1, 2), kit::I64(row, 2, 2),
          kit::Date(row, 3)};
}

std::vector<tpchq6vgen_lineitem_input> Scan(kit::Host &host) {
  std::vector<tpchq6vgen_lineitem_input> rows;
  host.Query(kScanSql, [&](const core::AuxRow &row) {
    rows.push_back(ReadLineitem(row));
  });
  return rows;
}

std::vector<tpchq6vgen_lineitem_delta> ReadChanges(const kit::Changes &changes) {
  const auto &lineitem = changes.at(0);
  std::vector<tpchq6vgen_lineitem_delta> rows;
  rows.reserve(lineitem.size());
  for (const auto &change : lineitem) {
    rows.push_back({ReadLineitem(change.row), change.sign});
  }
  return rows;
}

void *Build(kit::Host &host) {
  const auto rows = Scan(host);
  return tpchq6vgen_build(rows.size(), rows.data());
}

void FreeState(void *state) {
  tpchq6vgen_free(static_cast<tpchq6vgen_state *>(state));
}

void *Apply(const void *state, const kit::Changes &changes, kit::Host &,
            int32_t *err) {
  const auto rows = ReadChanges(changes);
  return tpchq6vgen_apply(static_cast<const tpchq6vgen_state *>(state),
                         rows.size(), rows.data(), err);
}

void *PrepareBuild(const void *store, kit::Host &host) {
  const auto rows = Scan(host);
  return tpchq6vgens_prepare_build(
      static_cast<const tpchq6vgens_store *>(store), rows.size(), rows.data());
}

void *PrepareApply(const void *store, const void *base,
                   const kit::Changes &changes, kit::Host &, int32_t *err) {
  const auto rows = ReadChanges(changes);
  return tpchq6vgens_prepare_apply(
      static_cast<const tpchq6vgens_store *>(store),
      static_cast<const tpchq6vgens_base *>(base), rows.size(), rows.data(), err);
}

bool Execute(const void *state, const core::QueryArguments &args,
             std::vector<core::AuxRow> &out) {
  int32_t start_date;
  int64_t discount;
  int64_t quantity;
  int32_t end_date;
  if (!kit::ArgDate(args, 0, start_date) ||
      !kit::ArgI64(args, 1, 2, discount) ||
      !kit::ArgI64(args, 2, 2, quantity) ||
      !kit::ArgDate(args, 3, end_date)) {
    return false;
  }
  std::unique_ptr<tpchq6vgen_result, decltype(&tpchq6vgen_result_free)> result(
      tpchq6vgen_execute(static_cast<const tpchq6vgen_state *>(state),
                         start_date, discount, quantity, end_date),
      &tpchq6vgen_result_free);
  if (!result) {
    return false;
  }
  const auto count = tpchq6vgen_result_len(result.get());
  out.reserve(out.size() + count);
  for (std::size_t index = 0; index < count; ++index) {
    int64_t high;
    uint64_t low;
    if (tpchq6vgen_result_c0(result.get(), index, &high, &low)) {
      out.push_back(core::AuxRow{{kit::OutDec(high, low, 4)}});
    } else {
      out.push_back(core::AuxRow{{kit::OutNull(core::AuxType::String)}});
    }
  }
  return true;
}

const kit::Glue kGlue = {
    "tpch_q6_vgen", "tpch", 6, kQuerySql,
    {{"lineitem", {"l_quantity", "l_extendedprice", "l_discount", "l_shipdate"}}},
    {kit::Kind::Date, kit::Kind::Dec, kit::Kind::Dec, kit::Kind::Date},
    Build, FreeState, Apply, Execute, PrepareApply, PrepareBuild,
    kit::StoreOps{
        []() -> void * { return tpchq6vgens_new(); },
        [](void *store) {
          tpchq6vgens_free(static_cast<tpchq6vgens_store *>(store));
        },
        [](const void *store) -> void * {
          return tpchq6vgens_newest_base(static_cast<const tpchq6vgens_store *>(store));
        },
        [](const void *base) -> const void * {
          return tpchq6vgens_base_state(static_cast<const tpchq6vgens_base *>(base));
        },
        [](void *base) {
          tpchq6vgens_base_free(static_cast<tpchq6vgens_base *>(base));
        },
        [](const void *store) -> void * {
          return tpchq6vgens_prepare_invalid(static_cast<const tpchq6vgens_store *>(store));
        },
        [](void *prepared) {
          tpchq6vgens_prepared_free(static_cast<tpchq6vgens_prepared *>(prepared));
        },
        [](const void *store, uint64_t from) -> int32_t {
          return tpchq6vgens_begin(static_cast<const tpchq6vgens_store *>(store), from);
        },
        [](const void *store) {
          tpchq6vgens_abort(static_cast<const tpchq6vgens_store *>(store));
        },
        [](const void *store, void *prepared, uint64_t from, uint64_t floor,
           int32_t end_commit) -> int32_t {
          return tpchq6vgens_install(static_cast<const tpchq6vgens_store *>(store),
                                   static_cast<tpchq6vgens_prepared *>(prepared),
                                   from, floor, end_commit);
        },
        [](const void *store, uint64_t at, uint64_t tries,
           int32_t *status) -> const void * {
          return tpchq6vgens_read(static_cast<const tpchq6vgens_store *>(store),
                                at, tries, status);
        },
        [](const void *state) {
          tpchq6vgens_state_release(static_cast<const tpchq6vgen_state *>(state));
        },
        [](const void *store) {
          tpchq6vgens_invalidate_all(static_cast<const tpchq6vgens_store *>(store));
        },
        [](const void *store) -> std::size_t {
          return tpchq6vgens_version_count(static_cast<const tpchq6vgens_store *>(store));
        },
    }};

const kit::Registrar kRegistrar(&kGlue);

}
