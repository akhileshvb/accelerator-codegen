#include "sec_edgar/q4/duckdb/aux/duckdb_scan.hpp"

#include <stdexcept>
#include <string>

#include "duckdb/common/types/vector.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen::aux_state::query {
namespace {

std::string Lit(const std::string &s) {
  std::string out = "'";
  for (const char c : s) {
    if (c == '\'') out.push_back('\'');
    out.push_back(c);
  }
  out.push_back('\'');
  return out;
}

template <typename T>
const T *Data(const ::duckdb::UnifiedVectorFormat &f) {
  return ::duckdb::UnifiedVectorFormat::GetData<T>(f);
}

template <typename Fn>
void StreamQuery(::duckdb::Connection &connection, const std::string &sql, Fn &&fn) {
  auto result = connection.SendQuery(sql);
  if (!result || result->HasError()) {
    throw std::runtime_error(result ? result->GetError() : "sec_edgar q4 scan failed");
  }
  while (true) {
    auto chunk = result->Fetch();
    if (!chunk || chunk->size() == 0) break;
    fn(*chunk);
  }
  if (result->HasError()) {
    throw std::runtime_error(result->GetError());
  }
}

std::string Str(const ::duckdb::UnifiedVectorFormat &f, ::duckdb::idx_t row) {
  const auto idx = f.sel->get_index(row);
  return Data<::duckdb::string_t>(f)[idx].GetString();
}

} // namespace

void SecEdgarQ4DuckDbScan::ScanSub(
    const std::function<void(const SecEdgarQ4SubInput &)> &consumer) const {
  StreamQuery(connection, "SELECT adsh, cik, sic FROM sub WHERE sic IS NOT NULL",
              [&](::duckdb::DataChunk &chunk) {
                ::duckdb::UnifiedVectorFormat a, c, s;
                chunk.data[0].ToUnifiedFormat(chunk.size(), a);
                chunk.data[1].ToUnifiedFormat(chunk.size(), c);
                chunk.data[2].ToUnifiedFormat(chunk.size(), s);
                for (::duckdb::idx_t r = 0; r < chunk.size(); ++r) {
                  SecEdgarQ4SubInput in;
                  in.adsh = Str(a, r);
                  const auto ci = c.sel->get_index(r);
                  in.cik_null = !c.validity.RowIsValid(ci);
                  in.cik = in.cik_null ? 0 : Data<int32_t>(c)[ci];
                  in.sic = Data<int32_t>(s)[s.sel->get_index(r)];
                  consumer(in);
                }
              });
}

void SecEdgarQ4DuckDbScan::ScanTag(
    const std::function<void(const SecEdgarQ4TagInput &)> &consumer) const {
  StreamQuery(connection, "SELECT tag, version, abstract, tlabel FROM tag WHERE abstract = 0",
              [&](::duckdb::DataChunk &chunk) {
                ::duckdb::UnifiedVectorFormat t, v, ab, tl;
                chunk.data[0].ToUnifiedFormat(chunk.size(), t);
                chunk.data[1].ToUnifiedFormat(chunk.size(), v);
                chunk.data[2].ToUnifiedFormat(chunk.size(), ab);
                chunk.data[3].ToUnifiedFormat(chunk.size(), tl);
                for (::duckdb::idx_t r = 0; r < chunk.size(); ++r) {
                  SecEdgarQ4TagInput in;
                  in.tag = Str(t, r);
                  in.version = Str(v, r);
                  in.abstract_flag = Data<int32_t>(ab)[ab.sel->get_index(r)];
                  const auto li = tl.sel->get_index(r);
                  in.tlabel_null = !tl.validity.RowIsValid(li);
                  if (!in.tlabel_null) in.tlabel = Str(tl, r);
                  consumer(in);
                }
              });
}

void SecEdgarQ4DuckDbScan::ScanPre(
    const std::function<void(const SecEdgarQ4PreInput &)> &consumer) const {
  StreamQuery(connection,
              "SELECT adsh, tag, version, stmt FROM pre WHERE stmt = 'EQ' AND adsh IS NOT NULL "
              "AND tag IS NOT NULL AND version IS NOT NULL",
              [&](::duckdb::DataChunk &chunk) {
                ::duckdb::UnifiedVectorFormat a, t, v, s;
                chunk.data[0].ToUnifiedFormat(chunk.size(), a);
                chunk.data[1].ToUnifiedFormat(chunk.size(), t);
                chunk.data[2].ToUnifiedFormat(chunk.size(), v);
                chunk.data[3].ToUnifiedFormat(chunk.size(), s);
                for (::duckdb::idx_t r = 0; r < chunk.size(); ++r) {
                  consumer({Str(a, r), Str(t, r), Str(v, r), Str(s, r)});
                }
              });
}

void SecEdgarQ4DuckDbScan::ScanNum(
    const std::function<void(const SecEdgarQ4NumInput &)> &consumer) const {
  StreamQuery(connection,
              "SELECT adsh, tag, version, uom, value FROM num WHERE uom = 'USD' "
              "AND value IS NOT NULL AND adsh IS NOT NULL AND tag IS NOT NULL "
              "AND version IS NOT NULL",
              [&](::duckdb::DataChunk &chunk) {
                ::duckdb::UnifiedVectorFormat a, t, v, u, x;
                chunk.data[0].ToUnifiedFormat(chunk.size(), a);
                chunk.data[1].ToUnifiedFormat(chunk.size(), t);
                chunk.data[2].ToUnifiedFormat(chunk.size(), v);
                chunk.data[3].ToUnifiedFormat(chunk.size(), u);
                chunk.data[4].ToUnifiedFormat(chunk.size(), x);
                for (::duckdb::idx_t r = 0; r < chunk.size(); ++r) {
                  SecEdgarQ4NumInput in;
                  in.adsh = Str(a, r);
                  in.tag = Str(t, r);
                  in.version = Str(v, r);
                  in.uom = Str(u, r);
                  in.value = Data<double>(x)[x.sel->get_index(r)];
                  in.value_is_null = false;
                  consumer(in);
                }
              });
}

