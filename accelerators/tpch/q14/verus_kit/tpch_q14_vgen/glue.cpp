#include "tpch_q14_vgen_verus.h"
#include "substrate/verus/kit/verus_kit.hpp"

#include <memory>
#include <utility>
#include <vector>

namespace {
namespace kit = accelerator_codegen::verus_kit;
namespace core = accelerator_codegen::aux_state::core;

constexpr const char kQuerySql[] = R"SQL(-- TPC-H Q14 (catalog text), :start_date as $1, $2 = $1 + 1 month (derived end date: sql2verus cannot add months to a non-literal date).
SELECT 100.00 * SUM(CASE WHEN p_type LIKE 'PROMO%' THEN l_extendedprice * (1 - l_discount) ELSE 0 END)
       / SUM(l_extendedprice * (1 - l_discount)) AS promo_revenue
FROM lineitem, part
WHERE l_partkey = p_partkey
  AND l_shipdate >= $1 AND l_shipdate < $2
)SQL";

constexpr const char kPartsSql[] = "SELECT p_partkey, p_type FROM part";
constexpr const char kLinesSql[] =
    "SELECT l_partkey, l_extendedprice, l_discount, l_shipdate FROM lineitem WHERE EXISTS (SELECT 1 FROM part WHERE p_partkey = l_partkey)";
constexpr const char kLinesRescanSql[] =
    "SELECT l_partkey, l_extendedprice, l_discount, l_shipdate FROM lineitem";

tpchq14vgen_line_input ReadLine(const core::AuxRow &row) {
  return {kit::I64(row, 0, 0), kit::I64(row, 1, 2),
          kit::I64(row, 2, 2), kit::Date(row, 3)};
}

struct Parts {
  std::vector<int64_t> keys;
  kit::TextColumn types;

  void Push(const core::AuxRow &row) {
    keys.push_back(kit::I64(row, 0, 0));
    types.Push(kit::Text(row, 1));
  }
};

struct BuildInputs {
  Parts parts;
  std::vector<tpchq14vgen_line_input> lines;

  explicit BuildInputs(kit::Host &host) {
    host.Query(kPartsSql, [&](const core::AuxRow &row) { parts.Push(row); });
    host.Query(kLinesSql, [&](const core::AuxRow &row) {
      lines.push_back(ReadLine(row));
    });
  }
};

void *Build(kit::Host &host) {
  const BuildInputs inputs(host);
  return tpchq14vgen_build(
      inputs.parts.keys.size(), inputs.parts.keys.data(),
      inputs.parts.types.bytes.data(), inputs.parts.types.off.data(),
      inputs.lines.size(), inputs.lines.data());
}

void *PrepareBuild(const kit::HeadFn &head_of, kit::Host &host) {
  const BuildInputs inputs(host);
  return tpchq14vgens_stage_build(
      static_cast<const tpchq14vgens_head *>(head_of()),
      inputs.parts.keys.size(), inputs.parts.keys.data(),
      inputs.parts.types.bytes.data(), inputs.parts.types.off.data(),
      inputs.lines.size(), inputs.lines.data());
}

using RescanKeys = decltype(&tpchq14vgen_rescan_parts_for_lines_keys);

struct ApplyInputs {
  std::vector<tpchq14vgen_line_delta> line_delta;
  std::vector<tpchq14vgen_part_delta> part_delta;
  std::vector<int64_t> part_keys;
  Parts parts;
  std::vector<int64_t> line_keys;
  std::vector<tpchq14vgen_line_input> lines;

  std::vector<int64_t> GetKeys(const tpchq14vgen_state *state,
                               RescanKeys rescan, std::size_t capacity) const {
    std::vector<int64_t> keys(capacity);
    for (;;) {
      const std::size_t count = rescan(
          state, line_delta.size(), line_delta.data(), part_delta.size(),
          part_delta.data(), keys.data(), keys.size());
      if (count <= keys.size()) {
        keys.resize(count);
        return keys;
      }
      keys.resize(count);
    }
  }

  ApplyInputs(const tpchq14vgen_state *state, const kit::Changes &changes,
              kit::Host &host) {
    line_delta.reserve(changes.at(0).size());
    for (const auto &change : changes.at(0)) {
      line_delta.push_back({ReadLine(change.row), change.sign});
    }
    part_delta.reserve(changes.at(1).size());
    for (const auto &change : changes.at(1)) {
      const int64_t key = kit::I64(change.row, 0, 0);
      const auto &type = kit::Text(change.row, 1);
      const uint8_t is_promo = type.compare(0, 5, "PROMO") == 0 ? 1 : 0;
      part_delta.push_back({key, is_promo, change.sign});
    }
    part_keys = GetKeys(state, tpchq14vgen_rescan_parts_for_lines_keys,
                        line_delta.size());
    line_keys = GetKeys(state, tpchq14vgen_rescan_lines_for_parts_keys,
                        part_delta.size());
    parts.keys.reserve(part_keys.size());
    parts.types.off.reserve(part_keys.size() + 1);
    kit::QueryKeys(host, kPartsSql, "p_partkey", part_keys,
                   [&](const core::AuxRow &row) { parts.Push(row); });
    lines.reserve(line_keys.size());
    kit::QueryKeys(host, kLinesRescanSql, "l_partkey", line_keys,
                   [&](const core::AuxRow &row) {
                     lines.push_back(ReadLine(row));
                   });
  }
};

