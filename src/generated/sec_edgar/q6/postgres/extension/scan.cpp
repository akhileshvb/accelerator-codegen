// GENERATED BACKEND — PostgreSQL scans + runtime for SEC EDGAR Q6 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// The module header pulls in DuckDB types; include it before PostgreSQL headers.
#include "sec_edgar/q6/common/module.hpp"
#include "sec_edgar/q6/layout.hpp"

#include <stdexcept>
#include <string>
#include <vector>

#include "postgres/engine_runtime.hpp"
#include "postgres/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

// Table ids the SEC-EDGAR capture triggers pass (shared across the SEC-EDGAR
// queries: sub=1, num=2, pre=3, tag=4; see create_accel_mutation_triggers()).
constexpr core::TableId kSubTable = 1, kNumTable = 2, kPreTable = 3;

namespace scans {

// A varchar / text / bpchar column read straight from the varlena (no output
// function call). bpchar loses its padding, as the cast to text does.
inline bool FastText(HeapTuple tuple, TupleDesc tuple_desc, int attribute,
                     std::string &out) {
  bool is_null = false;
  const Datum datum = SPI_getbinval(tuple, tuple_desc, attribute + 1, &is_null);
  if (is_null) {
    out.clear();
    return false;
  }
  const Oid type_oid = TupleDescAttr(tuple_desc, attribute)->atttypid;
  if (type_oid == VARCHAROID || type_oid == TEXTOID || type_oid == BPCHAROID) {
    struct varlena *value = PG_DETOAST_DATUM_PACKED(datum);
    out.assign(VARDATA_ANY(value), VARSIZE_ANY_EXHDR(value));
    if (value != reinterpret_cast<struct varlena *>(DatumGetPointer(datum)))
      pfree(value);
    if (type_oid == BPCHAROID)
      StripCharPadding(out);
    return true;
  }
  out = DatumText(datum, type_oid);
  return true;
}

inline double FastDouble(HeapTuple tuple, TupleDesc tuple_desc, int attribute,
                         bool &is_null) {
  const Datum datum = SPI_getbinval(tuple, tuple_desc, attribute + 1, &is_null);
  if (is_null)
    return 0.0;
  if (TupleDescAttr(tuple_desc, attribute)->atttypid == FLOAT8OID)
    return DatumGetFloat8(datum);
  return TypedDouble(tuple, tuple_desc, attribute, is_null);
}

// `col = ANY('{"a","b"}'::text[])` for a list of adsh values.
std::string AdshAnyPredicate(const char *column,
                             const std::vector<std::string> &values) {
  if (values.empty())
    return "FALSE";
  std::string sql = std::string(column) + " = ANY('{";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i)
      sql += ',';
    sql += '"';
    for (const char c : values[i]) {
      if (c == '\'')
        sql += "''";
      else if (c == '"' || c == '\\') {
        sql += '\\';
        sql += c;
      } else
        sql += c;
    }
    sql += '"';
  }
  return sql + "}'::text[])";
}

} // namespace scans

class SecEdgarQ6PgScan final : public query::SecEdgarQ6InitialScan {
public:
  void ScanNum(const std::function<void(const query::SecEdgarQ6NumInput &)>
                   &consumer) override {
    ScanTypedTuples(
        "SELECT adsh, tag, version, uom, value FROM num "
        "WHERE uom = 'USD' AND value IS NOT NULL",
        [&](HeapTuple tuple, TupleDesc desc) {
          query::SecEdgarQ6NumInput in;
          if (!scans::FastText(tuple, desc, 0, in.adsh) ||
              !scans::FastText(tuple, desc, 1, in.tag) ||
              !scans::FastText(tuple, desc, 2, in.version))
            return;
          scans::FastText(tuple, desc, 3, in.uom);
          in.value = scans::FastDouble(tuple, desc, 4, in.value_is_null);
          consumer(in);
        });
  }

  void ScanSub(const std::function<void(const query::SecEdgarQ6SubInput &)>
                   &consumer) override {
    ScanTypedTuples(
        "SELECT adsh, name FROM sub WHERE fy = 2023",
        [&](HeapTuple tuple, TupleDesc desc) {
          query::SecEdgarQ6SubInput in;
          in.fy = 2023;
          if (!scans::FastText(tuple, desc, 0, in.adsh))
            return;
          in.name_is_null = !scans::FastText(tuple, desc, 1, in.name);
          consumer(in);
        });
  }

  void ScanPre(const std::function<void(const query::SecEdgarQ6PreInput &)>
                   &consumer) override {
    ScanTypedTuples(
        "SELECT p.adsh, p.tag, p.version, p.stmt, p.plabel FROM pre p "
        "WHERE p.stmt = 'IS' AND p.tag IS NOT NULL AND p.version IS NOT NULL "
        "AND p.adsh IN (SELECT adsh FROM sub WHERE fy = 2023)",
        [&](HeapTuple tuple, TupleDesc desc) {
          query::SecEdgarQ6PreInput in;
          scans::FastText(tuple, desc, 0, in.adsh);
          scans::FastText(tuple, desc, 1, in.tag);
          scans::FastText(tuple, desc, 2, in.version);
          scans::FastText(tuple, desc, 3, in.stmt);
          in.plabel_is_null = !scans::FastText(tuple, desc, 4, in.plabel);
          consumer(in);
        });
  }

