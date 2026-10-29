#include "tpch_q17_vgen_verus.h"
#include "substrate/verus/kit/verus_kit.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace kit = accelerator_codegen::verus_kit;
namespace core = accelerator_codegen::aux_state::core;

constexpr const char kQuerySql[] = R"SQL(-- TPC-H Q17 (catalog text), :brand as $1, :container as $2.
SELECT SUM(l_extendedprice) / 7.0 AS avg_yearly
FROM lineitem, part,
     (SELECT l_partkey AS agg_partkey, 0.2 * AVG(l_quantity) AS avg_quantity
      FROM lineitem GROUP BY l_partkey) part_agg
WHERE p_partkey = l_partkey AND agg_partkey = l_partkey
  AND p_brand = $1 AND p_container = $2
  AND l_quantity < avg_quantity
)SQL";

constexpr const char kLinesSql[] =
    "SELECT l_partkey, l_quantity, l_extendedprice FROM lineitem;";
constexpr const char kPartsSql[] =
    "SELECT p_partkey, p_brand, p_container FROM part;";

tpchq17vgen_line_input ReadLine(const core::AuxRow &row) {
  return {kit::I64(row, 0, 0), kit::I64(row, 1, 2), kit::I64(row, 2, 2)};
}

struct Parts {
  std::vector<int64_t> keys;
  kit::TextColumn brands;
  kit::TextColumn containers;

  void Reserve(std::size_t count) {
    keys.reserve(count);
    brands.off.reserve(count + 1);
    containers.off.reserve(count + 1);
  }

  void Push(const core::AuxRow &row) {
    keys.push_back(kit::I64(row, 0, 0));
    brands.Push(kit::Text(row, 1));
    containers.Push(kit::Text(row, 2));
  }
};

struct BuildInputs {
  std::vector<tpchq17vgen_line_input> lines;
  Parts parts;

  explicit BuildInputs(kit::Host &host) {
    host.Query(kLinesSql, [&](const core::AuxRow &row) {
      lines.push_back(ReadLine(row));
    });
    host.Query(kPartsSql, [&](const core::AuxRow &row) { parts.Push(row); });
  }
};

void *Build(kit::Host &host) {
  const BuildInputs inputs(host);
  return tpchq17vgen_build(
      inputs.lines.size(), inputs.lines.data(), inputs.parts.keys.size(),
      inputs.parts.keys.data(), inputs.parts.brands.bytes.data(),
      inputs.parts.brands.off.data(), inputs.parts.containers.bytes.data(),
      inputs.parts.containers.off.data());
}

void *PrepareBuild(const kit::HeadFn &head_of, kit::Host &host) {
  const BuildInputs inputs(host);
  return tpchq17vgens_stage_build(
      static_cast<const tpchq17vgens_head *>(head_of()), inputs.lines.size(),
      inputs.lines.data(), inputs.parts.keys.size(), inputs.parts.keys.data(),
      inputs.parts.brands.bytes.data(), inputs.parts.brands.off.data(),
      inputs.parts.containers.bytes.data(), inputs.parts.containers.off.data());
}

struct ApplyInputs {
  std::vector<tpchq17vgen_line_key_delta> lineitem_delta;
  Parts part_delta;
  std::vector<int64_t> part_signs;
  std::vector<int64_t> rescan_keys;
  std::vector<tpchq17vgen_line_input> rescan_rows;

  ApplyInputs(const tpchq17vgen_state *state, const kit::Changes &changes,
              kit::Host &host) {
    const auto &line_changes = changes.at(0);
    const auto &part_changes = changes.at(1);
    lineitem_delta.reserve(line_changes.size());
    for (const auto &change : line_changes) {
      lineitem_delta.push_back({kit::I64(change.row, 0, 0), change.sign});
    }
    part_delta.Reserve(part_changes.size());
    part_signs.reserve(part_changes.size());
    for (const auto &change : part_changes) {
      part_delta.Push(change.row);
      part_signs.push_back(change.sign);
    }

    rescan_keys.resize(lineitem_delta.size() + part_delta.keys.size());
    for (;;) {
      const std::size_t count = tpchq17vgen_rescan_lines_for_parts_keys(
          state, lineitem_delta.size(), lineitem_delta.data(),
          part_delta.keys.size(), part_delta.keys.data(),
          part_delta.brands.bytes.data(), part_delta.brands.off.data(),
          part_delta.containers.bytes.data(), part_delta.containers.off.data(),
          part_signs.data(), rescan_keys.data(), rescan_keys.size());
      if (count <= rescan_keys.size()) {
        rescan_keys.resize(count);
        break;
      }
      rescan_keys.resize(count);
    }
    kit::QueryKeys(host, kLinesSql, "l_partkey", rescan_keys,
                   [&](const core::AuxRow &row) {
                     rescan_rows.push_back(ReadLine(row));
                   });
  }
};

void *Apply(const void *state, const kit::Changes &changes, kit::Host &host,
            int32_t *err) {
  const auto *typed_state = static_cast<const tpchq17vgen_state *>(state);
  const ApplyInputs inputs(typed_state, changes, host);
  return tpchq17vgen_apply(
      typed_state, inputs.lineitem_delta.size(), inputs.lineitem_delta.data(),
      inputs.part_delta.keys.size(), inputs.part_delta.keys.data(),
      inputs.part_delta.brands.bytes.data(), inputs.part_delta.brands.off.data(),
      inputs.part_delta.containers.bytes.data(),
      inputs.part_delta.containers.off.data(), inputs.part_signs.data(),
      inputs.rescan_keys.size(), inputs.rescan_keys.data(),
      inputs.rescan_rows.size(), inputs.rescan_rows.data(), err);
}