void *Apply(const void *state, const kit::Changes &changes, kit::Host &host,
            int32_t *err) {
  const auto *kernel_state = static_cast<const tpchq14vgen_state *>(state);
  const ApplyInputs inputs(kernel_state, changes, host);
  return tpchq14vgen_apply(
      kernel_state, inputs.line_delta.size(), inputs.line_delta.data(),
      inputs.part_delta.size(), inputs.part_delta.data(),
      inputs.part_keys.size(), inputs.part_keys.data(),
      inputs.parts.keys.size(), inputs.parts.keys.data(),
      inputs.parts.types.bytes.data(), inputs.parts.types.off.data(),
      inputs.line_keys.size(), inputs.line_keys.data(),
      inputs.lines.size(), inputs.lines.data(), err);
}

void *PrepareApply(const void *base,
                   const kit::Changes &changes, kit::Host &host, int32_t *err) {
  const auto *kernel_base = static_cast<const tpchq14vgens_base *>(base);
  const ApplyInputs inputs(tpchq14vgens_base_state(kernel_base), changes, host);
  return tpchq14vgens_stage_apply(
      kernel_base,
      inputs.line_delta.size(), inputs.line_delta.data(),
      inputs.part_delta.size(), inputs.part_delta.data(),
      inputs.part_keys.size(), inputs.part_keys.data(),
      inputs.parts.keys.size(), inputs.parts.keys.data(),
      inputs.parts.types.bytes.data(), inputs.parts.types.off.data(),
      inputs.line_keys.size(), inputs.line_keys.data(),
      inputs.lines.size(), inputs.lines.data(), err);
}

void FreeState(void *state) {
  tpchq14vgen_free(static_cast<tpchq14vgen_state *>(state));
}

template <class Call>
bool Run(const kit::core::QueryArguments &args, std::vector<kit::core::AuxRow> &out, Call call) {
  int32_t start_date;
  int32_t end_date;
  if (!kit::ArgDate(args, 0, start_date) ||
      !kit::ArgDate(args, 1, end_date)) {
    return false;
  }
  std::unique_ptr<tpchq14vgen_result, decltype(&tpchq14vgen_result_free)> result(
      call(start_date, end_date),
      &tpchq14vgen_result_free);
  if (!result) {
    return false;
  }
  const std::size_t count = tpchq14vgen_result_len(result.get());
  out.reserve(out.size() + count);
  for (std::size_t index = 0; index < count; ++index) {
    int64_t num_hi;
    uint64_t num_lo;
    int64_t den_hi;
    uint64_t den_lo;
    core::AuxRow row;
    if (tpchq14vgen_result_c0(result.get(), index, &num_hi, &num_lo,
                             &den_hi, &den_lo)) {
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
    return tpchq14vgen_execute(static_cast<const tpchq14vgen_state *>(state), x...);
  });
}

bool Answer(const void *state, const kit::core::QueryArguments &args,
            std::vector<kit::core::AuxRow> &out) {
  return Run(args, out, [&](auto... x) {
    return tpchq14vgens_answer(static_cast<const tpchq14vgens_reading *>(state), x...);
  });
}

const kit::Glue kGlue = {
    "tpch_q14_vgen", "tpch", 14, kQuerySql,
    {{"lineitem", {"l_partkey", "l_extendedprice", "l_discount", "l_shipdate"}},
     {"part", {"p_partkey", "p_type"}}},
    {kit::Kind::Date, kit::Kind::Date},
    Build, FreeState, Apply, Execute, Answer, PrepareApply, PrepareBuild,
    kit::StoreOps{
        []() -> void * { return tpchq14vgens_new(); },
        [](void *store) { tpchq14vgens_free(static_cast<tpchq14vgens_store *>(store)); },
        [](const void *store) -> void * { return tpchq14vgens_newest_base(static_cast<const tpchq14vgens_store *>(store)); },
        [](const void *base) -> const void * { return tpchq14vgens_base_state(static_cast<const tpchq14vgens_base *>(base)); },
        [](void *base) { tpchq14vgens_base_free(static_cast<tpchq14vgens_base *>(base)); },
        [](const void *store) -> void * { return tpchq14vgens_newest_head(static_cast<const tpchq14vgens_store *>(store)); },
        [](void *head) { tpchq14vgens_head_free(static_cast<tpchq14vgens_head *>(head)); },
        [](const void *head) -> void * { return tpchq14vgens_stage_invalid(static_cast<const tpchq14vgens_head *>(head)); },
        [](void *staged) { tpchq14vgens_staged_free(static_cast<tpchq14vgens_staged *>(staged)); },
        [](const void *store, uint64_t from) -> int32_t { return tpchq14vgens_begin(static_cast<const tpchq14vgens_store *>(store), from); },
        [](const void *store) { tpchq14vgens_abort(static_cast<const tpchq14vgens_store *>(store)); },
        [](const void *store, void *staged, uint64_t from, uint64_t floor, int32_t end_commit) -> int32_t {
            return tpchq14vgens_install(static_cast<const tpchq14vgens_store *>(store), static_cast<tpchq14vgens_staged *>(staged), from, floor, end_commit);
        },
        [](const void *store, uint64_t at, uint64_t tries, int32_t *status) -> const void * {
            return tpchq14vgens_read(static_cast<const tpchq14vgens_store *>(store), at, tries, status);
        },
        [](const void *reading) { tpchq14vgens_reading_free(const_cast<tpchq14vgens_reading *>(static_cast<const tpchq14vgens_reading *>(reading))); },
        [](const void *store) { tpchq14vgens_invalidate_all(static_cast<const tpchq14vgens_store *>(store)); },
        [](const void *store) -> std::size_t { return tpchq14vgens_version_count(static_cast<const tpchq14vgens_store *>(store)); },
    },
};

const kit::Registrar kRegistrar(&kGlue);
}
