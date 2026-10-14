#include "tpch_q19_vgen_verus.h"
#include "substrate/verus/kit/verus_kit.hpp"

#include <memory>
#include <string>
#include <vector>

namespace {

namespace kit = accelerator_codegen::verus_kit;
namespace core = accelerator_codegen::aux_state::core;

const char kQuerySql[] = R"SQL(-- TPC-H Q19 (catalog text), :brand1 as $1, :quantity1 as $2, :brand2 as $3, :quantity2 as $4, :brand3 as $5, :quantity3 as $6 (upper bound written l_quantity - 10 <= $n, equal to l_quantity <= $n + 10: sql2verus cannot type a parameter used at two types).
SELECT SUM(l_extendedprice * (1 - l_discount)) AS revenue
FROM lineitem, part
WHERE (p_partkey = l_partkey
       AND p_brand = $1
       AND p_container IN ('SM CASE', 'SM BOX', 'SM PACK', 'SM PKG')
       AND l_quantity >= $2 AND l_quantity - 10 <= $2
       AND p_size BETWEEN 1 AND 5
       AND l_shipmode IN ('AIR', 'AIR REG')
       AND l_shipinstruct = 'DELIVER IN PERSON')
   OR (p_partkey = l_partkey
       AND p_brand = $3
       AND p_container IN ('MED BAG', 'MED BOX', 'MED PKG', 'MED PACK')
       AND l_quantity >= $4 AND l_quantity - 10 <= $4
       AND p_size BETWEEN 1 AND 10
       AND l_shipmode IN ('AIR', 'AIR REG')
       AND l_shipinstruct = 'DELIVER IN PERSON')
   OR (p_partkey = l_partkey
       AND p_brand = $5
       AND p_container IN ('LG CASE', 'LG BOX', 'LG PACK', 'LG PKG')
       AND l_quantity >= $6 AND l_quantity - 10 <= $6
       AND p_size BETWEEN 1 AND 15
       AND l_shipmode IN ('AIR', 'AIR REG')
       AND l_shipinstruct = 'DELIVER IN PERSON')
)SQL";

const char kPartsSql[] = R"SQL(SELECT p_partkey, p_brand, p_size, p_container
FROM part
WHERE (p_size >= 1 AND p_size <= 5
       AND p_container IN ('SM CASE', 'SM BOX', 'SM PACK', 'SM PKG'))
   OR (p_size >= 1 AND p_size <= 10
       AND p_container IN ('MED BAG', 'MED BOX', 'MED PKG', 'MED PACK'))
   OR (p_size >= 1 AND p_size <= 15
       AND p_container IN ('LG CASE', 'LG BOX', 'LG PACK', 'LG PKG'));)SQL";

const char kLinesSql[] = R"SQL(SELECT l_partkey, l_quantity, l_extendedprice, l_discount
FROM lineitem
WHERE l_shipmode IN ('AIR', 'AIR REG')
  AND l_shipinstruct = 'DELIVER IN PERSON'
  AND EXISTS (
      SELECT p_partkey FROM part
      WHERE p_partkey = l_partkey
        AND ((p_size >= 1 AND p_size <= 5
              AND p_container IN ('SM CASE', 'SM BOX', 'SM PACK', 'SM PKG'))
          OR (p_size >= 1 AND p_size <= 10
              AND p_container IN ('MED BAG', 'MED BOX', 'MED PKG', 'MED PACK'))
          OR (p_size >= 1 AND p_size <= 15
              AND p_container IN ('LG CASE', 'LG BOX', 'LG PACK', 'LG PKG')))
  );)SQL";

const char kRescanSql[] = R"SQL(SELECT l_partkey, l_quantity, l_extendedprice, l_discount
FROM lineitem
WHERE l_shipmode IN ('AIR', 'AIR REG')
  AND l_shipinstruct = 'DELIVER IN PERSON';)SQL";

tpchq19vgen_line_input ReadLine(const core::AuxRow &row) {
  return {kit::I64(row, 0, 0), kit::I64(row, 1, 2),
          kit::I64(row, 2, 2), kit::I64(row, 3, 2)};
}

struct BuildInputs {
  std::vector<int64_t> partkeys;
  kit::TextColumn brands;
  std::vector<int64_t> sizes;
  kit::TextColumn containers;
  std::vector<tpchq19vgen_line_input> lines;

  explicit BuildInputs(kit::Host &host) {
    host.Query(kPartsSql, [&](const core::AuxRow &row) {
      partkeys.push_back(kit::I64(row, 0, 0));
      brands.Push(kit::Text(row, 1));
      sizes.push_back(kit::I64(row, 2, 0));
      containers.Push(kit::Text(row, 3));
    });
    host.Query(kLinesSql, [&](const core::AuxRow &row) {
      lines.push_back(ReadLine(row));
    });
  }
};

