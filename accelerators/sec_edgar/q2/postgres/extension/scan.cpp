// GENERATED BACKEND — PostgreSQL scans + runtime for SEC-EDGAR Q2 (module only).
//
// Every base-table read is an SPI scan of the native heap (ScanTypedTuples).
// The engine-neutral state and maintenance code is shared with DuckDB
// (sec_edgar/q2/common).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include "sec_edgar/q2/postgres/aux/module.hpp"
#include "sec_edgar/q2/cost_model.hpp"

#include "backends/postgres/api/engine_runtime.hpp"
#include "backends/postgres/api/runtime.hpp"

#include <string_view>
#include <unordered_set>

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

// Table ids of the capture triggers (the SEC-EDGAR trigger script registers
// sub as 1 and num as 2).
constexpr core::TableId kSecEdgarSub = 1;
constexpr core::TableId kSecEdgarNum = 2;

std::string SqlQuote(const std::string &s) {
  bool backslash = false;
  for (const char c : s) {
    backslash = backslash || c == '\\';
  }
  std::string out = backslash ? "E'" : "'";
  for (const char c : s) {
    if (c == '\'') {
      out += "''";
    } else if (c == '\\') {
      out += "\\\\";
    } else {
      out += c;
    }
  }
  out += '\'';
  return out;
}

std::string SqlTextArray(const std::vector<std::string> &values) {
  std::string out = "ARRAY[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      out += ',';
    }
    out += SqlQuote(values[i]);
  }
  return out + "]::text[]";
}

// A text column as a view into the tuple (no copy); `scratch` holds the bytes
// of a compressed or out-of-line value.  Valid until the next call with the
// same scratch / the tuple is freed.
std::string_view TextView(HeapTuple tuple, TupleDesc tuple_desc, int attribute,
                          bool &is_null, std::string &scratch) {
  bool null_value = false;
  const Datum datum = SPI_getbinval(tuple, tuple_desc, attribute + 1, &null_value);
  is_null = null_value;
  if (null_value) {
    return {};
  }
  const struct varlena *raw = reinterpret_cast<const struct varlena *>(DatumGetPointer(datum));
  if (VARATT_IS_EXTENDED(raw) && !VARATT_IS_SHORT(raw)) {
    scratch = TypedText(tuple, tuple_desc, attribute);
    return scratch;
  }
  return std::string_view(VARDATA_ANY(raw), VARSIZE_ANY_EXHDR(raw));
}

double DoubleOf(HeapTuple tuple, TupleDesc tuple_desc, int attribute, bool &is_null) {
  bool null_value = false;
  const Datum datum = SPI_getbinval(tuple, tuple_desc, attribute + 1, &null_value);
  is_null = null_value;
  if (null_value) {
    return 0.0;
  }
  if (TupleDescAttr(tuple_desc, attribute)->atttypid == FLOAT8OID) {
    return DatumGetFloat8(datum);
  }
  return TypedDouble(tuple, tuple_desc, attribute, is_null);
}

const char *const kNumFilter =
    "value IS NOT NULL AND adsh IS NOT NULL AND tag IS NOT NULL AND uom IS NOT NULL";

class SecEdgarQ2Scan final : public query::SecEdgarQ2InitialScan {
public:
  void ScanNum(const std::function<void(const query::SecEdgarQ2NumInput &)> &consumer) override {
    ScanTypedTuples(
        "SELECT adsh, tag, uom, value FROM num WHERE adsh IS NOT NULL AND "
        "tag IS NOT NULL AND uom IS NOT NULL",
        [&](HeapTuple tuple, TupleDesc tuple_desc) {
          query::SecEdgarQ2NumInput in;
          in.adsh = TypedText(tuple, tuple_desc, 0);
          in.tag = TypedText(tuple, tuple_desc, 1);
          in.uom = TypedText(tuple, tuple_desc, 2);
          in.value = DoubleOf(tuple, tuple_desc, 3, in.value_is_null);
          consumer(in);
        });
  }

