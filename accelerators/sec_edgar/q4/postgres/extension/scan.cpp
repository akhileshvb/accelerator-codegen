// GENERATED BACKEND — PostgreSQL scans + runtime for SEC-EDGAR Q4 (module only).
//
// The state, its maintenance and the result are the engine-neutral Q4 module
// (accelerators/sec_edgar/q4/common/aux).  This file is the PostgreSQL side:
// the SPI scans that feed it.  The bridge join (pre x sub x tag x num) runs
// inside the server, so the process receives one row per qualifying
// (adsh, tag, version) key (~65K on SF3), never the 39M-row num table; the
// structural re-read of a few dirty keys is the same join restricted to them.
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include "sec_edgar/q4/common/aux/module.hpp"
#include "sec_edgar/q4/cost_model.hpp"
#include "sec_edgar/q4/layout.hpp"

#include <stdexcept>

#include "backends/postgres/api/engine_runtime.hpp"
#include "backends/postgres/api/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

// Fixed capture-trigger table ids shared by every SEC-EDGAR PostgreSQL query
// (the trigger argument): sub=1 num=2 pre=3 tag=4.  Differs from the DuckDB binding.
constexpr core::TableId kSecEdgarSub = 1;
constexpr core::TableId kSecEdgarNum = 2;
constexpr core::TableId kSecEdgarPre = 3;
constexpr core::TableId kSecEdgarTag = 4;

bool IsNullAt(HeapTuple tuple, TupleDesc tuple_desc, int attribute) {
  bool is_null = false;
  SPI_getbinval(tuple, tuple_desc, attribute + 1, &is_null);
  return is_null;
}

double Float8At(HeapTuple tuple, TupleDesc tuple_desc, int attribute) {
  bool is_null = false;
  const Datum datum = SPI_getbinval(tuple, tuple_desc, attribute + 1, &is_null);
  if (is_null) return 0.0;
  if (TupleDescAttr(tuple_desc, attribute)->atttypid == FLOAT8OID) {
    return DatumGetFloat8(datum);
  }
  return std::stod(DatumText(datum, TupleDescAttr(tuple_desc, attribute)->atttypid));
}

// SPI_execute (not a cursor) so that the planner may parallelize the big build
// join.  The result of every query below is small (<= ~100K narrow rows).
template <typename Callback>
void RunQuery(const std::string &sql, Callback &&callback) {
  ++t_table_scans;
  if (SPI_connect() != SPI_OK_CONNECT) {
    throw std::runtime_error("SPI_connect failed");
  }
  const int status = SPI_execute(sql.c_str(), true, 0);
  if (status != SPI_OK_SELECT) {
    SPI_finish();
    throw std::runtime_error("sec_edgar q4 scan failed");
  }
  try {
    SPITupleTable *table = SPI_tuptable;
    if (table) {
      TupleDesc tuple_desc = table->tupdesc;
      for (uint64 i = 0; i < SPI_processed; ++i) {
        callback(table->vals[i], tuple_desc);
      }
    }
  } catch (...) {
    SPI_finish();
    throw;
  }
  SPI_finish();
}

// Reads the database as this transaction leaves it (own writes included):
// the structural re-read runs at PRE_COMMIT.
template <typename Callback>
void RunQueryAfterOwnWrites(const std::string &sql, Callback &&callback) {
  CommandCounterIncrement();
  PushActiveSnapshot(GetLatestSnapshot());
  try {
    RunQuery(sql, std::forward<Callback>(callback));
  } catch (...) {
    PopActiveSnapshot();
    throw;
  }
  PopActiveSnapshot();
}

std::string Lit(const std::string &s) {
  char *quoted = quote_literal_cstr(s.c_str());
  std::string out(quoted);
  pfree(quoted);
  return out;
}

