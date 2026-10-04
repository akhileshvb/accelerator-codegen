#include "tpch_q15_vgen_verus.h"
#include "accelerator_codegen/verus/verus_kit.hpp"

#include <cstdint>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace kit = accelerator_codegen::verus_kit;
using kit::Host;
using kit::Change;
using kit::Changes;
using kit::Kind;
using kit::TextColumn;
using AuxRow = kit::core::AuxRow;
using AuxType = kit::core::AuxType;
using QueryArguments = kit::core::QueryArguments;

constexpr const char kQuerySql[] =
    "-- TPC-H Q15 (catalog text), :start_date as $1, $2 = $1 + 3 months (derived end date: sql2verus cannot add months to a non-literal date).\n"
    "SELECT s_suppkey, s_name, s_address, s_phone, total_revenue\n"
    "FROM supplier, (SELECT l_suppkey AS supplier_no, SUM(l_extendedprice * (1 - l_discount)) AS total_revenue\n"
    "                FROM lineitem WHERE l_shipdate >= $1\n"
    "                  AND l_shipdate < $2\n"
    "                GROUP BY l_suppkey) revenue0\n"
    "WHERE s_suppkey = supplier_no\n"
    "  AND total_revenue = (SELECT MAX(total_revenue)\n"
    "                       FROM (SELECT l_suppkey AS supplier_no, SUM(l_extendedprice * (1 - l_discount)) AS total_revenue\n"
    "                             FROM lineitem WHERE l_shipdate >= $1\n"
    "                               AND l_shipdate < $2\n"
    "                             GROUP BY l_suppkey) revenue1)\n"
    "ORDER BY s_suppkey\n";

struct LineInput {
  std::vector<tpchq15vgen_line_input> rows;
};

struct SupplierInput {
  std::vector<int64_t> suppkey;
  TextColumn name;
  TextColumn address;
  TextColumn phone;
};

struct LineDelta {
  std::vector<tpchq15vgen_line_delta> rows;

  void reserve(std::size_t n) {
    rows.reserve(n);
  }
};

struct SupplierDelta {
  std::vector<int64_t> suppkey;
  TextColumn name;
  TextColumn address;
  TextColumn phone;
  std::vector<int64_t> sign;

  void reserve(std::size_t n) {
    suppkey.reserve(n);
    sign.reserve(n);
    name.bytes.reserve(n);
    name.off.reserve(n + 1);
    address.bytes.reserve(n);
    address.off.reserve(n + 1);
    phone.bytes.reserve(n);
    phone.off.reserve(n + 1);
  }
};

LineInput ReadLines(Host &h) {
  LineInput input;
  h.Query(
      "SELECT l_suppkey, l_extendedprice, l_discount, l_shipdate FROM lineitem;",
      [&](const AuxRow &row) {
        input.rows.push_back({kit::I64(row, 0, 0), kit::I64(row, 1, 2),
                              kit::I64(row, 2, 2), kit::Date(row, 3)});
      });
  return input;
}

SupplierInput ReadSuppliers(Host &h) {
  SupplierInput input;
  h.Query(
      "SELECT s_suppkey, s_name, s_address, s_phone FROM supplier;",
      [&](const AuxRow &row) {
        input.suppkey.push_back(kit::I64(row, 0, 0));
        input.name.Push(kit::Text(row, 1));
        input.address.Push(kit::Text(row, 2));
        input.phone.Push(kit::Text(row, 3));
      });
  return input;
}

LineDelta ReadLineDelta(const std::vector<Change> &changes) {
  LineDelta delta;
  delta.reserve(changes.size());
  for (const Change &change : changes) {
    tpchq15vgen_line_delta row{};
    row.row.l_suppkey = kit::I64(change.row, 0, 0);
    row.row.l_extendedprice = kit::I64(change.row, 1, 2);
    row.row.l_discount = kit::I64(change.row, 2, 2);
    row.row.l_shipdate = kit::Date(change.row, 3);
    row.sign = change.sign;
    delta.rows.push_back(row);
  }
  return delta;
}

