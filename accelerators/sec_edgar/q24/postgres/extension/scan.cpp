// GENERATED BACKEND — PostgreSQL scans + runtime for SEC-EDGAR Q24 (module only).
//
// The state, the maintenance and the exactness contract are the engine-neutral
// SecEdgarQ24Module (sec_edgar/q24/common, shared with DuckDB). This file is
// the PostgreSQL binding: the InitialScan over SPI (filtered batch scans for
// Build, indexed per-triple lookups for pre DELETE / key UPDATE maintenance)
// and the per-backend Runtime.
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// The module header has no engine types, but keep the project-wide order:
// query headers before PostgreSQL headers.
#include "sec_edgar/q24/common/module.hpp"
#include "sec_edgar/q24/cost_model.hpp"
#include "sec_edgar/q24/layout.hpp"

#include <deque>
#include <stdexcept>
#include <string>

#include "backends/postgres/api/engine_runtime.hpp"
#include "backends/postgres/api/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

// Table ids named in the capture triggers (the template triggers; the SEC-EDGAR
// vocabulary of the PG template: sub=1 num=2 pre=3 tag=4).
constexpr core::TableId kNumTableId = 2;
constexpr core::TableId kPreTableId = 3;

// Chunk-granular cursor scan: the callback sees a whole fetched chunk, so
// string views taken from its tuples stay valid until the callback returns.
template <typename OnChunk>
void ScanChunks(const std::string &sql, OnChunk &&on_chunk) {
  ++t_table_scans;
  if (SPI_connect() != SPI_OK_CONNECT)
    throw std::runtime_error("SPI_connect failed");
  SPIPlanPtr plan = SPI_prepare(sql.c_str(), 0, nullptr);
  if (!plan) {
    SPI_finish();
    throw std::runtime_error("SPI_prepare failed");
  }
  Portal portal = SPI_cursor_open(nullptr, plan, nullptr, nullptr, true);
  if (!portal) {
    SPI_freeplan(plan);
    SPI_finish();
    throw std::runtime_error("SPI_cursor_open failed");
  }
  for (;;) {
    SPI_cursor_fetch(portal, true, 32768);
    if (SPI_processed == 0) {
      if (SPI_tuptable)
        SPI_freetuptable(SPI_tuptable);
      break;
    }
    on_chunk(SPI_tuptable, SPI_tuptable->tupdesc,
             static_cast<std::size_t>(SPI_processed));
    SPI_freetuptable(SPI_tuptable);
  }
  SPI_cursor_close(portal);
  SPI_freeplan(plan);
  SPI_finish();
}

// The database as this transaction leaves it (own writes included): used by
// the PRE_COMMIT maintenance lookups.
template <typename OnChunk>
void ScanChunksAfterOwnWrites(const std::string &sql, OnChunk &&on_chunk) {
  CommandCounterIncrement();
  PushActiveSnapshot(GetLatestSnapshot());
  try {
    ScanChunks(sql, std::forward<OnChunk>(on_chunk));
  } catch (...) {
    PopActiveSnapshot();
    throw;
  }
  PopActiveSnapshot();
}

// A nullable string column as a view into the tuple (varchar / text are
// stored as varlena bytes; any other type is rendered once into `owned`).
class StrReader {
public:
  query::Q24Str At(HeapTuple tuple, TupleDesc desc, int column) {
    bool is_null = false;
    const Datum datum = SPI_getbinval(tuple, desc, column + 1, &is_null);
    if (is_null)
      return query::Q24Str::Null();
    const Oid type = TupleDescAttr(desc, column)->atttypid;
    if (type == VARCHAROID || type == TEXTOID) {
      struct varlena *v = PG_DETOAST_DATUM_PACKED(datum);
      return query::Q24Str::Of(VARDATA_ANY(v), VARSIZE_ANY_EXHDR(v));
    }
    owned.push_back(DatumText(datum, type));
    return query::Q24Str::Of(owned.back());
  }
  void Reset() { owned.clear(); }

private:
  std::deque<std::string> owned;
};

double DoubleAt(HeapTuple tuple, TupleDesc desc, int column) {
  bool is_null = false;
  const Datum datum = SPI_getbinval(tuple, desc, column + 1, &is_null);
  if (is_null)
    return 0.0;
  if (TupleDescAttr(desc, column)->atttypid == FLOAT8OID)
    return DatumGetFloat8(datum);
  bool unused = false;
  return TypedDouble(tuple, desc, column, unused);
}

std::string SqlLiteral(const std::string &s) {
  std::string q = "E'";
  for (const char c : s) {
    if (c == '\'')
      q += "''";
    else if (c == '\\')
      q += "\\\\";
    else
      q += c;
  }
  return q + "'";
}

