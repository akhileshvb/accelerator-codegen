#include "tpch_q2_vgen_verus.h"
#include "accelerator_codegen/verus/verus_kit.hpp"

#include <malloc.h>
#include <memory>
#include <utility>
#include <vector>

namespace {

namespace kit = accelerator_codegen::verus_kit;
namespace core = accelerator_codegen::aux_state::core;

constexpr char kQuerySql[] = R"SQL(-- TPC-H Q2 (catalog text), :part_size as $1, :part_type_pattern ('%' || type) as $2, :region_name as $3, LIMIT 100.
SELECT s_acctbal, s_name, n_name, p_partkey, p_mfgr, s_address, s_phone, s_comment
FROM part, supplier, partsupp, nation, region
WHERE p_partkey = ps_partkey AND s_suppkey = ps_suppkey AND p_size = $1
  AND p_type LIKE $2 AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey
  AND r_name = $3
  AND ps_supplycost = (SELECT MIN(ps_supplycost) FROM partsupp, supplier, nation, region
                       WHERE p_partkey = ps_partkey AND s_suppkey = ps_suppkey AND s_nationkey = n_nationkey
                         AND n_regionkey = r_regionkey AND r_name = $3)
ORDER BY s_acctbal DESC, n_name, s_name, p_partkey
LIMIT 100
)SQL";

constexpr char kBrassPartsSql[] = R"SQL(SELECT p_partkey, p_mfgr, p_size, p_type
  FROM part;)SQL";

constexpr char kBrassPartsuppSql[] = R"SQL(SELECT ps_partkey, ps_suppkey, ps_supplycost
  FROM partsupp
  WHERE EXISTS (
    SELECT p_partkey FROM part
    WHERE p_partkey = ps_partkey
  );)SQL";

constexpr char kSuppliersSql[] = R"SQL(SELECT s_suppkey, s_name, s_address, s_nationkey, s_phone,
         s_acctbal, s_comment
  FROM supplier;)SQL";

constexpr char kNationsSql[] =
    "SELECT n_nationkey, n_name, n_regionkey FROM nation;";
constexpr char kRegionsSql[] = "SELECT r_regionkey, r_name FROM region;";

struct Parts {
  std::vector<int64_t> partkey;
  kit::TextColumn mfgr;
  std::vector<int64_t> size;
  kit::TextColumn type;

  void Reserve(std::size_t count) {
    partkey.reserve(count);
    mfgr.off.reserve(count + 1);
    size.reserve(count);
    type.off.reserve(count + 1);
  }

  void Push(const core::AuxRow &row) {
    partkey.push_back(kit::I64(row, 0, 0));
    mfgr.Push(kit::Text(row, 1));
    size.push_back(kit::I64(row, 2, 0));
    type.Push(kit::Text(row, 3));
  }
};

tpchq2vgen_brass_partsupp_scan PartsuppRow(const core::AuxRow &row) {
  return {kit::I64(row, 0, 0), kit::I64(row, 1, 0), kit::I64(row, 2, 2)};
}

struct Suppliers {
  std::vector<int64_t> suppkey;
  kit::TextColumn name;
  kit::TextColumn address;
  std::vector<int64_t> nationkey;
  kit::TextColumn phone;
  std::vector<int64_t> acctbal;
  kit::TextColumn comment;
  std::vector<int64_t> sign;

  void Reserve(std::size_t count) {
    suppkey.reserve(count);
    name.off.reserve(count + 1);
    address.off.reserve(count + 1);
    nationkey.reserve(count);
    phone.off.reserve(count + 1);
    acctbal.reserve(count);
    comment.off.reserve(count + 1);
    sign.reserve(count);
  }

  void Push(const core::AuxRow &row) {
    suppkey.push_back(kit::I64(row, 0, 0));
    name.Push(kit::Text(row, 1));
    address.Push(kit::Text(row, 2));
    nationkey.push_back(kit::I64(row, 3, 0));
    phone.Push(kit::Text(row, 4));
    acctbal.push_back(kit::I64(row, 5, 2));
    comment.Push(kit::Text(row, 6));
  }
};

struct Nations {
  std::vector<int64_t> nationkey;
  kit::TextColumn name;
  std::vector<int64_t> regionkey;
  std::vector<int64_t> sign;

  void Reserve(std::size_t count) {
    nationkey.reserve(count);
    name.off.reserve(count + 1);
    regionkey.reserve(count);
    sign.reserve(count);
  }

  void Push(const core::AuxRow &row) {
    nationkey.push_back(kit::I64(row, 0, 0));
    name.Push(kit::Text(row, 1));
    regionkey.push_back(kit::I64(row, 2, 0));
  }
};