  void ScanSub(const std::function<void(const query::SecEdgarQ2SubInput &)> &consumer) override {
    ScanSubWith("SELECT adsh, name, fy FROM sub", false, consumer);
  }

  void ScanUomRowCounts(
      const std::function<void(const std::string &, uint64_t)> &consumer) override {
    ScanTypedTuples(std::string("SELECT uom, count(*) FROM num WHERE ") + kNumFilter +
                        " GROUP BY uom",
                    [&](HeapTuple tuple, TupleDesc tuple_desc) {
                      consumer(TypedText(tuple, tuple_desc, 0),
                               static_cast<uint64_t>(TypedInt(tuple, tuple_desc, 1)));
                    });
  }

  // One row per heap tuple (count 1): the state aggregates them itself.
  void ScanNumValueCounts(
      const std::vector<std::string> &uoms,
      const std::function<void(const query::SecEdgarQ2ValueCountRow &)> &consumer) override {
    if (uoms.empty()) {
      return;
    }
    std::unordered_set<std::string_view> wanted(uoms.begin(), uoms.end());
    std::string s_adsh, s_tag, s_uom;
    ScanTypedTuples(
        std::string("SELECT adsh, tag, uom, value FROM num WHERE ") + kNumFilter,
        [&](HeapTuple tuple, TupleDesc tuple_desc) {
          bool n = false;
          const auto uom = TextView(tuple, tuple_desc, 2, n, s_uom);
          if (wanted.find(uom) == wanted.end()) {
            return;
          }
          const auto adsh = TextView(tuple, tuple_desc, 0, n, s_adsh);
          const auto tag = TextView(tuple, tuple_desc, 1, n, s_tag);
          const double value = DoubleOf(tuple, tuple_desc, 3, n);
          consumer({adsh, tag, uom, value, 1});
        });
  }

  // Called from the capture trigger: read the database as the writing
  // statement (and this transaction) left it.
  void LookupNumValueCounts(
      const std::string &uom,
      const std::vector<std::pair<std::string, std::string>> &groups,
      const std::function<void(const query::SecEdgarQ2ValueCountRow &)> &consumer) override {
    if (groups.empty()) {
      return;
    }
    std::vector<std::string> adshs, tags;
    std::unordered_set<std::string> exact;
    for (const auto &g : groups) {
      adshs.push_back(g.first);
      tags.push_back(g.second);
      exact.insert(g.first + std::string(1, '\0') + g.second);
    }
    ScanTypedTuplesAfterOwnWrites(
        "SELECT adsh, tag, value, count(*) FROM num WHERE uom = " + SqlQuote(uom) +
            " AND value IS NOT NULL AND adsh = ANY(" + SqlTextArray(adshs) +
            ") AND tag = ANY(" + SqlTextArray(tags) + ") GROUP BY adsh, tag, value",
        [&](HeapTuple tuple, TupleDesc tuple_desc) {
          std::string key = TypedText(tuple, tuple_desc, 0);
          key.push_back('\0');
          key += TypedText(tuple, tuple_desc, 1);
          if (exact.count(key) == 0) {
            return;
          }
          bool n = false;
          const std::string adsh = TypedText(tuple, tuple_desc, 0);
          const std::string tag = TypedText(tuple, tuple_desc, 1);
          const double value = DoubleOf(tuple, tuple_desc, 2, n);
          consumer({adsh, tag, uom, value,
                    static_cast<uint64_t>(TypedInt(tuple, tuple_desc, 3))});
        });
  }

  void LookupSub(const std::vector<std::string> &adshs,
                 const std::function<void(const query::SecEdgarQ2SubInput &)> &consumer) override {
    if (adshs.empty()) {
      return;
    }
    ScanSubWith("SELECT adsh, name, fy FROM sub WHERE adsh = ANY(" + SqlTextArray(adshs) + ")",
                true, consumer);
  }

