#include "tpch_q11_vgen_verus.h"

#include "substrate/verus/kit/verus_kit.hpp"

namespace kit = accelerator_codegen::verus_kit;
namespace core = kit::core;

namespace {

const char *const kQuerySql = R"SQL(-- TPC-H Q11 (catalog text), :nation_name as $1, :fraction (0.0001/SF) as $2 (no LIMIT, as in the catalog).
SELECT ps_partkey, SUM(ps_supplycost * ps_availqty) AS value
FROM partsupp, supplier, nation
WHERE ps_suppkey = s_suppkey AND s_nationkey = n_nationkey
  AND n_name = $1
GROUP BY ps_partkey
HAVING SUM(ps_supplycost * ps_availqty) > (SELECT SUM(ps_supplycost * ps_availqty) * $2
                                           FROM partsupp, supplier, nation
                                           WHERE ps_suppkey = s_suppkey AND s_nationkey = n_nationkey
                                             AND n_name = $1)
ORDER BY value DESC
)SQL";

const char *const kPartsuppScanSql =
    "SELECT ps_partkey, ps_suppkey, ps_availqty, ps_supplycost FROM partsupp;";
const char *const kSupplierScanSql =
    "SELECT s_suppkey, s_nationkey FROM supplier;";
const char *const kNationScanSql =
    "SELECT n_nationkey, n_name FROM nation;";

std::vector<tpchq11vgen_partsupp_input> ReadPartsupp(kit::Host &host) {
  std::vector<tpchq11vgen_partsupp_input> rows;
  host.Query(kPartsuppScanSql, [&rows](const core::AuxRow &row) {
    rows.push_back({
        kit::I64(row, 0, 0),
        kit::I64(row, 1, 0),
        kit::I64(row, 2, 0),
        kit::I64(row, 3, 2),
    });
  });
  return rows;
}

std::vector<tpchq11vgen_supplier_input> ReadSupplier(kit::Host &host) {
  std::vector<tpchq11vgen_supplier_input> rows;
  host.Query(kSupplierScanSql, [&rows](const core::AuxRow &row) {
    rows.push_back({kit::I64(row, 0, 0), kit::I64(row, 1, 0)});
  });
  return rows;
}

struct NationRows {
  std::vector<int64_t> keys;
  kit::TextColumn names;
};

NationRows ReadNation(kit::Host &host) {
  NationRows rows;
  host.Query(kNationScanSql, [&rows](const core::AuxRow &row) {
    rows.keys.push_back(kit::I64(row, 0, 0));
    rows.names.Push(kit::Text(row, 1));
  });
  return rows;
}

void *Build(kit::Host &host) {
  auto partsupp = ReadPartsupp(host);
  auto supplier = ReadSupplier(host);
  auto nation = ReadNation(host);
  return tpchq11vgen_build(partsupp.size(), partsupp.data(), supplier.size(),
                           supplier.data(), nation.keys.size(), nation.keys.data(),
                           nation.names.bytes.data(), nation.names.off.data());
}

void FreeState(void *state) {
  tpchq11vgen_free(static_cast<tpchq11vgen_state *>(state));
}

struct NationDelta {
  std::vector<int64_t> keys;
  kit::TextColumn names;
  std::vector<int64_t> signs;
};

NationDelta MarshalNationDelta(const std::vector<kit::Change> &changes) {
  NationDelta delta;
  delta.keys.reserve(changes.size());
  delta.names.bytes.reserve(changes.size() * 8);
  delta.names.off.reserve(changes.size() + 1);
  delta.signs.reserve(changes.size());
  for (const auto &change : changes) {
    delta.keys.push_back(kit::I64(change.row, 0, 0));
    delta.names.Push(kit::Text(change.row, 1));
    delta.signs.push_back(change.sign);
  }
  return delta;
}