struct Regions {
  std::vector<int64_t> regionkey;
  kit::TextColumn name;
  std::vector<int64_t> sign;

  void Reserve(std::size_t count) {
    regionkey.reserve(count);
    name.off.reserve(count + 1);
    sign.reserve(count);
  }

  void Push(const core::AuxRow &row) {
    regionkey.push_back(kit::I64(row, 0, 0));
    name.Push(kit::Text(row, 1));
  }
};

struct ScanInputs {
  Parts parts;
  std::vector<tpchq2vgen_brass_partsupp_scan> partsupp;
  Suppliers suppliers;
  Nations nations;
  Regions regions;

  explicit ScanInputs(kit::Host &host) {
    host.Query(kBrassPartsSql, [&](const core::AuxRow &row) { parts.Push(row); });
    host.Query(kBrassPartsuppSql, [&](const core::AuxRow &row) {
      partsupp.push_back(PartsuppRow(row));
    });
    host.Query(kSuppliersSql, [&](const core::AuxRow &row) { suppliers.Push(row); });
    host.Query(kNationsSql, [&](const core::AuxRow &row) { nations.Push(row); });
    host.Query(kRegionsSql, [&](const core::AuxRow &row) { regions.Push(row); });
  }

  template <typename Function> auto Call(Function function) const {
    return function(
        parts.partkey.size(), parts.partkey.data(), parts.mfgr.bytes.data(),
        parts.mfgr.off.data(), parts.size.data(), parts.type.bytes.data(),
        parts.type.off.data(), partsupp.size(), partsupp.data(),
        suppliers.suppkey.size(), suppliers.suppkey.data(), suppliers.name.bytes.data(),
        suppliers.name.off.data(), suppliers.address.bytes.data(), suppliers.address.off.data(),
        suppliers.nationkey.data(), suppliers.phone.bytes.data(), suppliers.phone.off.data(),
        suppliers.acctbal.data(), suppliers.comment.bytes.data(), suppliers.comment.off.data(),
        nations.nationkey.size(), nations.nationkey.data(), nations.name.bytes.data(),
        nations.name.off.data(), nations.regionkey.data(), regions.regionkey.size(),
        regions.regionkey.data(), regions.name.bytes.data(), regions.name.off.data());
  }
};

struct DeltaInputs {
  std::vector<tpchq2vgen_part_key_delta> parts;
  std::vector<tpchq2vgen_partsupp_delta> partsupp;
  Suppliers suppliers;
  Nations nations;
  Regions regions;

  explicit DeltaInputs(const kit::Changes &changes) {
    parts.reserve(changes.at(0).size());
    for (const auto &change : changes.at(0)) {
      parts.push_back({kit::I64(change.row, 0, 0), change.sign});
    }
    partsupp.reserve(changes.at(1).size());
    for (const auto &change : changes.at(1)) {
      partsupp.push_back({PartsuppRow(change.row), change.sign});
    }
    suppliers.Reserve(changes.at(2).size());
    for (const auto &change : changes.at(2)) {
      suppliers.Push(change.row);
      suppliers.sign.push_back(change.sign);
    }
    nations.Reserve(changes.at(3).size());
    for (const auto &change : changes.at(3)) {
      nations.Push(change.row);
      nations.sign.push_back(change.sign);
    }
    regions.Reserve(changes.at(4).size());
    for (const auto &change : changes.at(4)) {
      regions.Push(change.row);
      regions.sign.push_back(change.sign);
    }
  }

  template <typename Function> auto Call(Function function) const {
    return function(
        nations.nationkey.size(), nations.nationkey.data(), nations.name.bytes.data(),
        nations.name.off.data(), nations.regionkey.data(), nations.sign.data(),
        parts.size(), parts.data(), partsupp.size(), partsupp.data(),
        regions.regionkey.size(), regions.regionkey.data(), regions.name.bytes.data(),
        regions.name.off.data(), regions.sign.data(),
        suppliers.suppkey.size(), suppliers.suppkey.data(), suppliers.name.bytes.data(),
        suppliers.name.off.data(), suppliers.address.bytes.data(), suppliers.address.off.data(),
        suppliers.nationkey.data(), suppliers.phone.bytes.data(), suppliers.phone.off.data(),
        suppliers.acctbal.data(), suppliers.comment.bytes.data(), suppliers.comment.off.data(),
        suppliers.sign.data());
  }
};

