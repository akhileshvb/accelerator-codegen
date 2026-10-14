#include "tpch_q1_vgen_verus.h"
#include "substrate/verus/kit/verus_kit.hpp"

#include <memory>
#include <utility>
#include <vector>

namespace {

namespace kit = accelerator_codegen::verus_kit;
namespace core = accelerator_codegen::aux_state::core;

const char kQuerySql[] = R"SQL(-- TPC-H Q1 (catalog text), the DELTA day count as $1.
SELECT l_returnflag, l_linestatus,
       SUM(l_quantity) AS sum_qty,
       SUM(l_extendedprice) AS sum_base_price,
       SUM(l_extendedprice * (1 - l_discount)) AS sum_disc_price,
       SUM(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge,
       AVG(l_quantity) AS avg_qty,
       AVG(l_extendedprice) AS avg_price,
       AVG(l_discount) AS avg_disc,
       COUNT(*) AS count_order
FROM lineitem
WHERE l_shipdate <= DATE '1998-12-01' - $1 * INTERVAL '1' DAY
GROUP BY l_returnflag, l_linestatus
ORDER BY l_returnflag, l_linestatus
)SQL";

const char kBuildSql[] = R"SQL(SELECT l_quantity, l_extendedprice, l_discount, l_tax,
           l_returnflag, l_linestatus, l_shipdate
    FROM lineitem;)SQL";

struct LineitemColumns {
  std::vector<int64_t> quantity;
  std::vector<int64_t> extendedprice;
  std::vector<int64_t> discount;
  std::vector<int64_t> tax;
  kit::TextColumn returnflag;
  kit::TextColumn linestatus;
  std::vector<int32_t> shipdate;

  explicit LineitemColumns(std::size_t count = 0) {
    quantity.reserve(count);
    extendedprice.reserve(count);
    discount.reserve(count);
    tax.reserve(count);
    returnflag.bytes.reserve(count);
    returnflag.off.reserve(count + 1);
    linestatus.bytes.reserve(count);
    linestatus.off.reserve(count + 1);
    shipdate.reserve(count);
  }

  void Push(const core::AuxRow &row) {
    quantity.push_back(kit::I64(row, 0, 2));
    extendedprice.push_back(kit::I64(row, 1, 2));
    discount.push_back(kit::I64(row, 2, 2));
    tax.push_back(kit::I64(row, 3, 2));
    returnflag.Push(kit::Text(row, 4));
    linestatus.Push(kit::Text(row, 5));
    shipdate.push_back(kit::Date(row, 6));
  }
};

struct LineitemDelta {
  LineitemColumns rows;
  std::vector<int64_t> signs;

  explicit LineitemDelta(const kit::Changes &changes)
      : rows(changes.at(0).size()) {
    const auto &lineitem = changes.at(0);
    signs.reserve(lineitem.size());
    for (const auto &change : lineitem) {
      rows.Push(change.row);
      signs.push_back(change.sign);
    }
  }
};

void *Build(kit::Host &host) {
  LineitemColumns rows;
  host.Query(kBuildSql, [&](const core::AuxRow &row) { rows.Push(row); });
  return tpchq1vgen_build(
      rows.quantity.size(), rows.quantity.data(), rows.extendedprice.data(),
      rows.discount.data(), rows.tax.data(), rows.returnflag.bytes.data(),
      rows.returnflag.off.data(), rows.linestatus.bytes.data(),
      rows.linestatus.off.data(), rows.shipdate.data());
}

void FreeState(void *state) {
  tpchq1vgen_free(static_cast<tpchq1vgen_state *>(state));
}

void *Apply(const void *state, const kit::Changes &changes, kit::Host &,
            int32_t *err) {
  LineitemDelta delta(changes);
  const auto &rows = delta.rows;
  return tpchq1vgen_apply(
      static_cast<const tpchq1vgen_state *>(state), rows.quantity.size(),
      rows.quantity.data(), rows.extendedprice.data(), rows.discount.data(),
      rows.tax.data(), rows.returnflag.bytes.data(), rows.returnflag.off.data(),
      rows.linestatus.bytes.data(), rows.linestatus.off.data(),
      rows.shipdate.data(), delta.signs.data(), err);
}

