// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q11 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q11/postgres/aux/module.hpp"
#include "tpch/q11/cost_model.hpp"


#include "tpch/common/postgres_tpch_table_ids.hpp"
#include "backends/postgres/api/engine_runtime.hpp"
#include "backends/postgres/api/runtime.hpp"
#include "prims/pg_numeric.hpp"

#include <cstring>
#include <limits>

extern "C" {
#include "executor/tuptable.h"
#include "utils/tuplestore.h"
}

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

namespace pg_tbl = accelerator_codegen::aux_state::engine::postgres::tpch_tables;

namespace scans {

void ScanTpchQ11Partsupp(
    const std::function<void(const query::TpchQ11PartsuppInput &)> &consumer) {
  ScanColumns("partsupp",
              {IntColumn("ps_partkey"), IntColumn("ps_suppkey"),
               IntColumn("ps_availqty"), DecimalColumn("ps_supplycost", 2)},
              [&](const BuildRow &row) {
                consumer({row.Int(0),
                          row.Int(1),
                          row.Int(2),
                          row.Decimal(3), 2});
              });
}

void ScanTpchQ11Suppliers(
    const std::function<void(const query::TpchQ11SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_nationkey FROM supplier",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedInt(tuple, tuple_desc, 1)});
      });
}

void ScanTpchQ11Nations(
    const std::function<void(const query::TpchQ11NationInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT n_nationkey, n_name FROM nation",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1)});
      });
}

} // namespace scans

class TpchQ11Scan final : public query::TpchQ11InitialScan {
public:
  void ScanPartsupp(
      const std::function<void(const query::TpchQ11PartsuppInput &)> &consumer)
      override {
    scans::ScanTpchQ11Partsupp(consumer);
  }
  void ScanSuppliers(
      const std::function<void(const query::TpchQ11SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ11Suppliers(consumer);
  }
  void ScanNations(
      const std::function<void(const query::TpchQ11NationInput &)> &consumer)
      override {
    scans::ScanTpchQ11Nations(consumer);
  }
};
Runtime<query::TpchQ11Module, query::TpchQ11Config, TpchQ11Scan> &
Q11Runtime() {
  static Runtime<query::TpchQ11Module, query::TpchQ11Config, TpchQ11Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q11_COST_MODEL",
              core::kTpchQ11CostModel,
              query::TpchQ11Layout::kAccelerator,
              query::TpchQ11Config{pg_tbl::kPartsupp, pg_tbl::kSupplier, pg_tbl::kNation});
  return runtime;
}
} // namespace
} // namespace detail

namespace core = accelerator_codegen::aux_state::core;