template <typename Selector>
std::vector<int64_t> RescanKeys(const tpchq2vgen_state *state,
                              const DeltaInputs &deltas, Selector selector) {
  std::vector<int64_t> keys(deltas.parts.size() + deltas.partsupp.size());
  for (;;) {
    const auto count = deltas.Call([&](auto... args) {
      return selector(state, args..., keys.data(), keys.size());
    });
    if (count <= keys.size()) {
      keys.resize(count);
      return keys;
    }
    keys.resize(count);
  }
}

struct RescanInputs {
  std::vector<int64_t> part_keys;
  Parts parts;
  std::vector<int64_t> partsupp_keys;
  std::vector<tpchq2vgen_brass_partsupp_scan> partsupp;

  RescanInputs(const tpchq2vgen_state *state, const DeltaInputs &deltas, kit::Host &host)
      : part_keys(RescanKeys(state, deltas, tpchq2vgen_rescan_brass_parts_after_keys)),
        partsupp_keys(RescanKeys(state, deltas, tpchq2vgen_rescan_brass_partsupp_after_keys)) {
    parts.Reserve(part_keys.size());
    partsupp.reserve(partsupp_keys.size());
    kit::QueryKeys(host, kBrassPartsSql, "p_partkey", part_keys,
                   [&](const core::AuxRow &row) { parts.Push(row); });
    kit::QueryKeys(host, kBrassPartsuppSql, "ps_partkey", partsupp_keys,
                   [&](const core::AuxRow &row) { partsupp.push_back(PartsuppRow(row)); });
  }

  template <typename Function> auto Call(Function function) const {
    return function(
        part_keys.size(), part_keys.data(), parts.partkey.size(), parts.partkey.data(),
        parts.mfgr.bytes.data(), parts.mfgr.off.data(), parts.size.data(),
        parts.type.bytes.data(), parts.type.off.data(), partsupp_keys.size(), partsupp_keys.data(), partsupp.size(), partsupp.data());
  }
};

void *Build(kit::Host &host) {
  void *state = nullptr;
  {
    const ScanInputs scans(host);
    state = scans.Call(tpchq2vgen_build);
  }
  malloc_trim(0);
  return state;
}

void FreeState(void *state) {
  tpchq2vgen_free(static_cast<tpchq2vgen_state *>(state));
}

void *PrepareBuild(const void *store, kit::Host &host) {
  void *state = nullptr;
  {
    const ScanInputs scans(host);
    state = scans.Call([&](auto... args) {
      return tpchq2vgens_prepare_build(static_cast<const tpchq2vgens_store *>(store), args...);
    });
  }
  malloc_trim(0);
  return state;
}

void *Apply(const void *state, const kit::Changes &changes, kit::Host &host, int32_t *err) {
  const auto *kernel_state = static_cast<const tpchq2vgen_state *>(state);
  const DeltaInputs deltas(changes);
  const RescanInputs rescans(kernel_state, deltas, host);
  return deltas.Call([&](auto... delta_args) {
    return rescans.Call([&](auto... rescan_args) {
      return tpchq2vgen_apply(kernel_state, delta_args..., rescan_args..., err);
    });
  });
}

void *PrepareApply(const void *store, const void *base, const kit::Changes &changes,
                   kit::Host &host, int32_t *err) {
  const auto *kernel_base = static_cast<const tpchq2vgens_base *>(base);
  const DeltaInputs deltas(changes);
  const RescanInputs rescans(tpchq2vgens_base_state(kernel_base), deltas, host);
  return deltas.Call([&](auto... delta_args) {
    return rescans.Call([&](auto... rescan_args) {
      return tpchq2vgens_prepare_apply(static_cast<const tpchq2vgens_store *>(store),
                                      kernel_base, delta_args..., rescan_args..., err);
    });
  });
}

using TextGetter = uint8_t (*)(const tpchq2vgen_result *, size_t, const uint8_t **, size_t *);

core::AuxValue ResultText(const tpchq2vgen_result *result, std::size_t index,
                          TextGetter getter) {
  const uint8_t *bytes = nullptr;
  std::size_t length = 0;
  if (!getter(result, index, &bytes, &length)) {
    return kit::OutNull(core::AuxType::String);
  }
  return kit::OutText(bytes, length);
}

