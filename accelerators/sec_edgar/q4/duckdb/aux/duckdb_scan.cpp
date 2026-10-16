#include "sec_edgar/q4/duckdb/aux/duckdb_scan.hpp"

#include <cstdio>
#include <cstdlib>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include "backends/duckdb/engine/build_scan.hpp"
#include "backends/duckdb/engine/duckdb_scan_adapter.hpp"
#include "substrate/aux/core/partitioned_build.hpp"
#include "backends/duckdb/engine/native_scan_compat.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/materialized_query_result.hpp"
#include "sec_edgar/q4/common/aux/state_ops.hpp"

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

constexpr uint32_t kSamePrev = 0xFFFFFFFDu;

inline std::string_view View(const ::duckdb::string_t &s) {
  return std::string_view(s.GetData(), s.GetSize());
}

// Maps a VARCHAR vector to ids with a batch resolver
// resolve(const std::string_view *, size_t n, uint32_t *out).  A dictionary
// vector (dictionary-compressed storage segment) is resolved once per
// dictionary entry: the ids are cached by the dictionary buffer, which is kept
// alive so that its address cannot be reused.  Flat vectors resolve each run of
// equal values once.  The strings of one chunk are resolved as one batch.
class IdColumn {
public:
  template <typename Resolve>
  void Map(::duckdb::Vector &vec, ::duckdb::idx_t n, Resolve &&resolve, uint32_t *out) {
    using ::duckdb::VectorType;
    views_.clear();
    where_.clear();
    switch (vec.GetVectorType()) {
    case VectorType::CONSTANT_VECTOR: {
      uint32_t id = kQ4None;
      if (!::duckdb::ConstantVector::IsNull(vec)) {
        const auto v = View(::duckdb::ConstantVector::GetData<::duckdb::string_t>(vec)[0]);
        resolve(&v, 1, &id);
      }
      for (::duckdb::idx_t i = 0; i < n; ++i) out[i] = id;
      return;
    }
    case VectorType::DICTIONARY_VECTOR: {
      auto &child = ::duckdb::DictionaryVector::Child(vec);
      const auto size = ::duckdb::DictionaryVector::DictionarySize(vec);
      if (child.GetVectorType() != VectorType::FLAT_VECTOR || !size.IsValid()) break;
      auto aux = vec.GetAuxiliary();
      if (!aux) break;
      if (aux.get() != key_) {
        // A new segment dictionary: resolve all of its entries as one batch.
        holder_ = aux;
        key_ = aux.get();
        const auto dict_size = size.GetIndex();
        ids_.assign(dict_size, kQ4None);
        const auto *data = ::duckdb::FlatVector::GetData<::duckdb::string_t>(child);
        const auto &valid = ::duckdb::FlatVector::Validity(child);
        for (::duckdb::idx_t idx = 0; idx < dict_size; ++idx) {
          if (!valid.RowIsValid(idx)) continue;
          where_.push_back(static_cast<uint32_t>(idx));
          views_.push_back(View(data[idx]));
        }
        RunBatch(resolve);
        for (size_t k = 0; k < where_.size(); ++k) ids_[where_[k]] = res_[k];
      }
      const auto &sel = ::duckdb::DictionaryVector::SelVector(vec);
      const uint32_t *ids = ids_.data();
      for (::duckdb::idx_t i = 0; i < n; ++i) out[i] = ids[sel.get_index(i)];
      return;
    }
    case VectorType::FLAT_VECTOR: {
      const auto *data = ::duckdb::FlatVector::GetData<::duckdb::string_t>(vec);
      const auto &valid = ::duckdb::FlatVector::Validity(vec);
      for (::duckdb::idx_t i = 0; i < n; ++i) {
        if (!valid.RowIsValid(i)) {
          out[i] = kQ4None;
        } else if (i > 0 && valid.RowIsValid(i - 1) && data[i] == data[i - 1]) {
          out[i] = kSamePrev;
        } else {
          where_.push_back(static_cast<uint32_t>(i));
          views_.push_back(View(data[i]));
        }
      }
      RunBatch(resolve);
      for (size_t k = 0; k < where_.size(); ++k) out[where_[k]] = res_[k];
      for (::duckdb::idx_t i = 1; i < n; ++i) {
        if (out[i] == kSamePrev) out[i] = out[i - 1];
      }
      return;
    }
    default:
      break;
    }
    ::duckdb::UnifiedVectorFormat f;
    vec.ToUnifiedFormat(n, f);
    const auto *data = Data<::duckdb::string_t>(f);
    for (::duckdb::idx_t i = 0; i < n; ++i) {
      const auto idx = f.sel->get_index(i);
      if (!f.validity.RowIsValid(idx)) {
        out[i] = kQ4None;
      } else {
        where_.push_back(static_cast<uint32_t>(i));
        views_.push_back(View(data[idx]));
      }
    }
    RunBatch(resolve);
    for (size_t k = 0; k < where_.size(); ++k) out[where_[k]] = res_[k];
  }

private:
  template <typename R> void RunBatch(R &&resolve) {
    res_.resize(views_.size());
    if (!views_.empty()) resolve(views_.data(), views_.size(), res_.data());
  }