class SecEdgarQ4PgScan final : public query::SecEdgarQ4InitialScan {
public:
  void ScanSub(const std::function<void(const query::SecEdgarQ4SubInput &)> &consumer)
      const override {
    ScanTypedTuples("SELECT adsh, cik, sic FROM sub WHERE sic IS NOT NULL",
                    [&](HeapTuple tuple, TupleDesc tuple_desc) {
                      query::SecEdgarQ4SubInput in;
                      in.adsh = TypedText(tuple, tuple_desc, 0);
                      in.cik_null = IsNullAt(tuple, tuple_desc, 1);
                      in.cik = in.cik_null ? 0 : static_cast<int32_t>(TypedInt(tuple, tuple_desc, 1));
                      in.sic = static_cast<int32_t>(TypedInt(tuple, tuple_desc, 2));
                      consumer(in);
                    });
  }
  void ScanTag(const std::function<void(const query::SecEdgarQ4TagInput &)> &consumer)
      const override {
    ScanTypedTuples("SELECT tag, version, abstract, tlabel FROM tag WHERE abstract = 0",
                    [&](HeapTuple tuple, TupleDesc tuple_desc) {
                      query::SecEdgarQ4TagInput in;
                      in.tag = TypedText(tuple, tuple_desc, 0);
                      in.version = TypedText(tuple, tuple_desc, 1);
                      in.abstract_flag = static_cast<int32_t>(TypedInt(tuple, tuple_desc, 2));
                      in.tlabel_null = IsNullAt(tuple, tuple_desc, 3);
                      if (!in.tlabel_null) in.tlabel = TypedText(tuple, tuple_desc, 3);
                      consumer(in);
                    });
  }
  void ScanPre(const std::function<void(const query::SecEdgarQ4PreInput &)> &consumer)
      const override {
    ScanTypedTuples(
        "SELECT adsh, tag, version, stmt FROM pre WHERE stmt IS NOT NULL AND adsh IS NOT NULL "
        "AND tag IS NOT NULL AND version IS NOT NULL",
        [&](HeapTuple tuple, TupleDesc tuple_desc) {
          consumer({TypedText(tuple, tuple_desc, 0), TypedText(tuple, tuple_desc, 1),
                    TypedText(tuple, tuple_desc, 2), TypedText(tuple, tuple_desc, 3)});
        });
  }
  void ScanNum(const std::function<void(const query::SecEdgarQ4NumInput &)> &consumer)
      const override {
    ScanTypedTuples(
        "SELECT adsh, tag, version, uom, value FROM num WHERE uom IS NOT NULL AND value IS NOT NULL "
        "AND adsh IS NOT NULL AND tag IS NOT NULL AND version IS NOT NULL",
        [&](HeapTuple tuple, TupleDesc tuple_desc) {
          query::SecEdgarQ4NumInput in;
          in.adsh = TypedText(tuple, tuple_desc, 0);
          in.tag = TypedText(tuple, tuple_desc, 1);
          in.version = TypedText(tuple, tuple_desc, 2);
          in.uom = TypedText(tuple, tuple_desc, 3);
          in.value = Float8At(tuple, tuple_desc, 4);
          in.value_is_null = false;
          consumer(in);
        });
  }

  void ScanBridge(const query::SecEdgarQ4BridgeFilter &filter,
                  const std::function<void(const query::SecEdgarQ4BridgeRow &)> &consumer)
      const override {
    // No predicate on stmt, uom or sic: the state keeps every group.
    const std::string pre_base =
        "p.stmt IS NOT NULL AND p.adsh IS NOT NULL AND p.tag IS NOT NULL AND "
        "p.version IS NOT NULL";
    std::string src;
    if (filter.all) {
      src = "SELECT p.adsh, p.tag, p.version, p.stmt FROM pre p WHERE " + pre_base;
    } else {
      // One index-friendly branch per kind of dirty key; UNION on the row id
      // makes a pre row matched by several branches count once.
      std::vector<std::string> branches;
      if (!filter.adsh.empty()) {
        std::string list;
        for (std::size_t i = 0; i < filter.adsh.size(); ++i) {
          list += (i ? "," : "") + Lit(filter.adsh[i]);
        }
        branches.push_back("SELECT p.ctid AS rid, p.adsh, p.tag, p.version, p.stmt FROM pre p WHERE " +
                           pre_base + " AND p.adsh = ANY(ARRAY[" + list + "]::text[])");
      }
      if (!filter.tag_versions.empty()) {
        std::string list;
        for (std::size_t i = 0; i < filter.tag_versions.size(); ++i) {
          list += (i ? ",(" : "(") + Lit(filter.tag_versions[i].first) + "," +
                  Lit(filter.tag_versions[i].second) + ")";
        }
        branches.push_back(
            "SELECT p.ctid AS rid, p.adsh, p.tag, p.version, p.stmt FROM pre p JOIN (VALUES " + list +
            ") AS k(tag, version) ON p.tag = k.tag AND p.version = k.version WHERE " + pre_base);
      }
      if (!filter.keys.empty()) {
        std::string list;
        for (std::size_t i = 0; i < filter.keys.size(); ++i) {
          list += (i ? ",(" : "(") + Lit(filter.keys[i].adsh) + "," + Lit(filter.keys[i].tag) +
                  "," + Lit(filter.keys[i].version) + ")";
        }
        branches.push_back(
            "SELECT p.ctid AS rid, p.adsh, p.tag, p.version, p.stmt FROM pre p JOIN (VALUES " + list +
            ") AS k(adsh, tag, version) ON p.adsh = k.adsh AND p.tag = k.tag AND "
            "p.version = k.version WHERE " + pre_base);
      }
      if (branches.empty()) return; // nothing requested
      std::string u;
      for (std::size_t i = 0; i < branches.size(); ++i) {
        u += (i ? " UNION " : "") + branches[i];
      }
      src = "SELECT adsh, tag, version, stmt FROM (" + u + ") u";
    }
    std::string sql =
        "WITH pe AS (SELECT adsh, tag, version, stmt, COUNT(*) AS mult FROM (" + src +
        ") x GROUP BY adsh, tag, version, stmt) "
        "SELECT pe.adsh, pe.tag, pe.version, pe.stmt, pe.mult::int8, s.cik, s.sic, t.tlabel, "
        "n.uom, n.ns, n.nc ";
    sql += "FROM pe JOIN sub s ON s.adsh = pe.adsh AND s.sic IS NOT NULL "
           "JOIN tag t ON t.tag = pe.tag AND t.version = pe.version AND t.abstract = 0 ";
    if (filter.all) {
      sql += "LEFT JOIN (SELECT adsh, tag, version, uom, SUM(value)::float8 AS ns, "
             "COUNT(value)::int8 AS nc FROM num WHERE uom IS NOT NULL AND value IS NOT NULL "
             "GROUP BY adsh, tag, version, uom) n ON n.adsh = pe.adsh AND n.tag = pe.tag AND "
             "n.version = pe.version";
    } else {
      // A handful of keys: probe num by (adsh, tag, version) per pre key.
      sql = "WITH pe AS (SELECT adsh, tag, version, stmt, COUNT(*) AS mult FROM (" + src +
            ") x GROUP BY adsh, tag, version, stmt) "
            "SELECT pe.adsh, pe.tag, pe.version, pe.stmt, pe.mult::int8, s.cik, s.sic, t.tlabel, "
            "n.uom, n.ns, n.nc FROM pe JOIN sub s ON s.adsh = pe.adsh AND s.sic IS NOT NULL "
            "JOIN tag t ON t.tag = pe.tag AND t.version = pe.version AND t.abstract = 0 "
            "LEFT JOIN LATERAL (SELECT uom, SUM(value)::float8 AS ns, COUNT(value)::int8 AS nc "
            "FROM num WHERE adsh = pe.adsh AND tag = pe.tag AND version = pe.version AND "
            "uom IS NOT NULL AND value IS NOT NULL GROUP BY uom) n ON true";
    }
    const auto row = [&](HeapTuple tuple, TupleDesc tuple_desc) {
      query::SecEdgarQ4BridgeRow r;
      r.adsh = TypedText(tuple, tuple_desc, 0);
      r.tag = TypedText(tuple, tuple_desc, 1);
      r.version = TypedText(tuple, tuple_desc, 2);
      r.stmt = TypedText(tuple, tuple_desc, 3);
      r.mult = static_cast<uint32_t>(TypedInt(tuple, tuple_desc, 4));
      r.cik_null = IsNullAt(tuple, tuple_desc, 5);
      r.cik = r.cik_null ? 0 : static_cast<int32_t>(TypedInt(tuple, tuple_desc, 5));
      r.sic = static_cast<int32_t>(TypedInt(tuple, tuple_desc, 6));
      r.tlabel_null = IsNullAt(tuple, tuple_desc, 7);
      if (!r.tlabel_null) r.tlabel = TypedText(tuple, tuple_desc, 7);
      r.has_num = !IsNullAt(tuple, tuple_desc, 8);
      if (r.has_num) {
        r.uom = TypedText(tuple, tuple_desc, 8);
        r.ns = Float8At(tuple, tuple_desc, 9);
        r.nc = TypedInt(tuple, tuple_desc, 10);
      }
      consumer(r);
    };
    if (filter.all) {
      RunQuery(sql, row);
    } else {
      RunQueryAfterOwnWrites(sql, row);
    }
  }
};

