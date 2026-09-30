// accel_tpch_q1_verus(day_offset): stateless TPC-H Q1 on PostgreSQL,
// aggregated by the Verus-verified kernel (src/verus/q1).
//
// TRUSTED GLUE.  One native heap scan of lineitem under the statement's
// active snapshot (table_beginscan + slot deforming, no SQL); each row's seven
// Q1 columns are decoded exactly and handed to the kernel in batches:
//   * NUMERIC is read from its on-disk base-10000 digits and converted to an
//     integer number of hundredths; a value that is not exactly representable
//     (more than 2 fractional digits, NaN / infinity, or beyond int64) makes
//     the entry point answer with SQL instead;
//   * DATE is the raw DateADT day number, and the cutoff is computed in the
//     same encoding;
//   * the flags must be exactly one byte (otherwise SQL).
// The result is formatted exactly as PostgreSQL does: SUM columns as NUMERIC
// at scales 2, 2, 4, 6 and AVG(numeric) at PostgreSQL's division scale
// (pg_numeric::AvgOfScaledSum).  The kernel orders groups byte-wise, so the
// entry point also requires l_returnflag / l_linestatus to have the C
// collation; kernel refusals, NULLs and the cases above fall back to the
// catalog SQL (run through SPI).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

extern "C" {
#include "postgres.h"

#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/tuplestore.h"
}

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "accelerator_codegen/stateful/core/pg_numeric.hpp"
#include "accelerator_codegen/verus/q1_verus.h"