  ::duckdb::buffer_ptr<::duckdb::VectorBuffer> holder_;
  const void *key_ = nullptr;
  std::vector<uint32_t> ids_;
  std::vector<std::string_view> views_;
  std::vector<uint32_t> where_, res_;
};

enum class Col { Str, Dbl, Int };
NativeScanSpec Spec(const char *table, std::initializer_list<std::pair<const char *, Col>> cols) {
  NativeScanSpec spec;
  spec.table_name = table;
  for (const auto &c : cols) {
    switch (c.second) {
    case Col::Dbl:
      spec.projected_columns.push_back(
          {c.first, c.first, ::duckdb::LogicalType::DOUBLE, ::duckdb::PhysicalType::DOUBLE});
      break;
    case Col::Int:
      spec.projected_columns.push_back(
          {c.first, c.first, ::duckdb::LogicalType::INTEGER, ::duckdb::PhysicalType::INT32});
      break;
    case Col::Str:
      spec.projected_columns.push_back(
          {c.first, c.first, ::duckdb::LogicalType::VARCHAR, ::duckdb::PhysicalType::VARCHAR});
      break;
    }
  }
  return spec;
}

std::string InList(const std::vector<std::string> &values) {
  std::string out;
  for (size_t i = 0; i < values.size(); ++i) {
    out += (i ? "," : "") + Lit(values[i]);
  }
  return out;
}

// Runs fn inside the connection's transaction (one is opened when none is).
template <typename Fn> void InTransaction(::duckdb::Connection &connection, Fn &&fn) {
  const bool started = !connection.HasActiveTransaction();
  if (started) connection.BeginTransaction();
  try {
    fn();
    if (started) connection.Commit();
  } catch (...) {
    if (started && connection.HasActiveTransaction()) connection.Rollback();
    throw;
  }
}

} // namespace