namespace {

// ── Q11 fallback result rows: direct numeric construction ───────────────────
namespace q11wire {

inline bool FitsInt64(query::Q11AccT v) {
  return v >= std::numeric_limits<int64_t>::min() &&
         v <= std::numeric_limits<int64_t>::max();
}

// numeric(cents / 100) built in place: the bytes make_result() produces for
// a value of display scale 2 (short format; base-10000 digits, leading and
// trailing zero digits dropped, zero = positive with weight 0).  Avoids the
// ~4 pallocs of int64_div_fast_to_numeric per row.  CentsFormatVerified()
// checks it against PostgreSQL's own construction once per process and the
// rows fall back to int64_div_fast_to_numeric if anything differs.
struct CentsNumeric {
  alignas(int32) char bytes[VARHDRSZ + 2 + 8 * 2];
};

void BuildCents(int64_t cents, CentsNumeric &out) {
  const bool negative = cents < 0;
  uint64_t mag = negative ? static_cast<uint64_t>(-(cents + 1)) + 1
                          : static_cast<uint64_t>(cents);
  uint16_t groups[8]; // base-10000 digits, least significant first
  int n = 0;
  groups[n++] = static_cast<uint16_t>((mag % 100) * 100); // fraction digit
  for (uint64_t ip = mag / 100; ip != 0; ip /= 10000)
    groups[n++] = static_cast<uint16_t>(ip % 10000);
  int weight = n - 2; // groups[n-1] is the most significant digit
  int hi = n - 1, lo = 0;
  while (hi >= lo && groups[hi] == 0) { // leading zeros (only when ip == 0)
    --hi;
    --weight;
  }
  while (lo <= hi && groups[lo] == 0) // trailing zeros
    ++lo;
  const int ndigits = hi >= lo ? hi - lo + 1 : 0;
  bool neg = negative;
  if (ndigits == 0) {
    weight = 0;
    neg = false;
  }
  constexpr uint16_t kShort = 0x8000, kShortSign = 0x2000;
  constexpr int kDscaleShift = 7;
  constexpr uint16_t kWeightSign = 0x0040, kWeightMask = 0x003F;
  const uint16_t header = static_cast<uint16_t>(
      (neg ? (kShort | kShortSign) : kShort) | (2 << kDscaleShift) |
      (weight < 0 ? kWeightSign : 0) | (weight & kWeightMask));
  char *p = out.bytes + VARHDRSZ;
  std::memcpy(p, &header, 2);
  p += 2;
  for (int i = hi; i >= lo; --i, p += 2)
    std::memcpy(p, &groups[i], 2);
  SET_VARSIZE(out.bytes, VARHDRSZ + 2 + 2 * ndigits);
}

bool CentsFormatVerified() {
  static const bool verified = [] {
    const int64_t probes[] = {0, 1, -1, 5, 10, 99, 100, 101, -100, 1000, 9999,
                              10000, 10001, 999999, 1000000, 100000000,
                              123456789012, -123456789012, 4200000000000000,
                              std::numeric_limits<int64_t>::max(),
                              std::numeric_limits<int64_t>::min(),
                              std::numeric_limits<int64_t>::min() + 1};
    for (const int64_t v : probes) {
      CentsNumeric mine;
      BuildCents(v, mine);
      Numeric ref = int64_div_fast_to_numeric(v, 2);
      const bool same = VARSIZE(ref) == VARSIZE(mine.bytes) &&
                        std::memcmp(ref, mine.bytes, VARSIZE(ref)) == 0;
      pfree(ref);
      if (!same) {
        elog(WARNING, "accel_tpch_q11_aux: numeric fast path disabled");
        return false;
      }
    }
    return true;
  }();
  return verified;
}

void PutRow(ReturnSetInfo *rsinfo, int64_t partkey, query::Q11AccT value) {
  Datum values[2];
  bool nulls[2] = {false, false};
  values[0] = Int64GetDatum(partkey);
  if (FitsInt64(value) && CentsFormatVerified()) {
    CentsNumeric cents;
    BuildCents(static_cast<int64_t>(value), cents);
    values[1] = PointerGetDatum(cents.bytes);
    tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
    return;
  }
  if (FitsInt64(value)) {
    values[1] = NumericGetDatum(
        int64_div_fast_to_numeric(static_cast<int64_t>(value), 2));
  } else {
    const std::string text = pg_numeric::ToText(value, 2);
    values[1] =
        DirectFunctionCall3(numeric_in, CStringGetDatum(text.c_str()),
                            ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
  }
  tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
  pfree(DatumGetPointer(values[1]));
}

void WriteRows(ReturnSetInfo *rsinfo,
               const std::vector<query::TpchQ11OutputRow> &rows) {
  for (const auto &row : rows)
    PutRow(rsinfo, row.partkey, row.value);
}

} // namespace q11wire

void ExecutePostgresTpchQ11(FunctionCallInfo fcinfo) {
  // The SQL function is not STRICT: a NULL argument reaches the module as a
  // NULL AuxValue.  The fraction arrives as numeric (passed on as its exact
  // decimal text) or as float8.
  core::AuxValue nation =
      PG_ARGISNULL(0)
          ? core::AuxValue::Null(core::AuxType::String)
          : core::AuxValue::Of(core::AuxType::String,
                               detail::TextDatum(PG_GETARG_DATUM(0)));
  core::AuxValue fraction = core::AuxValue::Null(core::AuxType::String);
  if (!PG_ARGISNULL(1)) {
    if (get_fn_expr_argtype(fcinfo->flinfo, 1) == NUMERICOID) {
      char *text = DatumGetCString(
          DirectFunctionCall1(numeric_out, PG_GETARG_DATUM(1)));
      fraction = core::AuxValue::Of(core::AuxType::String, std::string(text));
      pfree(text);
    } else {
      fraction = core::AuxValue::Of(core::AuxType::Double, PG_GETARG_FLOAT8(1));
    }
  }
  // The fallback's result rows bypass the generic AuxRow sink (see
  // TpchQ11ResultCollector): they are written here with a direct numeric
  // construction.
  static query::TpchQ11ResultCollector collector;
  collector.emitted = false;
  collector.rows.clear();
  query::TpchQ11ActiveCollector() = &collector;
  detail::Q11Runtime().Execute(
      core::QueryArguments{{std::move(nation), std::move(fraction)}}, fcinfo);
  query::TpchQ11ActiveCollector() = nullptr;
  if (collector.emitted) {
    q11wire::WriteRows(reinterpret_cast<ReturnSetInfo *>(fcinfo->resultinfo),
                       collector.rows);
  }
  collector.rows.clear();
  if (collector.rows.capacity() > (std::size_t{1} << 18))
    collector.rows.shrink_to_fit();
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(PostgresQueryExecutor{
    "tpch", 11, kPostgresVariantAux, &ExecutePostgresTpchQ11});
const detail::RuntimeRegistrar<&detail::Q11Runtime> kAcceleratorRegistrar(
    query::TpchQ11Layout::kAccelerator);

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