void SecEdgarQ4DuckDbScan::ScanBridge(
    const SecEdgarQ4BridgeFilter &filter,
    const std::function<void(const SecEdgarQ4BridgeRow &)> &consumer) const {
  std::string pre_filter = "p.stmt = 'EQ' AND p.adsh IS NOT NULL AND p.tag IS NOT NULL "
                           "AND p.version IS NOT NULL";
  if (!filter.all) {
    std::string any;
    if (!filter.adsh.empty()) {
      any += "p.adsh IN (";
      for (size_t i = 0; i < filter.adsh.size(); ++i) {
        any += (i ? "," : "") + Lit(filter.adsh[i]);
      }
      any += ")";
    }
    if (!filter.tag_versions.empty()) {
      if (!any.empty()) any += " OR ";
      any += "(p.tag, p.version) IN (";
      for (size_t i = 0; i < filter.tag_versions.size(); ++i) {
        any += (i ? ",(" : "(") + Lit(filter.tag_versions[i].first) + "," +
               Lit(filter.tag_versions[i].second) + ")";
      }
      any += ")";
    }
    if (!filter.keys.empty()) {
      if (!any.empty()) any += " OR ";
      any += "(p.adsh, p.tag, p.version) IN (";
      for (size_t i = 0; i < filter.keys.size(); ++i) {
        any += (i ? ",(" : "(") + Lit(filter.keys[i].adsh) + "," +
               Lit(filter.keys[i].tag) + "," + Lit(filter.keys[i].version) + ")";
      }
      any += ")";
    }
    if (any.empty()) {
      return; // nothing requested
    }
    pre_filter += " AND (" + any + ")";
  }
  std::string sql =
      "WITH pe AS (SELECT p.adsh, p.tag, p.version, COUNT(*) AS mult FROM pre p WHERE " +
      pre_filter + " GROUP BY p.adsh, p.tag, p.version) "
      "SELECT pe.adsh, pe.tag, pe.version, CAST(pe.mult AS BIGINT), "
      "CAST(s.cik AS INTEGER), CAST(s.sic AS INTEGER), t.tlabel, ";
  if (filter.include_num) {
    sql += "CAST(COALESCE(SUM(n.value), 0) AS DOUBLE), CAST(COUNT(n.value) AS BIGINT) ";
  } else {
    sql += "CAST(0 AS DOUBLE), CAST(0 AS BIGINT) ";
  }
  sql += "FROM pe JOIN sub s ON s.adsh = pe.adsh AND s.sic BETWEEN 4000 AND 4999 "
         "JOIN tag t ON t.tag = pe.tag AND t.version = pe.version AND t.abstract = 0 ";
  if (filter.include_num) {
    sql += "LEFT JOIN num n ON n.adsh = pe.adsh AND n.tag = pe.tag AND "
           "n.version = pe.version AND n.uom = 'USD' AND n.value IS NOT NULL "
           "GROUP BY pe.adsh, pe.tag, pe.version, pe.mult, s.cik, s.sic, t.tlabel";
  }
  StreamQuery(connection, sql, [&](::duckdb::DataChunk &chunk) {
    ::duckdb::UnifiedVectorFormat a, t, v, m, c, s, tl, ns, nc;
    const auto n = chunk.size();
    chunk.data[0].ToUnifiedFormat(n, a);
    chunk.data[1].ToUnifiedFormat(n, t);
    chunk.data[2].ToUnifiedFormat(n, v);
    chunk.data[3].ToUnifiedFormat(n, m);
    chunk.data[4].ToUnifiedFormat(n, c);
    chunk.data[5].ToUnifiedFormat(n, s);
    chunk.data[6].ToUnifiedFormat(n, tl);
    chunk.data[7].ToUnifiedFormat(n, ns);
    chunk.data[8].ToUnifiedFormat(n, nc);
    for (::duckdb::idx_t r = 0; r < n; ++r) {
      SecEdgarQ4BridgeRow row;
      row.adsh = Str(a, r);
      row.tag = Str(t, r);
      row.version = Str(v, r);
      row.mult = static_cast<uint32_t>(Data<int64_t>(m)[m.sel->get_index(r)]);
      const auto ci = c.sel->get_index(r);
      row.cik_null = !c.validity.RowIsValid(ci);
      row.cik = row.cik_null ? 0 : Data<int32_t>(c)[ci];
      row.sic = Data<int32_t>(s)[s.sel->get_index(r)];
      const auto li = tl.sel->get_index(r);
      row.tlabel_null = !tl.validity.RowIsValid(li);
      if (!row.tlabel_null) row.tlabel = Str(tl, r);
      row.ns = Data<double>(ns)[ns.sel->get_index(r)];
      row.nc = Data<int64_t>(nc)[nc.sel->get_index(r)];
      consumer(row);
    }
  });
}

} // namespace accelerator_codegen::aux_state::query