Runtime<query::SecEdgarQ4Module, query::SecEdgarQ4Config, SecEdgarQ4PgScan> &Q4Runtime() {
  static Runtime<query::SecEdgarQ4Module, query::SecEdgarQ4Config, SecEdgarQ4PgScan> runtime(
      "ACCELERATOR_CODEGEN_POSTGRES_SEC_EDGAR_Q4_COST_MODEL",
      core::kSecEdgarQ4CostModel, core::SecEdgarQ4Layout::kAccelerator,
      query::SecEdgarQ4Config{kSecEdgarNum, kSecEdgarSub, kSecEdgarTag, kSecEdgarPre},
      {}, /*tune_build=*/true);
  return runtime;
}

} // namespace
} // namespace detail

namespace {

void ExecutePostgresSecEdgarQ4(FunctionCallInfo fcinfo) {
  // accel_sec_edgar_q4_aux() uses the template defaults; the four-argument
  // overload is not STRICT: a NULL argument reaches the module as NULL.
  core::QueryArguments arguments;
  if (PG_NARGS() >= 4) {
    for (int i = 0; i < 2; ++i) {
      arguments.values.push_back(
          PG_ARGISNULL(i) ? core::AuxValue::Null(core::AuxType::Int64)
                          : core::AuxValue::Of(core::AuxType::Int64,
                                               static_cast<int64_t>(PG_GETARG_INT32(i))));
    }
    for (int i = 2; i < 4; ++i) {
      arguments.values.push_back(
          PG_ARGISNULL(i) ? core::AuxValue::Null(core::AuxType::String)
                          : core::AuxValue::Of(core::AuxType::String,
                                               detail::TextDatum(PG_GETARG_DATUM(i))));
    }
  }
  detail::Q4Runtime().Execute(arguments, fcinfo);
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(
    PostgresQueryExecutor{"sec_edgar", 4, kPostgresVariantAux, &ExecutePostgresSecEdgarQ4});
const detail::RuntimeRegistrar<&detail::Q4Runtime> kAcceleratorRegistrar(
    core::SecEdgarQ4Layout::kAccelerator);

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