std::vector<tpchq11vgen_partsupp_delta>
MarshalPartsuppDelta(const std::vector<kit::Change> &changes) {
  std::vector<tpchq11vgen_partsupp_delta> rows;
  rows.reserve(changes.size());
  for (const auto &change : changes) {
    rows.push_back({{kit::I64(change.row, 0, 0), kit::I64(change.row, 1, 0),
                     kit::I64(change.row, 2, 0), kit::I64(change.row, 3, 2)},
                    change.sign});
  }
  return rows;
}

std::vector<tpchq11vgen_supplier_delta>
MarshalSupplierDelta(const std::vector<kit::Change> &changes) {
  std::vector<tpchq11vgen_supplier_delta> rows;
  rows.reserve(changes.size());
  for (const auto &change : changes) {
    rows.push_back({{kit::I64(change.row, 0, 0), kit::I64(change.row, 1, 0)},
                    change.sign});
  }
  return rows;
}

void *Apply(const void *state, const kit::Changes &changes, kit::Host &, int32_t *err) {
  if (changes.size() != 3) {
    *err = 1;
    return nullptr;
  }
  auto partsupp = MarshalPartsuppDelta(changes[0]);
  auto supplier = MarshalSupplierDelta(changes[1]);
  auto nation = MarshalNationDelta(changes[2]);
  return tpchq11vgen_apply(
      static_cast<const tpchq11vgen_state *>(state), nation.keys.size(),
      nation.keys.data(), nation.names.bytes.data(), nation.names.off.data(),
      nation.signs.data(), partsupp.size(), partsupp.data(), supplier.size(),
      supplier.data(), err);
}

template <class Call>
bool Run(const kit::core::QueryArguments &args, std::vector<kit::core::AuxRow> &out, Call call) {
  std::string nation_name;
  if (!kit::ArgText(args, 0, nation_name)) {
    return false;
  }
  int64_t fraction = 0; // $2 * 10^12, converted exactly (else decline)
  if (!kit::ArgI64(args, 1, 12, fraction)) {
    return false;
  }
  tpchq11vgen_result *result = call(reinterpret_cast<const uint8_t *>(nation_name.data()), nation_name.size(),
      fraction);
  if (result == nullptr) {
    return false;
  }
  const size_t count = tpchq11vgen_result_len(result);
  out.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    int64_t partkey = 0;
    if (!tpchq11vgen_result_c0(result, i, &partkey)) {
      out.push_back({std::vector<core::AuxValue>{
          kit::OutNull(core::AuxType::Int64), kit::OutDec(0, 0, 2)}});
      continue;
    }
    int64_t hi = 0;
    uint64_t lo = 0;
    if (!tpchq11vgen_result_c1(result, i, &hi, &lo)) {
      out.push_back({std::vector<core::AuxValue>{
          kit::OutI64(partkey),
          kit::OutNull(core::AuxType::ScaledDecimal)}});
      continue;
    }
    out.push_back({std::vector<core::AuxValue>{kit::OutI64(partkey),
                                              kit::OutDec(hi, lo, 2)}});
  }
  tpchq11vgen_result_free(result);
  return true;
}

bool Execute(const void *state, const kit::core::QueryArguments &args,
             std::vector<kit::core::AuxRow> &out) {
  return Run(args, out, [&](auto... x) {
    return tpchq11vgen_execute(static_cast<const tpchq11vgen_state *>(state), x...);
  });
}

bool Answer(const void *state, const kit::core::QueryArguments &args,
            std::vector<kit::core::AuxRow> &out) {
  return Run(args, out, [&](auto... x) {
    return tpchq11vgens_answer(static_cast<const tpchq11vgens_reading *>(state), x...);
  });
}

void *PrepareBuild(const kit::HeadFn &head_of, kit::Host &host) {
  auto partsupp = ReadPartsupp(host);
  auto supplier = ReadSupplier(host);
  auto nation = ReadNation(host);
  return tpchq11vgens_stage_build(
      static_cast<const tpchq11vgens_head *>(head_of()), partsupp.size(), partsupp.data(),
      supplier.size(), supplier.data(), nation.keys.size(), nation.keys.data(),
      nation.names.bytes.data(), nation.names.off.data());
}