// num rows that can appear in some (year, uom) result: value, uom and ddate
// NOT NULL and ddate with month/day in [101, 1231] (see Q24DdateYear).
const char *kBucketablePredicate =
    "value IS NOT NULL AND uom IS NOT NULL AND ddate IS NOT NULL AND "
    "((ddate::bigint % 10000) + 10000) % 10000 BETWEEN 101 AND 1231";

class SecEdgarQ24Scan final : public query::SecEdgarQ24InitialScan {
public:
  void ScanNum(const std::function<void(const query::SecEdgarQ24NumInput &)>
                   &consumer) override {
    ScanTypedTuples(
        "SELECT adsh, tag, version, uom, value, ddate FROM num",
        [&](HeapTuple tuple, TupleDesc desc) {
          bool value_null = false;
          const double value = TypedDouble(tuple, desc, 4, value_null);
          consumer({TypedText(tuple, desc, 0), TypedText(tuple, desc, 1),
                    TypedText(tuple, desc, 2), TypedText(tuple, desc, 3), value,
                    value_null, static_cast<int32_t>(TypedInt(tuple, desc, 5))});
        });
  }

  void ScanPre(const std::function<void(const query::SecEdgarQ24PreInput &)>
                   &consumer) override {
    ScanTypedTuples("SELECT adsh, tag, version FROM pre",
                    [&](HeapTuple tuple, TupleDesc desc) {
                      consumer({TypedText(tuple, desc, 0),
                                TypedText(tuple, desc, 1),
                                TypedText(tuple, desc, 2)});
                    });
  }

  // The bucketable num rows without a pre match (the anti-join runs in the
  // executor; the module's own probe then finds nothing to filter). Rows with
  // a NULL key component never match pre and are kept, as in the base query.
  bool ScanQualifyingNumBatches(
      const std::function<void(const query::SecEdgarQ24NumBatch &)> &consumer)
      override {
    std::vector<query::Q24Str> a, t, v, u;
    std::vector<double> value;
    std::vector<int32_t> ddate;
    StrReader reader;
    ScanChunks(std::string("SELECT adsh, tag, version, value, uom, ddate FROM num n WHERE ") +
                   kBucketablePredicate +
                   " AND NOT EXISTS (SELECT 1 FROM pre p WHERE p.adsh = n.adsh AND "
                   "p.tag = n.tag AND p.version = n.version)",
               [&](SPITupleTable *table, TupleDesc desc, std::size_t n) {
                 reader.Reset();
                 a.resize(n);
                 t.resize(n);
                 v.resize(n);
                 u.resize(n);
                 value.resize(n);
                 ddate.resize(n);
                 for (std::size_t i = 0; i < n; ++i) {
                   HeapTuple tuple = table->vals[i];
                   a[i] = reader.At(tuple, desc, 0);
                   t[i] = reader.At(tuple, desc, 1);
                   v[i] = reader.At(tuple, desc, 2);
                   value[i] = DoubleAt(tuple, desc, 3);
                   u[i] = reader.At(tuple, desc, 4);
                   ddate[i] = static_cast<int32_t>(TypedInt(tuple, desc, 5));
                 }
                 consumer({n, a.data(), t.data(), v.data(), value.data(),
                           u.data(), ddate.data()});
               });
    return true;
  }

  bool ScanPreBatches(
      const std::function<void(const query::SecEdgarQ24PreBatch &)> &consumer)
      override {
    std::vector<query::Q24Str> a, t, v;
    StrReader reader;
    ScanChunks("SELECT adsh, tag, version FROM pre",
               [&](SPITupleTable *table, TupleDesc desc, std::size_t n) {
                 reader.Reset();
                 a.resize(n);
                 t.resize(n);
                 v.resize(n);
                 for (std::size_t i = 0; i < n; ++i) {
                   HeapTuple tuple = table->vals[i];
                   a[i] = reader.At(tuple, desc, 0);
                   t[i] = reader.At(tuple, desc, 1);
                   v[i] = reader.At(tuple, desc, 2);
                 }
                 consumer({n, a.data(), t.data(), v.data()});
               });
    return true;
  }