bool Execute(const void *state, const core::QueryArguments &args,
             std::vector<core::AuxRow> &out) {
  int64_t part_size = 0;
  std::string type_pattern;
  std::string region_name;
  if (!kit::ArgI64(args, 0, 0, part_size) || !kit::ArgText(args, 1, type_pattern) ||
      !kit::ArgText(args, 2, region_name)) {
    return false;
  }
  std::unique_ptr<tpchq2vgen_result, decltype(&tpchq2vgen_result_free)> result(
      tpchq2vgen_execute(static_cast<const tpchq2vgen_state *>(state), part_size,
                         reinterpret_cast<const uint8_t *>(type_pattern.data()), type_pattern.size(),
                         reinterpret_cast<const uint8_t *>(region_name.data()), region_name.size()),
      tpchq2vgen_result_free);
  if (!result) {
    return false;
  }
  const auto count = tpchq2vgen_result_len(result.get());
  out.reserve(out.size() + count);
  for (std::size_t index = 0; index < count; ++index) {
    core::AuxRow row;
    row.values.reserve(8);
    int64_t high = 0;
    uint64_t low = 0;
    if (tpchq2vgen_result_c0(result.get(), index, &high, &low)) {
      row.values.push_back(kit::OutDec(high, low, 2));
    } else {
      row.values.push_back(kit::OutNull(core::AuxType::ScaledDecimal));
    }
    row.values.push_back(ResultText(result.get(), index, tpchq2vgen_result_c1));
    row.values.push_back(ResultText(result.get(), index, tpchq2vgen_result_c2));
    int64_t partkey = 0;
    if (tpchq2vgen_result_c3(result.get(), index, &partkey)) {
      row.values.push_back(kit::OutI64(partkey));
    } else {
      row.values.push_back(kit::OutNull(core::AuxType::Int64));
    }
    row.values.push_back(ResultText(result.get(), index, tpchq2vgen_result_c4));
    row.values.push_back(ResultText(result.get(), index, tpchq2vgen_result_c5));
    row.values.push_back(ResultText(result.get(), index, tpchq2vgen_result_c6));
    row.values.push_back(ResultText(result.get(), index, tpchq2vgen_result_c7));
    out.push_back(std::move(row));
  }
  return true;
}

const kit::Glue kGlue = {
    "tpch_q2_vgen", "tpch", 2, kQuerySql,
    {{"part", {"p_partkey"}},
     {"partsupp", {"ps_partkey", "ps_suppkey", "ps_supplycost"}},
     {"supplier", {"s_suppkey", "s_name", "s_address", "s_nationkey", "s_phone", "s_acctbal", "s_comment"}},
     {"nation", {"n_nationkey", "n_name", "n_regionkey"}},
     {"region", {"r_regionkey", "r_name"}}},
    {kit::Kind::Int, kit::Kind::Text, kit::Kind::Text},
    Build, FreeState, Apply, Execute, PrepareApply, PrepareBuild,
    kit::StoreOps{
        []() -> void * { return tpchq2vgens_new(); },
        [](void *store) { tpchq2vgens_free(static_cast<tpchq2vgens_store *>(store)); },
        [](const void *store) -> void * {
          return tpchq2vgens_newest_base(static_cast<const tpchq2vgens_store *>(store));
        },
        [](const void *base) -> const void * {
          return tpchq2vgens_base_state(static_cast<const tpchq2vgens_base *>(base));
        },
        [](void *base) { tpchq2vgens_base_free(static_cast<tpchq2vgens_base *>(base)); },
        [](const void *store) -> void * {
          return tpchq2vgens_prepare_invalid(static_cast<const tpchq2vgens_store *>(store));
        },
        [](void *prepared) { tpchq2vgens_prepared_free(static_cast<tpchq2vgens_prepared *>(prepared)); },
        [](const void *store, uint64_t from) {
          return tpchq2vgens_begin(static_cast<const tpchq2vgens_store *>(store), from);
        },
        [](const void *store) { tpchq2vgens_abort(static_cast<const tpchq2vgens_store *>(store)); },
        [](const void *store, void *prepared, uint64_t from, uint64_t floor, int32_t end_commit) {
          return tpchq2vgens_install(static_cast<const tpchq2vgens_store *>(store),
                                    static_cast<tpchq2vgens_prepared *>(prepared), from, floor, end_commit);
        },
        [](const void *store, uint64_t at, uint64_t tries, int32_t *status) -> const void * {
          return tpchq2vgens_read(static_cast<const tpchq2vgens_store *>(store), at, tries, status);
        },
        [](const void *state) { tpchq2vgens_state_release(static_cast<const tpchq2vgen_state *>(state)); },
        [](const void *store) { tpchq2vgens_invalidate_all(static_cast<const tpchq2vgens_store *>(store)); },
        [](const void *store) {
          return tpchq2vgens_version_count(static_cast<const tpchq2vgens_store *>(store));
        }}};

const kit::Registrar kRegistrar(&kGlue);

}