  bool ScanGroupAggregates(
      const std::function<void(const query::SecEdgarQ6GroupAggregateInput &)>
          &consumer) override {
    ScanTypedTuples(
        "SELECT s.name, n.tag, p.plabel, SUM(n.value)::float8, COUNT(*) "
        "FROM num n JOIN sub s ON n.adsh = s.adsh "
        "JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version "
        "WHERE n.uom = 'USD' AND p.stmt = 'IS' AND s.fy = 2023 "
        "AND n.value IS NOT NULL GROUP BY s.name, n.tag, p.plabel",
        [&](HeapTuple tuple, TupleDesc desc) {
          query::SecEdgarQ6GroupAggregateInput in;
          in.name_is_null = !scans::FastText(tuple, desc, 0, in.name);
          scans::FastText(tuple, desc, 1, in.tag);
          in.plabel_is_null = !scans::FastText(tuple, desc, 2, in.plabel);
          bool null_sum = false;
          in.sum = scans::FastDouble(tuple, desc, 3, null_sum);
          in.count = TypedInt(tuple, desc, 4);
          consumer(in);
        });
    return true;
  }

  // Per-adsh lookups through the num / sub / pre adsh indexes, reading the
  // database as the writing transaction leaves it (called while maintaining a
  // commit, before it is visible to others).
  bool LoadAdshSnapshots(const std::vector<std::string> &adshs,
                         std::vector<query::SecEdgarQ6AdshSnapshot> &out) override {
    out.assign(adshs.size(), {});
    std::unordered_map<std::string, std::size_t> index;
    for (std::size_t i = 0; i < adshs.size(); ++i)
      index[adshs[i]] = i;
    ScanTypedTuplesAfterOwnWrites(
        "SELECT adsh, name FROM sub WHERE fy = 2023 AND " +
            scans::AdshAnyPredicate("adsh", adshs),
        [&](HeapTuple tuple, TupleDesc desc) {
          std::string adsh, name;
          if (!scans::FastText(tuple, desc, 0, adsh))
            return;
          const bool name_ok = scans::FastText(tuple, desc, 1, name);
          const auto it = index.find(adsh);
          if (it == index.end())
            return;
          auto &snap = out[it->second];
          snap.in_cohort = true;
          snap.name_is_null = !name_ok;
          snap.name = std::move(name);
        });
    ScanTypedTuplesAfterOwnWrites(
        "SELECT adsh, tag, version, plabel, COUNT(*) FROM pre WHERE stmt = 'IS' "
        "AND tag IS NOT NULL AND version IS NOT NULL AND " +
            scans::AdshAnyPredicate("adsh", adshs) +
            " GROUP BY adsh, tag, version, plabel",
        [&](HeapTuple tuple, TupleDesc desc) {
          std::string adsh;
          query::SecEdgarQ6PreEntry entry;
          if (!scans::FastText(tuple, desc, 0, adsh))
            return;
          scans::FastText(tuple, desc, 1, entry.tag);
          scans::FastText(tuple, desc, 2, entry.version);
          entry.plabel_is_null = !scans::FastText(tuple, desc, 3, entry.plabel);
          entry.mult = static_cast<uint32_t>(TypedInt(tuple, desc, 4));
          const auto it = index.find(adsh);
          if (it != index.end())
            out[it->second].pre.push_back(std::move(entry));
        });
    ScanTypedTuplesAfterOwnWrites(
        "SELECT adsh, tag, version, SUM(value)::float8, COUNT(*) FROM num "
        "WHERE uom = 'USD' AND value IS NOT NULL AND " +
            scans::AdshAnyPredicate("adsh", adshs) +
            " GROUP BY adsh, tag, version",
        [&](HeapTuple tuple, TupleDesc desc) {
          std::string adsh, tag, version;
          if (!scans::FastText(tuple, desc, 0, adsh) ||
              !scans::FastText(tuple, desc, 1, tag) ||
              !scans::FastText(tuple, desc, 2, version))
            return;
          bool null_sum = false;
          const double sum = scans::FastDouble(tuple, desc, 3, null_sum);
          const int64_t count = TypedInt(tuple, desc, 4);
          const auto it = index.find(adsh);
          if (it != index.end())
            out[it->second].nums.push_back({tag, version, sum, count});
        });
    return true;
  }
};

Runtime<query::SecEdgarQ6Module, query::SecEdgarQ6Config, SecEdgarQ6PgScan> &
Q6Runtime() {
  // The verified result orders name / tag / plabel byte-wise.
  static Runtime<query::SecEdgarQ6Module, query::SecEdgarQ6Config,
                 SecEdgarQ6PgScan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_SEC_EDGAR_Q6_COST_MODEL",
              "config/sec_edgar_q6_aux_cost_model.json",
              core::SecEdgarQ6Layout::kAccelerator,
              query::SecEdgarQ6Config{kNumTable, kSubTable, kPreTable},
              {{"sub", "name"}, {"num", "tag"}, {"pre", "plabel"}});
  return runtime;
}

} // namespace
} // namespace detail

namespace {

void ExecutePostgresSecEdgarQ6(FunctionCallInfo fcinfo) {
  detail::Q6Runtime().Execute(core::QueryArguments{}, fcinfo);
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(PostgresQueryExecutor{
    "sec_edgar", 6, kPostgresVariantAux, &ExecutePostgresSecEdgarQ6});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