  // The exact SQL answer (statement snapshot): used only when the aux state
  // cannot serve the statement.
  void ExecuteDirect(
      int64_t fy, const std::string &uom,
      const std::function<void(const query::SecEdgarQ2DirectRow &)> &consumer) override {
    const std::string u = SqlQuote(uom);
    ScanTypedTuples(
        "SELECT s.name, n.tag, n.value FROM num n "
        "JOIN sub s ON n.adsh = s.adsh "
        "JOIN (SELECT adsh, tag, MAX(value) AS max_value FROM num WHERE uom = " +
            u +
            " AND value IS NOT NULL GROUP BY adsh, tag) m "
            "ON n.adsh = m.adsh AND n.tag = m.tag AND n.value = m.max_value "
            "WHERE n.uom = " +
            u + " AND s.fy = " + std::to_string(fy) +
            " AND n.value IS NOT NULL ORDER BY n.value DESC, s.name, n.tag LIMIT " +
            std::to_string(query::kSecEdgarQ2ResultLimit),
        [&](HeapTuple tuple, TupleDesc tuple_desc) {
          bool name_null = false, n = false;
          std::string name_scratch, tag_scratch;
          const auto name = TextView(tuple, tuple_desc, 0, name_null, name_scratch);
          const auto tag = TextView(tuple, tuple_desc, 1, n, tag_scratch);
          const double value = DoubleOf(tuple, tuple_desc, 2, n);
          consumer({name, name_null, tag, value});
        });
  }

private:
  void ScanSubWith(const std::string &sql, bool after_own_writes,
                   const std::function<void(const query::SecEdgarQ2SubInput &)> &consumer) {
    const auto row = [&](HeapTuple tuple, TupleDesc tuple_desc) {
      query::SecEdgarQ2SubInput in;
      in.adsh = TypedText(tuple, tuple_desc, 0);
      in.name_is_null = false;
      bool fy_null = false;
      bool name_null = false;
      std::string scratch;
      in.name.assign(TextView(tuple, tuple_desc, 1, name_null, scratch));
      in.name_is_null = name_null;
      bool null_value = false;
      const Datum d = SPI_getbinval(tuple, tuple_desc, 3, &null_value);
      fy_null = null_value;
      in.fy_is_null = fy_null;
      in.fy = fy_null ? 0 : static_cast<int32_t>(DatumGetInt32(d));
      consumer(in);
    };
    if (after_own_writes) {
      ScanTypedTuplesAfterOwnWrites(sql, row);
    } else {
      ScanTypedTuples(sql, row);
    }
  }
};

Runtime<query::SecEdgarQ2Module, query::SecEdgarQ2Config, SecEdgarQ2Scan> &Q2Runtime() {
  // The verified order is byte-wise on sub.name and num.tag.
  static Runtime<query::SecEdgarQ2Module, query::SecEdgarQ2Config, SecEdgarQ2Scan> runtime(
      "ACCELERATOR_CODEGEN_POSTGRES_SEC_EDGAR_Q2_COST_MODEL",
      core::kSecEdgarQ2CostModel, core::SecEdgarQ2Layout::kAccelerator,
      query::SecEdgarQ2Config{kSecEdgarNum, kSecEdgarSub},
      {{"sub", "name"}, {"num", "tag"}});
  return runtime;
}

} // namespace
} // namespace detail

namespace core = accelerator_codegen::aux_state::core;

namespace {

void ExecutePostgresSecEdgarQ2(FunctionCallInfo fcinfo) {
  // accel_sec_edgar_q2_aux() uses the template defaults; the two-argument
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
  detail::Q2Runtime().Execute(arguments, fcinfo);
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(PostgresQueryExecutor{
    "sec_edgar", 2, kPostgresVariantAux, &ExecutePostgresSecEdgarQ2});
const detail::RuntimeRegistrar<&detail::Q2Runtime> kAcceleratorRegistrar(
    core::SecEdgarQ2Layout::kAccelerator);

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