void *Build(kit::Host &host) {
  const BuildInputs inputs(host);
  return tpchq19vgen_build(
      inputs.partkeys.size(), inputs.partkeys.data(), inputs.brands.bytes.data(),
      inputs.brands.off.data(), inputs.sizes.data(), inputs.containers.bytes.data(),
      inputs.containers.off.data(), inputs.lines.size(), inputs.lines.data());
}

void *PrepareBuild(const void *store, kit::Host &host) {
  const BuildInputs inputs(host);
  return tpchq19vgens_prepare_build(
      static_cast<const tpchq19vgens_store *>(store),
      inputs.partkeys.size(), inputs.partkeys.data(), inputs.brands.bytes.data(),
      inputs.brands.off.data(), inputs.sizes.data(), inputs.containers.bytes.data(),
      inputs.containers.off.data(), inputs.lines.size(), inputs.lines.data());
}

void FreeState(void *state) {
  tpchq19vgen_free(static_cast<tpchq19vgen_state *>(state));
}

struct ApplyInputs {
  std::vector<tpchq19vgen_line_delta> lines;
  std::vector<int64_t> partkeys;
  kit::TextColumn brands;
  std::vector<uint8_t> small;
  std::vector<uint8_t> medium;
  std::vector<uint8_t> large;
  std::vector<int64_t> signs;
  std::vector<int64_t> keys;
  std::vector<tpchq19vgen_line_input> rescan_rows;

  ApplyInputs(const tpchq19vgen_state *state, const kit::Changes &changes,
              kit::Host &host) {
    const auto &line_changes = changes.at(0);
    const auto &part_changes = changes.at(1);
    lines.reserve(line_changes.size());
    partkeys.reserve(part_changes.size());
    brands.off.reserve(part_changes.size() + 1);
    small.reserve(part_changes.size());
    medium.reserve(part_changes.size());
    large.reserve(part_changes.size());
    signs.reserve(part_changes.size());

    for (const auto &change : line_changes) {
      const auto line = ReadLine(change.row);
      const auto &shipmode = kit::Text(change.row, 4);
      const auto &shipinstruct = kit::Text(change.row, 5);
      const bool eligible = (shipmode == "AIR" || shipmode == "AIR REG") &&
                            shipinstruct == "DELIVER IN PERSON";
      lines.push_back({line.l_partkey, line.l_quantity, line.l_extendedprice,
                       line.l_discount, static_cast<uint8_t>(eligible), change.sign});
    }

    for (const auto &change : part_changes) {
      const int64_t partkey = kit::I64(change.row, 0, 0);
      const auto &brand = kit::Text(change.row, 1);
      const int64_t size = kit::I64(change.row, 2, 0);
      const auto &container = kit::Text(change.row, 3);
      partkeys.push_back(partkey);
      brands.Push(brand);
      small.push_back(size >= 1 && size <= 5 &&
                      (container == "SM CASE" || container == "SM BOX" ||
                       container == "SM PACK" || container == "SM PKG"));
      medium.push_back(size >= 1 && size <= 10 &&
                       (container == "MED BAG" || container == "MED BOX" ||
                        container == "MED PKG" || container == "MED PACK"));
      large.push_back(size >= 1 && size <= 15 &&
                      (container == "LG CASE" || container == "LG BOX" ||
                       container == "LG PACK" || container == "LG PKG"));
      signs.push_back(change.sign);
    }

    keys.resize(part_changes.size());
    for (;;) {
      const size_t count = tpchq19vgen_rescan_lines_for_parts_keys(
          state, lines.size(), lines.data(), partkeys.size(), partkeys.data(),
          brands.bytes.data(), brands.off.data(), small.data(), medium.data(),
          large.data(), signs.data(), keys.data(), keys.size());
      if (count <= keys.size()) {
        keys.resize(count);
        break;
      }
      keys.resize(count);
    }
    kit::QueryKeys(host, kRescanSql, "l_partkey", keys,
                   [&](const core::AuxRow &row) {
                     rescan_rows.push_back(ReadLine(row));
                   });
  }
};

void *Apply(const void *state, const kit::Changes &changes, kit::Host &host,
            int32_t *err) {
  const auto *kernel_state = static_cast<const tpchq19vgen_state *>(state);
  const ApplyInputs inputs(kernel_state, changes, host);
  return tpchq19vgen_apply(
      kernel_state, inputs.lines.size(), inputs.lines.data(),
      inputs.partkeys.size(), inputs.partkeys.data(), inputs.brands.bytes.data(),
      inputs.brands.off.data(), inputs.small.data(), inputs.medium.data(),
      inputs.large.data(), inputs.signs.data(), inputs.keys.size(), inputs.keys.data(),
      inputs.rescan_rows.size(), inputs.rescan_rows.data(), err);
}