void *PrepareApply(const void *base,
                   const kit::Changes &changes, kit::Host &host, int32_t *err) {
  const auto *typed_base = static_cast<const tpchq17vgens_base *>(base);
  const ApplyInputs inputs(tpchq17vgens_base_state(typed_base), changes, host);
  return tpchq17vgens_stage_apply(
      typed_base,
      inputs.lineitem_delta.size(), inputs.lineitem_delta.data(),
      inputs.part_delta.keys.size(), inputs.part_delta.keys.data(),
      inputs.part_delta.brands.bytes.data(), inputs.part_delta.brands.off.data(),
      inputs.part_delta.containers.bytes.data(),
      inputs.part_delta.containers.off.data(), inputs.part_signs.data(),
      inputs.rescan_keys.size(), inputs.rescan_keys.data(),
      inputs.rescan_rows.size(), inputs.rescan_rows.data(), err);
}

void FreeState(void *state) {
  tpchq17vgen_free(static_cast<tpchq17vgen_state *>(state));
}

template <class Call>
bool Run(const kit::core::QueryArguments &args, std::vector<kit::core::AuxRow> &out, Call call) {
  std::string brand;
  std::string container;
  if (!kit::ArgText(args, 0, brand) || !kit::ArgText(args, 1, container)) {
    return false;
  }
  std::unique_ptr<tpchq17vgen_result, decltype(&tpchq17vgen_result_free)> result(
      call(reinterpret_cast<const uint8_t *>(brand.data()),
                          brand.size(),
                          reinterpret_cast<const uint8_t *>(container.data()),
                          container.size()),
      &tpchq17vgen_result_free);
  if (!result) {
    return false;
  }
  const std::size_t count = tpchq17vgen_result_len(result.get());
  out.reserve(out.size() + count);
  for (std::size_t index = 0; index < count; ++index) {
    int64_t num_hi;
    uint64_t num_lo;
    int64_t den_hi;
    uint64_t den_lo;
    core::AuxRow row;
    if (tpchq17vgen_result_c0(result.get(), index, &num_hi, &num_lo, &den_hi,
                             &den_lo)) {
      row.values.push_back(kit::OutRat(num_hi, num_lo, den_hi, den_lo));
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
    return tpchq17vgen_execute(static_cast<const tpchq17vgen_state *>(state), x...);
  });
}

bool Answer(const void *state, const kit::core::QueryArguments &args,
            std::vector<kit::core::AuxRow> &out) {
  return Run(args, out, [&](auto... x) {
    return tpchq17vgens_answer(static_cast<const tpchq17vgens_reading *>(state), x...);
  });
}

const kit::Glue kGlue = {
    "tpch_q17_vgen", "tpch", 17, kQuerySql,
    {{"lineitem", {"l_partkey"}},
     {"part", {"p_partkey", "p_brand", "p_container"}}},
    {kit::Kind::Text, kit::Kind::Text},
    Build, FreeState, Apply, Execute, Answer, PrepareApply, PrepareBuild,
    kit::StoreOps{
        []() -> void * { return tpchq17vgens_new(); },
        [](void *store) { tpchq17vgens_free(static_cast<tpchq17vgens_store *>(store)); },
        [](const void *store) -> void * { return tpchq17vgens_newest_base(static_cast<const tpchq17vgens_store *>(store)); },
        [](const void *base) -> const void * { return tpchq17vgens_base_state(static_cast<const tpchq17vgens_base *>(base)); },
        [](void *base) { tpchq17vgens_base_free(static_cast<tpchq17vgens_base *>(base)); },
        [](const void *store) -> void * { return tpchq17vgens_newest_head(static_cast<const tpchq17vgens_store *>(store)); },
        [](void *head) { tpchq17vgens_head_free(static_cast<tpchq17vgens_head *>(head)); },
        [](const void *head) -> void * { return tpchq17vgens_stage_invalid(static_cast<const tpchq17vgens_head *>(head)); },
        [](void *staged) { tpchq17vgens_staged_free(static_cast<tpchq17vgens_staged *>(staged)); },
        [](const void *store, uint64_t from) -> int32_t { return tpchq17vgens_begin(static_cast<const tpchq17vgens_store *>(store), from); },
        [](const void *store) { tpchq17vgens_abort(static_cast<const tpchq17vgens_store *>(store)); },
        [](const void *store, void *staged, uint64_t from, uint64_t floor, int32_t end_commit) -> int32_t {
            return tpchq17vgens_install(static_cast<const tpchq17vgens_store *>(store), static_cast<tpchq17vgens_staged *>(staged), from, floor, end_commit);
        },
        [](const void *store, uint64_t at, uint64_t tries, int32_t *status) -> const void * {
            return tpchq17vgens_read(static_cast<const tpchq17vgens_store *>(store), at, tries, status);
        },
        [](const void *reading) { tpchq17vgens_reading_free(const_cast<tpchq17vgens_reading *>(static_cast<const tpchq17vgens_reading *>(reading))); },
        [](const void *store) { tpchq17vgens_invalidate_all(static_cast<const tpchq17vgens_store *>(store)); },
        [](const void *store) -> std::size_t { return tpchq17vgens_version_count(static_cast<const tpchq17vgens_store *>(store)); },
        [](const void *head, void *state) -> void * { return tpchq17vgens_stage_adopt(static_cast<const tpchq17vgens_head *>(head), static_cast<tpchq17vgen_state *>(state)); },
    },
};

const kit::Registrar kRegistrar(&kGlue);

}