void *PrepareApply(const void *base, const kit::Changes &changes,
                   kit::Host &, int32_t *err) {
  if (changes.size() != 3) {
    *err = 1;
    return nullptr;
  }
  auto partsupp = MarshalPartsuppDelta(changes[0]);
  auto supplier = MarshalSupplierDelta(changes[1]);
  auto nation = MarshalNationDelta(changes[2]);
  return tpchq11vgens_stage_apply(
      static_cast<const tpchq11vgens_base *>(base), nation.keys.size(),
      nation.keys.data(), nation.names.bytes.data(), nation.names.off.data(),
      nation.signs.data(), partsupp.size(), partsupp.data(), supplier.size(),
      supplier.data(), err);
}

const kit::Glue kGlue = {
    "tpch_q11_vgen",
    "tpch",
    11,
    kQuerySql,
    {{"partsupp", {"ps_partkey", "ps_suppkey", "ps_availqty", "ps_supplycost"}},
     {"supplier", {"s_suppkey", "s_nationkey"}},
     {"nation", {"n_nationkey", "n_name"}}},
    {kit::Kind::Text, kit::Kind::Dec},
    Build,
    FreeState,
    Apply,
    Execute,
    Answer,
    PrepareApply,
    PrepareBuild,
    kit::StoreOps{
        []() -> void * { return tpchq11vgens_new(); },
        [](void *store) { tpchq11vgens_free(static_cast<tpchq11vgens_store *>(store)); },
        [](const void *store) -> void * { return tpchq11vgens_newest_base(static_cast<const tpchq11vgens_store *>(store)); },
        [](const void *base) -> const void * { return tpchq11vgens_base_state(static_cast<const tpchq11vgens_base *>(base)); },
        [](void *base) { tpchq11vgens_base_free(static_cast<tpchq11vgens_base *>(base)); },
        [](const void *store) -> void * { return tpchq11vgens_newest_head(static_cast<const tpchq11vgens_store *>(store)); },
        [](void *head) { tpchq11vgens_head_free(static_cast<tpchq11vgens_head *>(head)); },
        [](const void *head) -> void * { return tpchq11vgens_stage_invalid(static_cast<const tpchq11vgens_head *>(head)); },
        [](void *staged) { tpchq11vgens_staged_free(static_cast<tpchq11vgens_staged *>(staged)); },
        [](const void *store, uint64_t from) -> int32_t { return tpchq11vgens_begin(static_cast<const tpchq11vgens_store *>(store), from); },
        [](const void *store) { tpchq11vgens_abort(static_cast<const tpchq11vgens_store *>(store)); },
        [](const void *store, void *staged, uint64_t from, uint64_t floor, int32_t end_commit) -> int32_t {
            return tpchq11vgens_install(static_cast<const tpchq11vgens_store *>(store), static_cast<tpchq11vgens_staged *>(staged), from, floor, end_commit);
        },
        [](const void *store, uint64_t at, uint64_t tries, int32_t *status) -> const void * {
            return tpchq11vgens_read(static_cast<const tpchq11vgens_store *>(store), at, tries, status);
        },
        [](const void *reading) { tpchq11vgens_reading_free(const_cast<tpchq11vgens_reading *>(static_cast<const tpchq11vgens_reading *>(reading))); },
        [](const void *store) { tpchq11vgens_invalidate_all(static_cast<const tpchq11vgens_store *>(store)); },
        [](const void *store) -> std::size_t { return tpchq11vgens_version_count(static_cast<const tpchq11vgens_store *>(store)); },
        []() -> std::size_t { return tpchq11vgens_heap_bytes(); },
        []() -> std::size_t { return tpchq11vgens_heap_peak_bytes(); },
        []() { tpchq11vgens_heap_reset_peak(); },
        [](const void *head, void *state) -> void * { return tpchq11vgens_stage_adopt(static_cast<const tpchq11vgens_head *>(head), static_cast<tpchq11vgen_state *>(state)); },
    },
};

const kit::Registrar kRegistrar(&kGlue);

} // namespace