  // Post-transaction truth per (adsh, tag, version): one statement, one
  // equality-probe subquery per key (an index on the triple makes each an
  // index lookup; without one it degrades to scans).
  bool LoadTripleStates(const std::vector<query::SecEdgarQ24TripleKey> &keys,
                        std::vector<query::SecEdgarQ24TripleState> &out) override {
    out.assign(keys.size(), {});
    if (keys.empty())
      return true;
    std::string values;
    for (std::size_t i = 0; i < keys.size(); ++i) {
      if (i)
        values += ",";
      values += "(" + std::to_string(i) + "," + SqlLiteral(keys[i].adsh) + "," +
                SqlLiteral(keys[i].tag) + "," + SqlLiteral(keys[i].version) + ")";
    }
    // One row per (key, bucket): the pre row count repeats on each; a key
    // without num rows yields a single row with NULL bucket columns.
    const std::string sql =
        "SELECT k.i, (SELECT count(*) FROM pre p WHERE p.adsh = k.a AND "
        "p.tag = k.t AND p.version = k.v), x.y, x.u, x.c, x.s "
        "FROM (VALUES " + values + ") k(i, a, t, v) "
        "LEFT JOIN LATERAL (SELECT (q.dd - q.r) / 10000 AS y, q.u, count(*) AS c, "
        "sum(q.val) AS s FROM (SELECT n.ddate::bigint AS dd, "
        "((n.ddate::bigint % 10000) + 10000) % 10000 AS r, n.uom AS u, "
        "n.value AS val FROM num n WHERE n.adsh = k.a AND n.tag = k.t AND "
        "n.version = k.v AND n.value IS NOT NULL AND n.uom IS NOT NULL AND "
        "n.ddate IS NOT NULL) q WHERE q.r BETWEEN 101 AND 1231 "
        "GROUP BY 1, 2) x ON true";
    ScanChunksAfterOwnWrites(
        sql, [&](SPITupleTable *table, TupleDesc desc, std::size_t n) {
          StrReader reader;
          for (std::size_t r = 0; r < n; ++r) {
            HeapTuple tuple = table->vals[r];
            const auto index = static_cast<std::size_t>(TypedInt(tuple, desc, 0));
            if (index >= out.size())
              throw std::runtime_error("SEC-EDGAR Q24: bad triple lookup index");
            auto &state = out[index];
            state.pre_rows = TypedInt(tuple, desc, 1);
            bool is_null = false;
            SPI_getbinval(tuple, desc, 6, &is_null); // x.c
            if (is_null)
              continue;
            query::SecEdgarQ24TripleBucket b;
            b.year = static_cast<int32_t>(TypedInt(tuple, desc, 2));
            const query::Q24Str uom = reader.At(tuple, desc, 3);
            b.uom.assign(uom.data, uom.size);
            b.num_rows = TypedInt(tuple, desc, 4);
            b.num_sum = DoubleAt(tuple, desc, 5);
            state.buckets.push_back(std::move(b));
          }
        });
    return true;
  }
};

Runtime<query::SecEdgarQ24Module, query::SecEdgarQ24Config, SecEdgarQ24Scan> &
Q24Runtime() {
  // Capture only what the module reads: num without `footnote`, and pre's
  // (adsh, tag, version) — the layouts documented in common/module.hpp.
  static Runtime<query::SecEdgarQ24Module, query::SecEdgarQ24Config,
                 SecEdgarQ24Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_SEC_EDGAR_Q24_COST_MODEL",
              core::kSecEdgarQ24CostModel,
              core::SecEdgarQ24Layout::kAccelerator,
              query::SecEdgarQ24Config{kNumTableId,
                                       kPreTableId,
                                       {0, 1, 2, 3, 4, 5, 6, 7},
                                       {0, 6, 7}},
              {}, /*tune_build=*/true);
  return runtime;
}

} // namespace
} // namespace detail

namespace {

void ExecutePostgresSecEdgarQ24(FunctionCallInfo fcinfo) {
  // accel_sec_edgar_q24_aux() uses the defaults (2023, 'USD'); the two-argument
  // overload is not STRICT: a NULL argument reaches the module as NULL.
  core::QueryArguments arguments;
  if (PG_NARGS() >= 2) {
    arguments.values.push_back(
        PG_ARGISNULL(0) ? core::AuxValue::Null(core::AuxType::Int64)
                        : core::AuxValue::Of(core::AuxType::Int64,
                                             static_cast<int64_t>(PG_GETARG_INT64(0))));
    arguments.values.push_back(
        PG_ARGISNULL(1) ? core::AuxValue::Null(core::AuxType::String)
                        : core::AuxValue::Of(core::AuxType::String,
                                             detail::TextDatum(PG_GETARG_DATUM(1))));
  }
  detail::Q24Runtime().Execute(arguments, fcinfo);
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(PostgresQueryExecutor{
    "sec_edgar", 24, kPostgresVariantAux, &ExecutePostgresSecEdgarQ24});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
