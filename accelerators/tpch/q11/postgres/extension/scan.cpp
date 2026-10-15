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

// ── Q11 result wire format (worker -> calling backend) ──────────────────────
// The shared-state worker returns its tuplestore to the backend tuple by
// tuple.  Q11 results are ~20k (partkey, numeric) rows, and shipping, copying
// and re-inserting each tuple dominated the read.  Instead the worker emits
// chunk tuples (bigint kMarker, <payload in the numeric slot>):
//   payload = u16 kMagic | u8 kind | u8 0 | u32 rows | records
//   kind kNarrow: per row varint(zigzag(partkey)), varint(zigzag(prev - value))
//                 (value in cents; prev = previous row's value, 0 first; all
//                 arithmetic wraps, so any int64 sequence round-trips)
//   kind kWide:   per row i64 partkey, u64 low, i64 high (beyond-int64 value)
// kMagic as the numeric header word has the "special" bits set with a payload
// longer than 2 bytes, which no valid numeric has, so a chunk is never
// mistaken for a regular row (and a regular row never for a chunk).  The
// backend decodes the chunks into ordinary rows before anything reads them.
namespace q11wire {

constexpr int64_t kMarker = std::numeric_limits<int64_t>::min();
constexpr uint16_t kMagic = 0xE5A5;
constexpr uint8_t kNarrow = 1;
constexpr uint8_t kWide = 2;
constexpr std::size_t kHeaderBytes = 8;
constexpr std::size_t kChunkBytes = 192 * 1024;

inline uint64_t ZigZag(int64_t v) {
  return (static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63);
}
inline int64_t UnZigZag(uint64_t u) {
  return static_cast<int64_t>((u >> 1) ^ (~(u & 1) + 1));
}
inline char *PutVarint(char *out, uint64_t v) {
  while (v >= 0x80) {
    *out++ = static_cast<char>(v | 0x80);
    v >>= 7;
  }
  *out++ = static_cast<char>(v);
  return out;
}
inline bool GetVarint(const unsigned char *&p, const unsigned char *end,
                      uint64_t &v) {
  v = 0;
  for (int shift = 0; shift < 64 && p < end; shift += 7) {
    const unsigned char b = *p++;
    v |= static_cast<uint64_t>(b & 0x7f) << shift;
    if (!(b & 0x80))
      return true;
  }
  return false;
}

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

// Chunks are assembled in one buffer: kHeaderBytes, then the records.
class ChunkWriter {
public:
  explicit ChunkWriter(ReturnSetInfo *rsinfo) : rsinfo_(rsinfo) {
    buf_.resize(VARHDRSZ + kHeaderBytes + kChunkBytes + 64);
  }
  // Room for one more record of `kind` (starting a new chunk if needed).
  char *Reserve(uint8_t kind) {
    if (kind != kind_ || pos_ >= kChunkBytes) {
      Flush();
      kind_ = kind;
      prev_ = 0;
    }
    return buf_.data() + VARHDRSZ + kHeaderBytes + pos_;
  }
  void Commit(char *end) {
    pos_ = static_cast<std::size_t>(end - (buf_.data() + VARHDRSZ + kHeaderBytes));
    ++rows_;
  }
  int64_t &Prev() { return prev_; }
  void Flush() {
    if (rows_ == 0)
      return;
    char *base = buf_.data();
    SET_VARSIZE(base, VARHDRSZ + kHeaderBytes + pos_);
    char *h = base + VARHDRSZ;
    const uint16_t magic = kMagic;
    std::memcpy(h, &magic, 2);
    h[2] = static_cast<char>(kind_);
    h[3] = 0;
    std::memcpy(h + 4, &rows_, 4);
    Datum values[2] = {Int64GetDatum(kMarker), PointerGetDatum(base)};
    bool nulls[2] = {false, false};
    tuplestore_putvalues(rsinfo_->setResult, rsinfo_->setDesc, values, nulls);
    pos_ = 0;
    rows_ = 0;
  }

private:
  ReturnSetInfo *rsinfo_;
  std::vector<char> buf_; // varlena header | chunk header | records
  std::size_t pos_ = 0;
  uint32_t rows_ = 0;
  uint8_t kind_ = 0;
  int64_t prev_ = 0;
};

void WritePacked(ReturnSetInfo *rsinfo,
                 const std::vector<query::TpchQ11OutputRow> &rows) {
  ChunkWriter writer(rsinfo);
  for (const auto &row : rows) {
    if (FitsInt64(row.value)) {
      char *p = writer.Reserve(kNarrow);
      const int64_t v = static_cast<int64_t>(row.value);
      int64_t &prev = writer.Prev();
      p = PutVarint(p, ZigZag(row.partkey));
      p = PutVarint(p, ZigZag(static_cast<int64_t>(static_cast<uint64_t>(prev) -
                                                   static_cast<uint64_t>(v))));
      prev = v;
      writer.Commit(p);
    } else {
      char *p = writer.Reserve(kWide);
      const uint64_t lo = static_cast<uint64_t>(static_cast<unsigned __int128>(row.value));
      const int64_t hi = static_cast<int64_t>(row.value >> 64);
      std::memcpy(p, &row.partkey, 8);
      std::memcpy(p + 8, &lo, 8);
      std::memcpy(p + 16, &hi, 8);
      writer.Commit(p + 24);
    }
  }
  writer.Flush();
}

// The chunk payload of a tuple, or false for a regular row.
bool ChunkOf(TupleTableSlot *slot, const unsigned char *&data,
             std::size_t &size) {
  slot_getallattrs(slot);
  if (slot->tts_isnull[0] || slot->tts_isnull[1] ||
      DatumGetInt64(slot->tts_values[0]) != kMarker)
    return false;
  const auto *v = reinterpret_cast<const struct varlena *>(
      DatumGetPointer(slot->tts_values[1]));
  if (VARATT_IS_EXTENDED(v) && !VARATT_IS_SHORT(v))
    return false; // compressed / external: never produced by WritePacked
  size = VARSIZE_ANY_EXHDR(v);
  data = reinterpret_cast<const unsigned char *>(VARDATA_ANY(v));
  uint16_t magic = 0;
  if (size < kHeaderBytes)
    return false;
  std::memcpy(&magic, data, sizeof(magic));
  return magic == kMagic;
}

void DecodeChunk(ReturnSetInfo *rsinfo, const std::string &chunk) {
  const auto *p = reinterpret_cast<const unsigned char *>(chunk.data());
  const unsigned char *end = p + chunk.size();
  const uint8_t kind = p[2];
  uint32_t rows = 0;
  std::memcpy(&rows, p + 4, sizeof(rows));
  p += kHeaderBytes;
  int64_t prev = 0;
  for (uint32_t i = 0; i < rows; ++i) {
    if (kind == kNarrow) {
      uint64_t zk = 0, zd = 0;
      if (!GetVarint(p, end, zk) || !GetVarint(p, end, zd))
        ereport(ERROR, (errmsg("accel_tpch_q11_aux: corrupt result chunk")));
      const int64_t value = static_cast<int64_t>(
          static_cast<uint64_t>(prev) - static_cast<uint64_t>(UnZigZag(zd)));
      prev = value;
      PutRow(rsinfo, UnZigZag(zk), value);
    } else if (kind == kWide && end - p >= 24) {
      int64_t partkey = 0, hi = 0;
      uint64_t lo = 0;
      std::memcpy(&partkey, p, 8);
      std::memcpy(&lo, p + 8, 8);
      std::memcpy(&hi, p + 16, 8);
      p += 24;
      const query::Q11AccT value = static_cast<query::Q11AccT>(
          (static_cast<unsigned __int128>(static_cast<uint64_t>(hi)) << 64) | lo);
      PutRow(rsinfo, partkey, value);
    } else {
      ereport(ERROR, (errmsg("accel_tpch_q11_aux: corrupt result chunk")));
    }
  }
}

// Replaces the worker's chunk tuples in the tuplestore by the rows they hold.
// A tuplestore of regular rows (or an empty one) is left as it is.
void UnpackRemote(ReturnSetInfo *rsinfo) {
  if (!rsinfo || !rsinfo->setResult || !rsinfo->setDesc)
    return;
  Tuplestorestate *store = rsinfo->setResult;
  TupleTableSlot *slot =
      MakeSingleTupleTableSlot(rsinfo->setDesc, &TTSOpsMinimalTuple);
  std::vector<std::string> chunks;
  bool regular = false;
  while (tuplestore_gettupleslot(store, true, false, slot)) {
    const unsigned char *data = nullptr;
    std::size_t size = 0;
    if (!ChunkOf(slot, data, size)) {
      regular = true;
      break;
    }
    chunks.emplace_back(reinterpret_cast<const char *>(data), size);
  }
  ExecDropSingleTupleTableSlot(slot);
  if (regular && !chunks.empty())
    ereport(ERROR, (errmsg("accel_tpch_q11_aux: mixed result chunks")));
  if (chunks.empty()) {
    tuplestore_rescan(store); // nothing to unpack: rewind for the executor
    return;
  }
  tuplestore_clear(store);
  for (const auto &chunk : chunks)
    DecodeChunk(rsinfo, chunk);
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
  // Result rows bypass the generic AuxRow sink (see TpchQ11ResultCollector):
  // they are written here with a direct numeric construction, and the
  // shared-state worker ships them to this backend as a few packed tuples
  // (~7 B/row) instead of one ~45 B tuple per row.
  namespace srv = accelerator_codegen::aux_state::engine::postgres::aux_server;
  static query::TpchQ11ResultCollector collector;
  collector.emitted = false;
  collector.rows.clear();
  query::TpchQ11ActiveCollector() = &collector;
  detail::Q11Runtime().Execute(
      core::QueryArguments{{std::move(nation), std::move(fraction)}}, fcinfo);
  query::TpchQ11ActiveCollector() = nullptr;
  auto *rsinfo = reinterpret_cast<ReturnSetInfo *>(fcinfo->resultinfo);
  if (collector.emitted) {
    if (srv::InServer())
      q11wire::WritePacked(rsinfo, collector.rows);
    else
      q11wire::WriteRows(rsinfo, collector.rows);
  } else if (!srv::InServer()) {
    // Served by the shared-state worker: its packed tuples (if any) are in
    // the tuplestore now.
    q11wire::UnpackRemote(rsinfo);
  }
  collector.rows.clear();
  if (collector.rows.capacity() > (std::size_t{1} << 18))
    collector.rows.shrink_to_fit();
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(PostgresQueryExecutor{
    "tpch", 11, kPostgresVariantAux, &ExecutePostgresTpchQ11});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