void *PrepareBuild(const void *store, kit::Host &host) {
  LineitemColumns rows;
  host.Query(kBuildSql, [&](const core::AuxRow &row) { rows.Push(row); });
  return tpchq1vgens_prepare_build(
      static_cast<const tpchq1vgens_store *>(store), rows.quantity.size(),
      rows.quantity.data(), rows.extendedprice.data(), rows.discount.data(),
      rows.tax.data(), rows.returnflag.bytes.data(), rows.returnflag.off.data(),
      rows.linestatus.bytes.data(), rows.linestatus.off.data(),
      rows.shipdate.data());
}

void *PrepareApply(const void *store, const void *base,
                   const kit::Changes &changes, kit::Host &, int32_t *err) {
  LineitemDelta delta(changes);
  const auto &rows = delta.rows;
  return tpchq1vgens_prepare_apply(
      static_cast<const tpchq1vgens_store *>(store),
      static_cast<const tpchq1vgens_base *>(base), rows.quantity.size(),
      rows.quantity.data(), rows.extendedprice.data(), rows.discount.data(),
      rows.tax.data(), rows.returnflag.bytes.data(), rows.returnflag.off.data(),
      rows.linestatus.bytes.data(), rows.linestatus.off.data(),
      rows.shipdate.data(), delta.signs.data(), err);
}

using TextReader = uint8_t (*)(const tpchq1vgen_result *, std::size_t,
                              const uint8_t **, std::size_t *);
using DecimalReader = uint8_t (*)(const tpchq1vgen_result *, std::size_t,
                                 int64_t *, uint64_t *);
using RationalReader = uint8_t (*)(const tpchq1vgen_result *, std::size_t,
                                  int64_t *, uint64_t *, int64_t *, uint64_t *);

core::AuxValue ReadText(const tpchq1vgen_result *result, std::size_t index,
                        TextReader reader) {
  const uint8_t *bytes = nullptr;
  std::size_t length = 0;
  if (!reader(result, index, &bytes, &length)) {
    return kit::OutNull(core::AuxType::String);
  }
  return kit::OutText(bytes, length);
}

core::AuxValue ReadDecimal(const tpchq1vgen_result *result, std::size_t index,
                           DecimalReader reader, int scale) {
  int64_t high = 0;
  uint64_t low = 0;
  if (!reader(result, index, &high, &low)) {
    return kit::OutNull(core::AuxType::String);
  }
  return kit::OutDec(high, low, scale);
}

core::AuxValue ReadRational(const tpchq1vgen_result *result, std::size_t index,
                            RationalReader reader) {
  int64_t numerator_high = 0;
  uint64_t numerator_low = 0;
  int64_t denominator_high = 0;
  uint64_t denominator_low = 0;
  if (!reader(result, index, &numerator_high, &numerator_low,
              &denominator_high, &denominator_low)) {
    return kit::OutNull(core::AuxType::Double);
  }
  return kit::OutRat(numerator_high, numerator_low, denominator_high,
                     denominator_low);
}