void *PrepareApply(const void *store, const void *base,
                   const kit::Changes &changes, kit::Host &host, int32_t *err) {
  const auto *kernel_base = static_cast<const tpchq19vgens_base *>(base);
  const ApplyInputs inputs(tpchq19vgens_base_state(kernel_base), changes, host);
  return tpchq19vgens_prepare_apply(
      static_cast<const tpchq19vgens_store *>(store), kernel_base,
      inputs.lines.size(), inputs.lines.data(), inputs.partkeys.size(),
      inputs.partkeys.data(), inputs.brands.bytes.data(), inputs.brands.off.data(),
      inputs.small.data(), inputs.medium.data(), inputs.large.data(),
      inputs.signs.data(), inputs.keys.size(), inputs.keys.data(),
      inputs.rescan_rows.size(), inputs.rescan_rows.data(), err);
}

bool Execute(const void *state, const core::QueryArguments &args,
             std::vector<core::AuxRow> &out) {
  std::string brand1;
  std::string brand2;
  std::string brand3;
  int64_t quantity1;
  int64_t quantity2;
  int64_t quantity3;
  if (!kit::ArgText(args, 0, brand1) || !kit::ArgI64(args, 1, 2, quantity1) ||
      !kit::ArgText(args, 2, brand2) || !kit::ArgI64(args, 3, 2, quantity2) ||
      !kit::ArgText(args, 4, brand3) || !kit::ArgI64(args, 5, 2, quantity3)) {
    return false;
  }
  std::unique_ptr<tpchq19vgen_result, decltype(&tpchq19vgen_result_free)> result(
      tpchq19vgen_execute(
          static_cast<const tpchq19vgen_state *>(state),
          reinterpret_cast<const uint8_t *>(brand1.data()), brand1.size(), quantity1,
          reinterpret_cast<const uint8_t *>(brand2.data()), brand2.size(), quantity2,
          reinterpret_cast<const uint8_t *>(brand3.data()), brand3.size(), quantity3),
      &tpchq19vgen_result_free);
  if (!result) {
    return false;
  }
  const size_t count = tpchq19vgen_result_len(result.get());
  out.reserve(out.size() + count);
  for (size_t index = 0; index < count; ++index) {
    int64_t hi;
    uint64_t lo;
    if (tpchq19vgen_result_c0(result.get(), index, &hi, &lo)) {
      out.push_back(core::AuxRow{{kit::OutDec(hi, lo, 4)}});
    } else {
      out.push_back(core::AuxRow{{kit::OutNull(core::AuxType::ScaledDecimal)}});
    }
  }
  return true;
}

const kit::Glue kGlue = {
    "tpch_q19_vgen", "tpch", 19, kQuerySql,
    {{"lineitem", {"l_partkey", "l_quantity", "l_extendedprice", "l_discount",
                    "l_shipmode", "l_shipinstruct"}},
     {"part", {"p_partkey", "p_brand", "p_size", "p_container"}}},
    {kit::Kind::Text, kit::Kind::Dec, kit::Kind::Text, kit::Kind::Dec,
     kit::Kind::Text, kit::Kind::Dec},
    Build, FreeState, Apply, Execute, PrepareApply, PrepareBuild,
    kit::StoreOps{
        []() -> void * { return tpchq19vgens_new(); },
        [](void *store) {
          tpchq19vgens_free(static_cast<tpchq19vgens_store *>(store));
        },
        [](const void *store) -> void * {
          return tpchq19vgens_newest_base(static_cast<const tpchq19vgens_store *>(store));
        },
        [](const void *base) -> const void * {
          return tpchq19vgens_base_state(static_cast<const tpchq19vgens_base *>(base));
        },
        [](void *base) {
          tpchq19vgens_base_free(static_cast<tpchq19vgens_base *>(base));
        },
        [](const void *store) -> void * {
          return tpchq19vgens_prepare_invalid(static_cast<const tpchq19vgens_store *>(store));
        },
        [](void *prepared) {
          tpchq19vgens_prepared_free(static_cast<tpchq19vgens_prepared *>(prepared));
        },
        [](const void *store, uint64_t from) -> int32_t {
          return tpchq19vgens_begin(static_cast<const tpchq19vgens_store *>(store), from);
        },
        [](const void *store) {
          tpchq19vgens_abort(static_cast<const tpchq19vgens_store *>(store));
        },
        [](const void *store, void *prepared, uint64_t from, uint64_t floor,
           int32_t end_commit) -> int32_t {
          return tpchq19vgens_install(static_cast<const tpchq19vgens_store *>(store),
                                    static_cast<tpchq19vgens_prepared *>(prepared),
                                    from, floor, end_commit);
        },
        [](const void *store, uint64_t at, uint64_t tries, int32_t *status) -> const void * {
          return tpchq19vgens_read(static_cast<const tpchq19vgens_store *>(store),
                                 at, tries, status);
        },
        [](const void *state) {
          tpchq19vgens_state_release(static_cast<const tpchq19vgen_state *>(state));
        },
        [](const void *store) {
          tpchq19vgens_invalidate_all(static_cast<const tpchq19vgens_store *>(store));
        },
        [](const void *store) -> size_t {
          return tpchq19vgens_version_count(static_cast<const tpchq19vgens_store *>(store));
        }}};

const kit::Registrar kRegistrar(&kGlue);

}