SupplierDelta ReadSupplierDelta(const std::vector<Change> &changes) {
  SupplierDelta delta;
  delta.reserve(changes.size());
  for (const Change &change : changes) {
    delta.suppkey.push_back(kit::I64(change.row, 0, 0));
    delta.name.Push(kit::Text(change.row, 1));
    delta.address.Push(kit::Text(change.row, 2));
    delta.phone.Push(kit::Text(change.row, 3));
    delta.sign.push_back(change.sign);
  }
  return delta;
}

void *Build(Host &h) {
  LineInput lines = ReadLines(h);
  SupplierInput suppliers = ReadSuppliers(h);
  return tpchq15vgen_build(
      lines.rows.size(),
      lines.rows.data(),
      suppliers.suppkey.size(),
      suppliers.suppkey.data(),
      suppliers.name.bytes.data(),
      suppliers.name.off.data(),
      suppliers.address.bytes.data(),
      suppliers.address.off.data(),
      suppliers.phone.bytes.data(),
      suppliers.phone.off.data());
}

void FreeState(void *state) {
  tpchq15vgen_free(static_cast<tpchq15vgen_state *>(state));
}

void *Apply(const void *state, const Changes &changes, Host &, int32_t *err) {
  const LineDelta lines = ReadLineDelta(changes.at(0));
  const SupplierDelta suppliers = ReadSupplierDelta(changes.at(1));
  return tpchq15vgen_apply(
      static_cast<const tpchq15vgen_state *>(state),
      lines.rows.size(),
      lines.rows.data(),
      suppliers.suppkey.size(),
      suppliers.suppkey.data(),
      suppliers.name.bytes.data(),
      suppliers.name.off.data(),
      suppliers.address.bytes.data(),
      suppliers.address.off.data(),
      suppliers.phone.bytes.data(),
      suppliers.phone.off.data(),
      suppliers.sign.data(),
      err);
}

bool Execute(const void *state, const QueryArguments &args,
             std::vector<AuxRow> &out) {
  int32_t start_date = 0;
  int32_t end_date = 0;
  if (!kit::ArgDate(args, 0, start_date) || !kit::ArgDate(args, 1, end_date)) {
    return false;
  }
  tpchq15vgen_result *result = tpchq15vgen_execute(
      static_cast<const tpchq15vgen_state *>(state), start_date, end_date);
  if (result == nullptr) {
    return false;
  }
  const std::size_t n = tpchq15vgen_result_len(result);
  out.clear();
  out.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    AuxRow row;
    row.values.reserve(5);
    int64_t integer = 0;
    if (tpchq15vgen_result_c0(result, i, &integer)) {
      row.values.push_back(kit::OutI64(integer));
    } else {
      row.values.push_back(kit::OutNull(AuxType::Int64));
    }
    const uint8_t *text = nullptr;
    std::size_t len = 0;
    if (tpchq15vgen_result_c1(result, i, &text, &len)) {
      row.values.push_back(kit::OutText(text, len));
    } else {
      row.values.push_back(kit::OutNull(AuxType::String));
    }
    if (tpchq15vgen_result_c2(result, i, &text, &len)) {
      row.values.push_back(kit::OutText(text, len));
    } else {
      row.values.push_back(kit::OutNull(AuxType::String));
    }
    if (tpchq15vgen_result_c3(result, i, &text, &len)) {
      row.values.push_back(kit::OutText(text, len));
    } else {
      row.values.push_back(kit::OutNull(AuxType::String));
    }
    int64_t hi = 0;
    uint64_t lo = 0;
    if (tpchq15vgen_result_c4(result, i, &hi, &lo)) {
      row.values.push_back(kit::OutDec(hi, lo, 4));
    } else {
      row.values.push_back(kit::OutNull(AuxType::ScaledDecimal));
    }
    out.push_back(std::move(row));
  }
  tpchq15vgen_result_free(result);
  return true;
}