bool Execute(const void *state, const core::QueryArguments &args,
             std::vector<core::AuxRow> &out) {
  int64_t day_offset = 0;
  if (!kit::ArgI64(args, 0, 0, day_offset)) {
    return false;
  }
  std::unique_ptr<tpchq1vgen_result, decltype(&tpchq1vgen_result_free)> result(
      tpchq1vgen_execute(static_cast<const tpchq1vgen_state *>(state), day_offset),
      &tpchq1vgen_result_free);
  if (!result) {
    return false;
  }
  const std::size_t count = tpchq1vgen_result_len(result.get());
  out.reserve(out.size() + count);
  for (std::size_t index = 0; index < count; ++index) {
    core::AuxRow row;
    row.values.reserve(10);
    row.values.push_back(ReadText(result.get(), index, tpchq1vgen_result_c0));
    row.values.push_back(ReadText(result.get(), index, tpchq1vgen_result_c1));
    row.values.push_back(ReadDecimal(result.get(), index, tpchq1vgen_result_c2, 2));
    row.values.push_back(ReadDecimal(result.get(), index, tpchq1vgen_result_c3, 2));
    row.values.push_back(ReadDecimal(result.get(), index, tpchq1vgen_result_c4, 4));
    row.values.push_back(ReadDecimal(result.get(), index, tpchq1vgen_result_c5, 6));
    row.values.push_back(ReadRational(result.get(), index, tpchq1vgen_result_c6));
    row.values.push_back(ReadRational(result.get(), index, tpchq1vgen_result_c7));
    row.values.push_back(ReadRational(result.get(), index, tpchq1vgen_result_c8));
    int64_t count_order = 0;
    row.values.push_back(tpchq1vgen_result_c9(result.get(), index, &count_order)
                             ? kit::OutI64(count_order)
                             : kit::OutNull(core::AuxType::Int64));
    out.push_back(std::move(row));
  }
  return true;
}

const kit::Glue kGlue = {
    "tpch_q1_vgen", "tpch", 1, kQuerySql,
    {{"lineitem", {"l_quantity", "l_extendedprice", "l_discount", "l_tax",
                   "l_returnflag", "l_linestatus", "l_shipdate"}}},
    {kit::Kind::Int},
    Build, FreeState, Apply, Execute, PrepareApply, PrepareBuild,
    kit::StoreOps{
        []() -> void * { return tpchq1vgens_new(); },
        [](void *store) {
          tpchq1vgens_free(static_cast<tpchq1vgens_store *>(store));
        },
        [](const void *store) -> void * {
          return tpchq1vgens_newest_base(static_cast<const tpchq1vgens_store *>(store));
        },
        [](const void *base) -> const void * {
          return tpchq1vgens_base_state(static_cast<const tpchq1vgens_base *>(base));
        },
        [](void *base) {
          tpchq1vgens_base_free(static_cast<tpchq1vgens_base *>(base));
        },
        [](const void *store) -> void * {
          return tpchq1vgens_prepare_invalid(static_cast<const tpchq1vgens_store *>(store));
        },
        [](void *prepared) {
          tpchq1vgens_prepared_free(static_cast<tpchq1vgens_prepared *>(prepared));
        },
        [](const void *store, uint64_t from) -> int32_t {
          return tpchq1vgens_begin(static_cast<const tpchq1vgens_store *>(store), from);
        },
        [](const void *store) {
          tpchq1vgens_abort(static_cast<const tpchq1vgens_store *>(store));
        },
        [](const void *store, void *prepared, uint64_t from, uint64_t floor,
           int32_t end_commit) -> int32_t {
          return tpchq1vgens_install(static_cast<const tpchq1vgens_store *>(store),
                                   static_cast<tpchq1vgens_prepared *>(prepared),
                                   from, floor, end_commit);
        },
        [](const void *store, uint64_t at, uint64_t tries,
           int32_t *status) -> const void * {
          return tpchq1vgens_read(static_cast<const tpchq1vgens_store *>(store),
                                at, tries, status);
        },
        [](const void *state) {
          tpchq1vgens_state_release(static_cast<const tpchq1vgen_state *>(state));
        },
        [](const void *store) {
          tpchq1vgens_invalidate_all(static_cast<const tpchq1vgens_store *>(store));
        },
        [](const void *store) -> std::size_t {
          return tpchq1vgens_version_count(static_cast<const tpchq1vgens_store *>(store));
        },
    },
};

const kit::Registrar kRegistrar(&kGlue);

}