bool SecEdgarQ4DuckDbScan::BuildFull(const core::AuxEngineServices &services, q4::Builder &b) const {
  if (!connection.context) return false;
  auto &context = *connection.context;
  InTransaction(connection, [&] {
    StreamQuery(connection, "SELECT adsh, cik, sic FROM sub WHERE sic IS NOT NULL AND adsh IS NOT NULL",
                [&](::duckdb::DataChunk &chunk) {
                  ::duckdb::UnifiedVectorFormat a, c, s;
                  chunk.data[0].ToUnifiedFormat(chunk.size(), a);
                  chunk.data[1].ToUnifiedFormat(chunk.size(), c);
                  chunk.data[2].ToUnifiedFormat(chunk.size(), s);
                  for (::duckdb::idx_t r = 0; r < chunk.size(); ++r) {
                    const auto &as = Data<::duckdb::string_t>(a)[a.sel->get_index(r)];
                    const auto ci = c.sel->get_index(r);
                    const bool cik_null = !c.validity.RowIsValid(ci);
                    b.AddSub(std::string_view(as.GetData(), as.GetSize()), cik_null,
                             cik_null ? 0 : Data<int32_t>(c)[ci], Data<int32_t>(s)[s.sel->get_index(r)]);
                  }
                });
    {
      const auto spec = Spec("tag", {{"tag", Col::Str}, {"version", Col::Str}, {"abstract", Col::Int}, {"tlabel", Col::Str}});
      b.ReserveTags(GetNativeScanTableRowCount(context, spec));
      std::vector<std::string_view> tv(STANDARD_VECTOR_SIZE), vv(STANDARD_VECTOR_SIZE), lv(STANDARD_VECTOR_SIZE);
      std::vector<uint8_t> lnull(STANDARD_VECTOR_SIZE);
      ScanNativeBaseTable(context, spec, [&](::duckdb::DataChunk &chunk) {
        const auto n = chunk.size();
        ::duckdb::UnifiedVectorFormat t, v, ab, tl;
        chunk.data[0].ToUnifiedFormat(n, t);
        chunk.data[1].ToUnifiedFormat(n, v);
        chunk.data[2].ToUnifiedFormat(n, ab);
        chunk.data[3].ToUnifiedFormat(n, tl);
        size_t k = 0;
        for (::duckdb::idx_t r = 0; r < n; ++r) {
          const auto ti = t.sel->get_index(r), vi = v.sel->get_index(r), ai = ab.sel->get_index(r),
                     li = tl.sel->get_index(r);
          if (!t.validity.RowIsValid(ti) || !v.validity.RowIsValid(vi) || !ab.validity.RowIsValid(ai) ||
              Data<int32_t>(ab)[ai] != 0) {
            continue; // t.abstract = 0 is false / the key cannot join
          }
          tv[k] = View(Data<::duckdb::string_t>(t)[ti]);
          vv[k] = View(Data<::duckdb::string_t>(v)[vi]);
          lnull[k] = !tl.validity.RowIsValid(li);
          lv[k] = lnull[k] ? std::string_view() : View(Data<::duckdb::string_t>(tl)[li]);
          ++k;
        }
        b.AddTags(tv.data(), vv.data(), lv.data(), lnull.data(), k);
      });
    }
    const auto adsh_id = [&](const std::string_view *v, size_t k, uint32_t *o) { b.AdshIds(v, k, o); };
    const auto tag_id = [&](const std::string_view *v, size_t k, uint32_t *o) { b.TagIds(v, k, o); };
    const auto ver_id = [&](const std::string_view *v, size_t k, uint32_t *o) { b.VerIds(v, k, o); };
    const auto stmt_id = [&](const std::string_view *v, size_t k, uint32_t *o) {
      for (size_t i = 0; i < k; ++i) o[i] = b.StmtId(v[i].data(), v[i].size());
    };
    const auto uom_id = [&](const std::string_view *v, size_t k, uint32_t *o) {
      for (size_t i = 0; i < k; ++i) o[i] = b.UomId(v[i].data(), v[i].size());
    };
    std::vector<uint32_t> ia(STANDARD_VECTOR_SIZE), it(STANDARD_VECTOR_SIZE), iv(STANDARD_VECTOR_SIZE),
        ix(STANDARD_VECTOR_SIZE);
    std::vector<double> xs(STANDARD_VECTOR_SIZE);
    {
      const auto spec =
          Spec("pre", {{"adsh", Col::Str}, {"tag", Col::Str}, {"version", Col::Str}, {"stmt", Col::Str}});
      b.BeginPre(GetNativeScanTableRowCount(context, spec));
      IdColumn ca, ct, cv, cs;
      ScanNativeBaseTable(context, spec, [&](::duckdb::DataChunk &chunk) {
        const auto n = chunk.size();
        ca.Map(chunk.data[0], n, adsh_id, ia.data());
        ct.Map(chunk.data[1], n, tag_id, it.data());
        cv.Map(chunk.data[2], n, ver_id, iv.data());
        cs.Map(chunk.data[3], n, stmt_id, ix.data());
        b.AddPre(ia.data(), it.data(), iv.data(), ix.data(), n);
      });
    }
    using Partial = q4::Builder::NumPartial;
    b.MergeNum(core::PartitionedBuild<Partial>(
        services, [] { return Partial{}; },
        [&](std::size_t partitions, auto &&sink) {
          engine::duckdb::scan::ScanPartitioned(services, connection, "num",
                                {engine::duckdb::scan::Nullable(engine::duckdb::scan::TextColumn("adsh")), engine::duckdb::scan::Nullable(engine::duckdb::scan::TextColumn("tag")),
                                 engine::duckdb::scan::Nullable(engine::duckdb::scan::TextColumn("version")), engine::duckdb::scan::Nullable(engine::duckdb::scan::TextColumn("uom")),
                                 engine::duckdb::scan::Nullable(engine::duckdb::scan::DoubleColumn("value"))},
                                partitions, sink);
        },
        [&](Partial &partial, const auto &row) { q4::AddNumRow(b, partial, row); },
        [](Partial &into, Partial &&from) { into.Merge(std::move(from)); }, core::kFloatBuildPartitions));
  });
  return true;
}