namespace {

namespace pgn = accelerator_codegen::pg_numeric;
using accelerator_codegen::verus_q1::Q1Batch;
using accelerator_codegen::verus_q1::Q1Kernel;

constexpr const char *kQ1Sql =
    "SELECT l_returnflag, l_linestatus, sum(l_quantity) AS sum_qty, sum(l_extendedprice) AS sum_base_price, "
    "sum(l_extendedprice * (1 - l_discount)) AS sum_disc_price, "
    "sum(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge, avg(l_quantity) AS avg_qty, "
    "avg(l_extendedprice) AS avg_price, avg(l_discount) AS avg_disc, count(*) AS count_order "
    "FROM lineitem WHERE l_shipdate <= DATE '1998-12-01' - $1::integer "
    "GROUP BY l_returnflag, l_linestatus ORDER BY l_returnflag, l_linestatus";

// PostgreSQL NUMERIC on-disk header bits (utils/adt/numeric.c).
constexpr uint16_t kSignMask = 0xC000;
constexpr uint16_t kNeg = 0x4000;
constexpr uint16_t kShort = 0x8000;
constexpr uint16_t kSpecial = 0xC000;
constexpr uint16_t kShortSignMask = 0x2000;
constexpr uint16_t kShortWeightSignMask = 0x0040;
constexpr uint16_t kShortWeightMask = 0x003F;

// value * 100 as an exact int64, or false.
bool NumericHundredths(Datum datum, int64_t *out) {
  auto *v = reinterpret_cast<struct varlena *>(DatumGetPointer(datum));
  if (VARATT_IS_EXTENDED(v) && !VARATT_IS_SHORT(v)) {
    v = pg_detoast_datum_packed(v);
  }
  const char *p = VARDATA_ANY(v);
  const size_t len = VARSIZE_ANY_EXHDR(v);
  if (len < sizeof(uint16_t)) {
    return false;
  }
  uint16_t header;
  std::memcpy(&header, p, sizeof header);
  if ((header & kSignMask) == kSpecial) {
    return false; // NaN / +-Infinity
  }
  bool negative;
  int weight;
  const char *digits;
  size_t ndigits;
  if ((header & kSignMask) == kShort) {
    negative = (header & kShortSignMask) != 0;
    weight = (header & kShortWeightSignMask) ? static_cast<int>(~kShortWeightMask | (header & kShortWeightMask))
                                             : static_cast<int>(header & kShortWeightMask);
    digits = p + 2;
    ndigits = (len - 2) / 2;
  } else {
    if (len < 4) {
      return false;
    }
    negative = (header & kSignMask) == kNeg;
    int16_t w;
    std::memcpy(&w, p + 2, sizeof w);
    weight = w;
    digits = p + 4;
    ndigits = (len - 4) / 2;
  }
  __int128 ip = 0;   // integer part
  int64_t frac = 0;  // hundredths
  for (size_t i = 0; i < ndigits; i++) {
    int16_t d;
    std::memcpy(&d, digits + 2 * i, sizeof d);
    const int e = weight - static_cast<int>(i);
    if (e >= 0) {
      ip = ip * 10000 + d;
      if (ip > (static_cast<__int128>(1) << 62)) {
        return false;
      }
    } else if (e == -1) {
      if (d % 100 != 0) {
        return false;
      }
      frac = d / 100;
    } else if (d != 0) {
      return false;
    }
  }
  if (ndigits > 0) {
    for (int e = weight - static_cast<int>(ndigits) + 1; e > 0; e--) {
      ip *= 10000;
      if (ip > (static_cast<__int128>(1) << 62)) {
        return false;
      }
    }
  }
  const __int128 value = ip * 100 + frac;
  if (value > INT64_MAX) {
    return false;
  }
  *out = negative ? -static_cast<int64_t>(value) : static_cast<int64_t>(value);
  return true;
}

bool OneByte(Datum datum, uint8_t *out) {
  auto *v = reinterpret_cast<struct varlena *>(DatumGetPointer(datum));
  if (VARATT_IS_EXTENDED(v) && !VARATT_IS_SHORT(v)) {
    v = pg_detoast_datum_packed(v);
  }
  if (VARSIZE_ANY_EXHDR(v) != 1) {
    return false;
  }
  *out = static_cast<uint8_t>(VARDATA_ANY(v)[0]);
  return true;
}

Datum NumericDatum(const std::string &text) {
  return DirectFunctionCall3(numeric_in, CStringGetDatum(text.c_str()), ObjectIdGetDatum(InvalidOid),
                             Int32GetDatum(-1));
}

struct Columns {
  AttrNumber shipdate, quantity, extendedprice, discount, tax, returnflag, linestatus;
};

AttrNumber AttNum(Relation rel, const char *name) {
  TupleDesc desc = RelationGetDescr(rel);
  for (int i = 0; i < desc->natts; i++) {
    Form_pg_attribute att = TupleDescAttr(desc, i);
    if (!att->attisdropped && std::strcmp(NameStr(att->attname), name) == 0) {
      return att->attnum;
    }
  }
  return InvalidAttrNumber;
}

// Scan lineitem into the kernel.  Returns why it must answer with SQL, or
// nullptr.
const char *RunKernel(int32 day_offset, std::vector<q1v_group> &groups) {
  RangeVar *rv = makeRangeVar(nullptr, const_cast<char *>("lineitem"), -1);
  Relation rel = table_openrv(rv, AccessShareLock);
  Columns c{AttNum(rel, "l_shipdate"),  AttNum(rel, "l_quantity"), AttNum(rel, "l_extendedprice"),
            AttNum(rel, "l_discount"),  AttNum(rel, "l_tax"),      AttNum(rel, "l_returnflag"),
            AttNum(rel, "l_linestatus")};
  const AttrNumber all[] = {c.shipdate, c.quantity, c.extendedprice, c.discount, c.tax, c.returnflag,
                            c.linestatus};
  TupleDesc desc = RelationGetDescr(rel);
  const Oid types[] = {DATEOID, NUMERICOID, NUMERICOID, NUMERICOID, NUMERICOID, InvalidOid, InvalidOid};
  AttrNumber max_att = 0;
  const char *why = nullptr;
  for (int i = 0; i < 7 && !why; i++) {
    if (all[i] == InvalidAttrNumber) {
      why = "missing column";
      break;
    }
    Form_pg_attribute att = TupleDescAttr(desc, all[i] - 1);
    if (i < 5 && att->atttypid != types[i]) {
      why = "unexpected column type";
    }
    if (i >= 5 && ((att->atttypid != VARCHAROID && att->atttypid != TEXTOID && att->atttypid != BPCHAROID) ||
                   att->attcollation != C_COLLATION_OID)) {
      why = "flag column is not text with the C collation";
    }
    max_att = std::max(max_att, all[i]);
  }
  if (why) {
    table_close(rel, AccessShareLock);
    return why;
  }
  bool ok = true;

  const int32 cutoff = date2j(1998, 12, 1) - POSTGRES_EPOCH_JDATE - day_offset;
  Q1Kernel kernel(cutoff);
  Q1Batch batch(2048);
  TableScanDesc scan = table_beginscan(rel, GetActiveSnapshot(), 0, nullptr);
  TupleTableSlot *slot = table_slot_create(rel, nullptr);
  size_t n = 0;
  while (!why && table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
    slot_getsomeattrs(slot, max_att);
    const Datum *v = slot->tts_values;
    const bool *isnull = slot->tts_isnull;
    for (int i = 0; i < 7; i++) {
      if (isnull[all[i] - 1]) {
        why = "NULL in a Q1 column";
      }
    }
    if (why) {
      break;
    }
    batch.shipdate[n] = DatumGetDateADT(v[c.shipdate - 1]);
    if (!(NumericHundredths(v[c.quantity - 1], &batch.quantity[n]) &&
          NumericHundredths(v[c.extendedprice - 1], &batch.extendedprice[n]) &&
          NumericHundredths(v[c.discount - 1], &batch.discount[n]) &&
          NumericHundredths(v[c.tax - 1], &batch.tax[n]))) {
      why = "NUMERIC value not exactly representable in hundredths";
      break;
    }
    if (!(OneByte(v[c.returnflag - 1], &batch.returnflag[n]) && OneByte(v[c.linestatus - 1], &batch.linestatus[n]))) {
      why = "flag is not exactly one byte";
      break;
    }
    if (++n == batch.Capacity()) {
      batch.n = n;
      if (!kernel.Consume(batch)) {
        why = "kernel refused (row outside its numeric domain)";
      }
      n = 0;
      CHECK_FOR_INTERRUPTS();
    }
  }
  if (!why && n > 0) {
    batch.n = n;
    if (!kernel.Consume(batch)) {
      why = "kernel refused (row outside its numeric domain)";
    }
  }
  ExecDropSingleTupleTableSlot(slot);
  table_endscan(scan);
  table_close(rel, AccessShareLock);
  if (!why && !kernel.Finish(groups)) {
    why = "kernel refused";
  }
  return why;
}

void EmitKernelRows(ReturnSetInfo *rsinfo, const std::vector<q1v_group> &groups) {
  for (const auto &g : groups) {
    std::string avg_qty, avg_price, avg_disc;
    try {
      avg_qty = pgn::ToText(pgn::AvgOfScaledSum(g.sum_qty, 2, g.count));
      avg_price = pgn::ToText(pgn::AvgOfScaledSum(g.sum_base_price, 2, g.count));
      avg_disc = pgn::ToText(pgn::AvgOfScaledSum(g.sum_disc, 2, g.count));
    } catch (const std::exception &e) {
      ereport(ERROR, (errmsg("accel_tpch_q1_verus: %s", e.what())));
    }
    const char rf[2] = {static_cast<char>(g.returnflag), 0};
    const char ls[2] = {static_cast<char>(g.linestatus), 0};
    Datum values[10] = {
        CStringGetTextDatum(rf),
        CStringGetTextDatum(ls),
        NumericDatum(pgn::ToText(g.sum_qty, 2)),
        NumericDatum(pgn::ToText(g.sum_base_price, 2)),
        NumericDatum(pgn::ToText(g.sum_disc_price, 4)),
        NumericDatum(pgn::ToText(g.sum_charge, 6)),
        NumericDatum(avg_qty),
        NumericDatum(avg_price),
        NumericDatum(avg_disc),
        Int64GetDatum(g.count),
    };
    bool nulls[10] = {};
    tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
  }
}

void EmitSqlRows(ReturnSetInfo *rsinfo, int32 day_offset) {
  if (SPI_connect() != SPI_OK_CONNECT) {
    ereport(ERROR, (errmsg("accel_tpch_q1_verus: SPI_connect failed")));
  }
  Oid argtypes[1] = {INT4OID};
  Datum args[1] = {Int32GetDatum(day_offset)};
  if (SPI_execute_with_args(kQ1Sql, 1, argtypes, args, nullptr, true, 0) != SPI_OK_SELECT) {
    ereport(ERROR, (errmsg("accel_tpch_q1_verus: fallback SQL failed")));
  }
  for (uint64 r = 0; r < SPI_processed; r++) {
    Datum values[10];
    bool nulls[10];
    for (int col = 0; col < 10; col++) {
      values[col] = SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, col + 1, &nulls[col]);
    }
    tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
  }
  SPI_finish();
}

} // namespace

extern "C" {

PG_FUNCTION_INFO_V1(AcceleratorCodegenTpchQ1Verus);
Datum AcceleratorCodegenTpchQ1Verus(PG_FUNCTION_ARGS) {
  const int32 day_offset = PG_GETARG_INT32(0);
  InitMaterializedSRF(fcinfo, 0);
  auto *rsinfo = reinterpret_cast<ReturnSetInfo *>(fcinfo->resultinfo);
  std::vector<q1v_group> groups;
  if (const char *why = RunKernel(day_offset, groups)) {
    ereport(DEBUG1, (errmsg("accel_tpch_q1_verus: answering with SQL: %s", why)));
    EmitSqlRows(rsinfo, day_offset);
  } else {
    ereport(DEBUG1, (errmsg("accel_tpch_q1_verus: answered by the verified kernel")));
    EmitKernelRows(rsinfo, groups);
  }
  PG_RETURN_NULL();
}

} // extern "C"

#endif