void *PrepareApply(const void *store, const void *base, const Changes &changes,
                   Host &, int32_t *err) {
  const LineDelta lines = ReadLineDelta(changes.at(0));
  const SupplierDelta suppliers = ReadSupplierDelta(changes.at(1));
  return tpchq15vgens_prepare_apply(
      static_cast<const tpchq15vgens_store *>(store),
      static_cast<const tpchq15vgens_base *>(base),
      lines.rows.size(),
      lines.rows.data(),
      suppliers.suppkey.size(),
      suppliers.suppkey.data(),
      suppliers.name.bytes.data(),
      suppliers.name.off.data(),
      suppliers.address.bytes.data(),
      suppliers.address.off.data(),
      suppliers.phone.bytes.data(),
      suppliers.phone.off.data(),
      suppliers.sign.data(),
      err);
}

void *PrepareBuild(const void *store, Host &h) {
  LineInput lines = ReadLines(h);
  SupplierInput suppliers = ReadSuppliers(h);
  return tpchq15vgens_prepare_build(
      static_cast<const tpchq15vgens_store *>(store),
      lines.rows.size(),
      lines.rows.data(),
      suppliers.suppkey.size(),
      suppliers.suppkey.data(),
      suppliers.name.bytes.data(),
      suppliers.name.off.data(),
      suppliers.address.bytes.data(),
      suppliers.address.off.data(),
      suppliers.phone.bytes.data(),
      suppliers.phone.off.data());
}

void *NewStore() {
  return tpchq15vgens_new();
}

void FreeStore(void *store) {
  tpchq15vgens_free(static_cast<tpchq15vgens_store *>(store));
}

void *NewestBase(const void *store) {
  return tpchq15vgens_newest_base(static_cast<const tpchq15vgens_store *>(store));
}

const void *BaseState(const void *base) {
  return tpchq15vgens_base_state(static_cast<const tpchq15vgens_base *>(base));
}

void BaseFree(void *base) {
  tpchq15vgens_base_free(static_cast<tpchq15vgens_base *>(base));
}

void *PrepareInvalid(const void *store) {
  return tpchq15vgens_prepare_invalid(static_cast<const tpchq15vgens_store *>(store));
}

void PreparedFree(void *prepared) {
  tpchq15vgens_prepared_free(static_cast<tpchq15vgens_prepared *>(prepared));
}

int32_t Begin(const void *store, uint64_t from) {
  return tpchq15vgens_begin(static_cast<const tpchq15vgens_store *>(store), from);
}

void Abort(const void *store) {
  tpchq15vgens_abort(static_cast<const tpchq15vgens_store *>(store));
}

int32_t Install(const void *store, void *prepared, uint64_t from, uint64_t floor,
                int32_t end_commit) {
  return tpchq15vgens_install(
      static_cast<const tpchq15vgens_store *>(store),
      static_cast<tpchq15vgens_prepared *>(prepared), from, floor, end_commit);
}

const void *Read(const void *store, uint64_t at, uint64_t tries, int32_t *status) {
  return tpchq15vgens_read(static_cast<const tpchq15vgens_store *>(store), at, tries, status);
}

void StateRelease(const void *state) {
  tpchq15vgens_state_release(static_cast<const tpchq15vgen_state *>(state));
}

void InvalidateAll(const void *store) {
  tpchq15vgens_invalidate_all(static_cast<const tpchq15vgens_store *>(store));
}

std::size_t VersionCount(const void *store) {
  return tpchq15vgens_version_count(static_cast<const tpchq15vgens_store *>(store));
}

} // namespace

namespace accelerator_codegen::verus_kit {

const Glue kGlue = {
    "tpch_q15_vgen",
    "tpch",
    15,
    kQuerySql,
    {{"lineitem", {"l_suppkey", "l_extendedprice", "l_discount", "l_shipdate"}},
     {"supplier", {"s_suppkey", "s_name", "s_address", "s_phone"}}},
    {Kind::Date, Kind::Date},
    ::Build,
    ::FreeState,
    ::Apply,
    ::Execute,
    ::PrepareApply,
    ::PrepareBuild,
    {::NewStore,
     ::FreeStore,
     ::NewestBase,
     ::BaseState,
     ::BaseFree,
     ::PrepareInvalid,
     ::PreparedFree,
     ::Begin,
     ::Abort,
     ::Install,
     ::Read,
     ::StateRelease,
     ::InvalidateAll,
     ::VersionCount},
};

const Registrar kRegistrar(&kGlue);

} // namespace accelerator_codegen::verus_kit