void SecEdgarQ4DuckDbScan::ScanDirty(const SecEdgarQ4BridgeFilter &filter,
                                     SecEdgarQ4DirtyRows &out) const {
  const auto restrict_to = [&](const std::string &a) -> std::string {
    if (filter.all) return std::string();
    std::string any;
    if (!filter.adsh.empty()) {
      any += a + ".adsh IN (" + InList(filter.adsh) + ")";
    }
    if (!filter.tag_versions.empty()) {
      if (!any.empty()) any += " OR ";
      any += "(" + a + ".tag, " + a + ".version) IN (";
      for (size_t i = 0; i < filter.tag_versions.size(); ++i) {
        any += (i ? ",(" : "(") + Lit(filter.tag_versions[i].first) + "," +
               Lit(filter.tag_versions[i].second) + ")";
      }
      any += ")";
    }
    if (!filter.keys.empty()) {
      if (!any.empty()) any += " OR ";
      any += "(" + a + ".adsh, " + a + ".tag, " + a + ".version) IN (";
      for (size_t i = 0; i < filter.keys.size(); ++i) {
        any += (i ? ",(" : "(") + Lit(filter.keys[i].adsh) + "," + Lit(filter.keys[i].tag) + "," +
               Lit(filter.keys[i].version) + ")";
      }
      any += ")";
    }
    if (any.empty()) any = "FALSE";
    return " AND (" + any + ")";
  };
  std::unordered_set<std::string> need_adsh(filter.adsh.begin(), filter.adsh.end());
  std::set<std::pair<std::string, std::string>> need_tv(filter.tag_versions.begin(),
                                                        filter.tag_versions.end());
  for (const auto &k : filter.keys) {
    need_adsh.insert(k.adsh);
    need_tv.emplace(k.tag, k.version);
  }
  StreamQuery(connection,
              "SELECT p.adsh, p.tag, p.version, p.stmt FROM pre p WHERE p.stmt IS NOT NULL "
              "AND p.adsh IS NOT NULL AND p.tag IS NOT NULL AND p.version IS NOT NULL" +
                  restrict_to("p"),
              [&](::duckdb::DataChunk &chunk) {
                ::duckdb::UnifiedVectorFormat a, t, v, s;
                chunk.data[0].ToUnifiedFormat(chunk.size(), a);
                chunk.data[1].ToUnifiedFormat(chunk.size(), t);
                chunk.data[2].ToUnifiedFormat(chunk.size(), v);
                chunk.data[3].ToUnifiedFormat(chunk.size(), s);
                for (::duckdb::idx_t r = 0; r < chunk.size(); ++r) {
                  out.pres.push_back({Str(a, r), Str(t, r), Str(v, r), Str(s, r)});
                  need_adsh.insert(out.pres.back().adsh);
                  need_tv.emplace(out.pres.back().tag, out.pres.back().version);
                }
              });
  StreamQuery(connection,
              "SELECT n.adsh, n.tag, n.version, n.uom, CAST(n.value AS DOUBLE) FROM num n "
              "WHERE n.uom IS NOT NULL AND n.value IS NOT NULL AND n.adsh IS NOT NULL "
              "AND n.tag IS NOT NULL AND n.version IS NOT NULL" +
                  restrict_to("n"),
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
                  out.nums.push_back(std::move(in));
                }
              });
  constexpr size_t kMaxInList = 2048;
  if (!need_adsh.empty()) {
    std::string sql = "SELECT adsh, cik, sic FROM sub WHERE sic IS NOT NULL AND adsh IS NOT NULL";
    if (need_adsh.size() <= kMaxInList) {
      sql += " AND adsh IN (" + InList(std::vector<std::string>(need_adsh.begin(), need_adsh.end())) + ")";
    }
    StreamQuery(connection, sql, [&](::duckdb::DataChunk &chunk) {
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
        out.subs.push_back(std::move(in));
      }
    });
  }
  if (!need_tv.empty()) {
    std::string sql = "SELECT t.tag, t.version, t.tlabel FROM tag t WHERE t.abstract = 0 "
                      "AND t.tag IS NOT NULL AND t.version IS NOT NULL";
    if (need_tv.size() <= kMaxInList) {
      sql += " AND (t.tag, t.version) IN (";
      size_t i = 0;
      for (const auto &tv : need_tv) {
        sql += (i++ ? ",(" : "(") + Lit(tv.first) + "," + Lit(tv.second) + ")";
      }
      sql += ")";
    }
    StreamQuery(connection, sql, [&](::duckdb::DataChunk &chunk) {
      ::duckdb::UnifiedVectorFormat t, v, tl;
      chunk.data[0].ToUnifiedFormat(chunk.size(), t);
      chunk.data[1].ToUnifiedFormat(chunk.size(), v);
      chunk.data[2].ToUnifiedFormat(chunk.size(), tl);
      for (::duckdb::idx_t r = 0; r < chunk.size(); ++r) {
        SecEdgarQ4TagInput in;
        in.tag = Str(t, r);
        in.version = Str(v, r);
        in.abstract_flag = 0;
        const auto li = tl.sel->get_index(r);
        in.tlabel_null = !tl.validity.RowIsValid(li);
        if (!in.tlabel_null) in.tlabel = Str(tl, r);
        out.tags.push_back(std::move(in));
      }
    });
  }
}

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
              "SELECT adsh, tag, version, stmt FROM pre WHERE stmt IS NOT NULL AND adsh IS NOT NULL "
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
              "SELECT adsh, tag, version, uom, value FROM num WHERE uom IS NOT NULL "
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
  // Restriction on (adsh, tag, version) for alias `a` (pre or num).
  const auto restrict_to = [&](const std::string &a) -> std::string {
    if (filter.all) return std::string();
    std::string any;
    if (!filter.adsh.empty()) {
      any += a + ".adsh IN (";
      for (size_t i = 0; i < filter.adsh.size(); ++i) {
        any += (i ? "," : "") + Lit(filter.adsh[i]);
      }
      any += ")";
    }
    if (!filter.tag_versions.empty()) {
      if (!any.empty()) any += " OR ";
      any += "(" + a + ".tag, " + a + ".version) IN (";
      for (size_t i = 0; i < filter.tag_versions.size(); ++i) {
        any += (i ? ",(" : "(") + Lit(filter.tag_versions[i].first) + "," +
               Lit(filter.tag_versions[i].second) + ")";
      }
      any += ")";
    }
    if (!filter.keys.empty()) {
      if (!any.empty()) any += " OR ";
      any += "(" + a + ".adsh, " + a + ".tag, " + a + ".version) IN (";
      for (size_t i = 0; i < filter.keys.size(); ++i) {
        any += (i ? ",(" : "(") + Lit(filter.keys[i].adsh) + "," +
               Lit(filter.keys[i].tag) + "," + Lit(filter.keys[i].version) + ")";
      }
      any += ")";
    }
    return " AND (" + any + ")";
  };
  if (!filter.all && filter.adsh.empty() && filter.tag_versions.empty() && filter.keys.empty()) {
    return; // nothing requested
  }
  // No predicate on stmt, uom or sic: the state keeps every group.
  const std::string sql =
      "WITH pe AS (SELECT p.adsh, p.tag, p.version, p.stmt, COUNT(*) AS mult FROM pre p "
      "WHERE p.stmt IS NOT NULL AND p.adsh IS NOT NULL AND p.tag IS NOT NULL "
      "AND p.version IS NOT NULL" + restrict_to("p") +
      " GROUP BY p.adsh, p.tag, p.version, p.stmt), "
      "nu AS (SELECT n.adsh, n.tag, n.version, n.uom, SUM(n.value) AS ns, "
      "COUNT(n.value) AS nc FROM num n WHERE n.uom IS NOT NULL AND n.value IS NOT NULL" +
      restrict_to("n") + " GROUP BY n.adsh, n.tag, n.version, n.uom) "
      "SELECT pe.adsh, pe.tag, pe.version, pe.stmt, CAST(pe.mult AS BIGINT), "
      "CAST(s.cik AS INTEGER), CAST(s.sic AS INTEGER), t.tlabel, nu.uom, "
      "CAST(nu.ns AS DOUBLE), CAST(nu.nc AS BIGINT) "
      "FROM pe JOIN sub s ON s.adsh = pe.adsh AND s.sic IS NOT NULL "
      "JOIN tag t ON t.tag = pe.tag AND t.version = pe.version AND t.abstract = 0 "
      "LEFT JOIN nu ON nu.adsh = pe.adsh AND nu.tag = pe.tag AND nu.version = pe.version";
  StreamQuery(connection, sql, [&](::duckdb::DataChunk &chunk) {
    ::duckdb::UnifiedVectorFormat a, t, v, st, m, c, s, tl, u, ns, nc;
    const auto n = chunk.size();
    chunk.data[0].ToUnifiedFormat(n, a);
    chunk.data[1].ToUnifiedFormat(n, t);
    chunk.data[2].ToUnifiedFormat(n, v);
    chunk.data[3].ToUnifiedFormat(n, st);
    chunk.data[4].ToUnifiedFormat(n, m);
    chunk.data[5].ToUnifiedFormat(n, c);
    chunk.data[6].ToUnifiedFormat(n, s);
    chunk.data[7].ToUnifiedFormat(n, tl);
    chunk.data[8].ToUnifiedFormat(n, u);
    chunk.data[9].ToUnifiedFormat(n, ns);
    chunk.data[10].ToUnifiedFormat(n, nc);
    for (::duckdb::idx_t r = 0; r < n; ++r) {
      SecEdgarQ4BridgeRow row;
      row.adsh = Str(a, r);
      row.tag = Str(t, r);
      row.version = Str(v, r);
      row.stmt = Str(st, r);
      row.mult = static_cast<uint32_t>(Data<int64_t>(m)[m.sel->get_index(r)]);
      const auto ci = c.sel->get_index(r);
      row.cik_null = !c.validity.RowIsValid(ci);
      row.cik = row.cik_null ? 0 : Data<int32_t>(c)[ci];
      row.sic = Data<int32_t>(s)[s.sel->get_index(r)];
      const auto li = tl.sel->get_index(r);
      row.tlabel_null = !tl.validity.RowIsValid(li);
      if (!row.tlabel_null) row.tlabel = Str(tl, r);
      const auto ui = u.sel->get_index(r);
      row.has_num = u.validity.RowIsValid(ui);
      if (row.has_num) {
        row.uom = Str(u, r);
        row.ns = Data<double>(ns)[ns.sel->get_index(r)];
        row.nc = Data<int64_t>(nc)[nc.sel->get_index(r)];
      }
      consumer(row);
    }
  });
}

} // namespace accelerator_codegen::aux_state::query
