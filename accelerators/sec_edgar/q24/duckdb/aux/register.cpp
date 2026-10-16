// GENERATED QUERY: SEC EDGAR Q24 aux state on DuckDB.
// Registers the catalog entry (SQL, probes, benchmark arguments), the
// table function accel_sec_edgar_q24_aux and the module's binding: its config, the
// tables engine capture watches, and the InitialScan over DuckDB SQL.
#include "sec_edgar/q24/duckdb/aux/module.hpp"
#include "sec_edgar/q24/cost_model.hpp"
#include "sec_edgar/q24/duckdb/aux/parallel/module.hpp"
#include "sec_edgar/q24/layout.hpp"
#include "sec_edgar/common/duckdb_probes.hpp"
#include "backends/duckdb/api/aux_query_registry.hpp"
#include "catalog/query_catalog.hpp"
#include "backends/duckdb/engine/dictionary_id_cache.hpp"
#include "backends/duckdb/engine/duckdb_scan_adapter.hpp"
#include "sec_edgar/q24/duckdb/base/query.hpp"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen::aux_state::engine {
namespace {
namespace q24_catalog {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace catalog_values;
using sec_edgar_probes::BuildSecNumRowidProbe;

SharedProbe BuildSecEdgarQ24Probe(::duckdb::MaterializedQueryResult &result,
                                  ::duckdb::idx_t row) {
  return BuildSecNumRowidProbe(result, row, 101);
}

const SharedQueryRegistrar kEntry{SharedQueryCatalogEntry{24,
     SharedWorkloadKind::SecEdgar,
     "sec_edgar",
     core::SecEdgarQ24Layout::kAccelerator,
     "SELECT n.tag, n.version, COUNT(*) AS cnt, SUM(n.value) AS total "
     "FROM num n "
     "LEFT JOIN pre p ON n.tag = p.tag AND n.version = p.version AND "
     "n.adsh = p.adsh "
     "WHERE n.uom = 'USD' AND n.ddate BETWEEN 20230101 AND 20231231 "
     "AND n.value IS NOT NULL AND p.adsh IS NULL "
     "GROUP BY n.tag, n.version "
     "HAVING COUNT(*) > 10 "
     "ORDER BY cnt DESC "
     "LIMIT 100",
     "SELECT * FROM accel_sec_edgar_q24_aux()",
     "SELECT * FROM accel_sec_edgar_q24()",
     "SELECT * FROM accel_sec_edgar_q24_aux()",
     "SELECT n.rowid, n.adsh, n.tag, n.version, n.ddate, n.qtrs, n.uom, n.coreg, "
     "n.value FROM num n "
     "LEFT JOIN pre p ON n.tag = p.tag AND n.version = p.version AND "
     "n.adsh = p.adsh WHERE n.uom = 'USD' AND n.ddate BETWEEN 20230101 "
     "AND 20231231 AND n.value IS NOT NULL AND p.adsh IS NULL "
     "AND (n.tag, n.version) IN ("
     "  SELECT n2.tag, n2.version FROM num n2 "
     "  LEFT JOIN pre p2 ON n2.tag = p2.tag AND n2.version = p2.version AND "
     "  n2.adsh = p2.adsh WHERE n2.uom = 'USD' AND n2.ddate BETWEEN 20230101 "
     "  AND 20231231 AND n2.value IS NOT NULL AND p2.adsh IS NULL "
     "  GROUP BY n2.tag, n2.version HAVING COUNT(*) > 10 "
     "  ORDER BY COUNT(*) DESC "
     "  LIMIT 100"
     ") ORDER BY n.value DESC LIMIT 1",
     {},
     101,
     true,
     BuildSecEdgarQ24Probe,
     core::kSecEdgarQ24CostModel}};

} // namespace q24_catalog
} // namespace
} // namespace accelerator_codegen::aux_state::engine

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace accelerator_codegen::aux_state::query;
using namespace scan;

// Streams a SELECT of (VARCHAR, VARCHAR, VARCHAR[, DOUBLE]) as column-oriented
// batches of string views, avoiding per-cell Value construction.
template <typename Consumer>
void StreamQ24Columns(::duckdb::Connection &connection, const std::string &sql,
                      bool with_value, Consumer &&consume) {
  auto result = ScanConnection(connection).SendQuery(sql);
  Require(result != nullptr && !result->HasError(),
          result ? result->GetError() : "DuckDB scan returned null");
  std::vector<Q24Str> strings[3];
  std::vector<double> values;
  ::duckdb::UnifiedVectorFormat formats[4];
  while (true) {
    auto chunk = result->Fetch();
    if (!chunk || chunk->size() == 0) {
      break;
    }
    const auto n = chunk->size();
    for (int c = 0; c < 3; ++c) {
      chunk->data[c].ToUnifiedFormat(n, formats[c]);
      const auto *data =
          ::duckdb::UnifiedVectorFormat::GetData<::duckdb::string_t>(formats[c]);
      strings[c].resize(n);
      for (::duckdb::idx_t i = 0; i < n; ++i) {
        const auto idx = formats[c].sel->get_index(i);
        if (!formats[c].validity.RowIsValid(idx)) {
          strings[c][i] = Q24Str::Null();
        } else {
          strings[c][i] = Q24Str::Of(data[idx].GetData(), data[idx].GetSize());
        }
      }
    }
    if (with_value) {
      chunk->data[3].ToUnifiedFormat(n, formats[3]);
      const auto *data = ::duckdb::UnifiedVectorFormat::GetData<double>(formats[3]);
      values.resize(n);
      for (::duckdb::idx_t i = 0; i < n; ++i) {
        values[i] = data[formats[3].sel->get_index(i)];
      }
    }
    consume(static_cast<std::size_t>(n), strings[0].data(), strings[1].data(),
            strings[2].data(), values.data());
  }
  Require(!result->HasError(), result->GetError());
}

template <typename T> struct ChunkColumn;
template <> struct ChunkColumn<::duckdb::string_t> {
  ::duckdb::UnifiedVectorFormat fmt;
  const ::duckdb::string_t *data = nullptr;
  void Init(::duckdb::Vector &vec, ::duckdb::idx_t n) {
    vec.ToUnifiedFormat(n, fmt);
    data = ::duckdb::UnifiedVectorFormat::GetData<::duckdb::string_t>(fmt);
  }
  Q24Str At(::duckdb::idx_t i) const {
    const auto idx = fmt.sel->get_index(i);
    if (!fmt.validity.RowIsValid(idx)) return Q24Str::Null();
    return Q24Str::Of(data[idx].GetData(), data[idx].GetSize());
  }
};

struct SecEdgarQ24Scan : SecEdgarQ24InitialScan {
  explicit SecEdgarQ24Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanNum(
      const std::function<void(const SecEdgarQ24NumInput &)> &consumer) override {
    ScanQueryDecoded(
        connection, "SELECT adsh, tag, version, uom, value, ddate FROM num",
        [](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          const auto value = ChunkValue(chunk, 4, row);
          const auto date = ChunkValue(chunk, 5, row);
          return SecEdgarQ24NumInput{ChunkValue(chunk, 0, row).ToString(),
                                     ChunkValue(chunk, 1, row).ToString(),
                                     ChunkValue(chunk, 2, row).ToString(),
                                     ChunkValue(chunk, 3, row).ToString(),
                                     value.IsNull() ? 0.0 : std::stod(value.ToString()),
                                     value.IsNull(),
                                     static_cast<int32_t>(SafeInt32(date))};
        },
        [&](auto &&input) { consumer(input); });
  }
  void ScanPre(
      const std::function<void(const SecEdgarQ24PreInput &)> &consumer) override {
    ScanQueryDecoded(
        connection, "SELECT adsh, tag, version FROM pre",
        [](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          return SecEdgarQ24PreInput{ChunkValue(chunk, 0, row).ToString(),
                                     ChunkValue(chunk, 1, row).ToString(),
                                     ChunkValue(chunk, 2, row).ToString()};
        },
        [&](auto &&input) { consumer(input); });
  }
  bool ScanQualifyingNumBatches(
      const std::function<void(const SecEdgarQ24NumBatch &)> &consumer) override {
    bool delivered = false;
    try {
      std::vector<Q24Str> a, t, v, u;
      std::vector<double> value;
      std::vector<int32_t> ddate;
      NativeScan(CreateSecEdgarQ24AuxNumScanSpec(), [&](::duckdb::DataChunk &chunk) {
        delivered = true;
        const auto n = chunk.size();
        ChunkColumn<::duckdb::string_t> ca, ct, cv, cu;
        ca.Init(chunk.data[0], n);
        ct.Init(chunk.data[1], n);
        cv.Init(chunk.data[2], n);
        cu.Init(chunk.data[4], n);
        ::duckdb::UnifiedVectorFormat value_fmt, date_fmt;
        chunk.data[3].ToUnifiedFormat(n, value_fmt);
        chunk.data[5].ToUnifiedFormat(n, date_fmt);
        const auto *values = ::duckdb::UnifiedVectorFormat::GetData<double>(value_fmt);
        const auto *dates = ::duckdb::UnifiedVectorFormat::GetData<int32_t>(date_fmt);
        a.resize(n); t.resize(n); v.resize(n); u.resize(n); value.resize(n);
        ddate.resize(n);
        std::size_t out = 0;
        for (::duckdb::idx_t i = 0; i < n; ++i) {
          // Pushed-down storage filters are re-checked (cheap, keeps the
          // contract exact even if a scan only applies them partially).
          const auto vi = value_fmt.sel->get_index(i);
          const auto di = date_fmt.sel->get_index(i);
          if (!value_fmt.validity.RowIsValid(vi) || !date_fmt.validity.RowIsValid(di))
            continue;
          int32_t year;
          if (!Q24DdateYear(dates[di], year)) continue; // in no year bucket
          const Q24Str uom = cu.At(i);
          if (uom.null) continue;
          u[out] = uom;
          ddate[out] = dates[di];
          a[out] = ca.At(i);
          t[out] = ct.At(i);
          v[out] = cv.At(i);
          value[out] = values[vi];
          ++out;
        }
        if (out > 0)
          consumer({out, a.data(), t.data(), v.data(), value.data(), u.data(),
                    ddate.data()});
      });
      return true;
    } catch (const std::exception &) {
      if (delivered) throw;
    }
    // Native scan unavailable (e.g. unexpected column types): SQL streaming.
    std::vector<Q24Str> a, t, v, u;
    std::vector<double> value;
    std::vector<int32_t> ddate;
    auto result = ScanConnection(connection).SendQuery(
        "SELECT adsh, tag, version, value, uom, ddate FROM num WHERE value IS NOT NULL "
        "AND uom IS NOT NULL AND ddate IS NOT NULL");
    Require(result != nullptr && !result->HasError(),
            result ? result->GetError() : "DuckDB scan returned null");
    while (true) {
      auto chunk = result->Fetch();
      if (!chunk || chunk->size() == 0) break;
      const auto n = chunk->size();
      ChunkColumn<::duckdb::string_t> ca, ct, cv, cu;
      ca.Init(chunk->data[0], n);
      ct.Init(chunk->data[1], n);
      cv.Init(chunk->data[2], n);
      cu.Init(chunk->data[4], n);
      ::duckdb::UnifiedVectorFormat value_fmt, date_fmt;
      chunk->data[3].ToUnifiedFormat(n, value_fmt);
      chunk->data[5].ToUnifiedFormat(n, date_fmt);
      const auto *values = ::duckdb::UnifiedVectorFormat::GetData<double>(value_fmt);
      const auto *dates = ::duckdb::UnifiedVectorFormat::GetData<int32_t>(date_fmt);
      a.resize(n); t.resize(n); v.resize(n); u.resize(n); value.resize(n);
      ddate.resize(n);
      for (::duckdb::idx_t i = 0; i < n; ++i) {
        a[i] = ca.At(i);
        t[i] = ct.At(i);
        v[i] = cv.At(i);
        u[i] = cu.At(i);
        value[i] = values[value_fmt.sel->get_index(i)];
        ddate[i] = dates[date_fmt.sel->get_index(i)];
      }
      consumer({static_cast<std::size_t>(n), a.data(), t.data(), v.data(),
                value.data(), u.data(), ddate.data()});
    }
    Require(!result->HasError(), result->GetError());
    return true;
  }
  bool ScanNumOrphans(
      const Q24PreBase &base,
      const std::function<void(const SecEdgarQ24NumBatch &)> &consumer) override {
    bool delivered = false;
    try {
      // Membership is tested on pre's dictionary ids, resolved once per
      // storage-dictionary entry (DictionaryIdCache), not per row.
      DictionaryIdCache adsh_ids(Q24StringDict::kNone), tag_ids(Q24StringDict::kNone),
          version_ids(Q24StringDict::kNone);
      const auto find_in = [](const Q24StringDict &dict) {
        return [&dict](const char *data, uint32_t size) { return dict.Find(data, size); };
      };
      const auto find_adsh = find_in(base.adsh);
      const auto find_tag = find_in(base.tag);
      const auto find_version = find_in(base.version);
      // A new storage dictionary is resolved whole with pipelined lookups.
      std::vector<const char *> entry_data;
      std::vector<uint32_t> entry_size;
      const auto batch_in = [&entry_data, &entry_size](const Q24StringDict &dict) {
        return [&dict, &entry_data, &entry_size](const ::duckdb::string_t *entries,
                                                 const ::duckdb::ValidityMask &validity,
                                                 ::duckdb::idx_t count, uint32_t *ids) {
          entry_data.resize(count);
          entry_size.resize(count);
          for (::duckdb::idx_t k = 0; k < count; ++k) {
            const bool valid = validity.RowIsValid(k);
            entry_data[k] = valid ? entries[k].GetData() : nullptr;
            entry_size[k] = valid ? static_cast<uint32_t>(entries[k].GetSize()) : 0;
          }
          dict.FindBatch(entry_data.data(), entry_size.data(), count, ids);
        };
      };
      const auto batch_adsh = batch_in(base.adsh);
      const auto batch_tag = batch_in(base.tag);
      const auto batch_version = batch_in(base.version);
      std::vector<uint32_t> aid(STANDARD_VECTOR_SIZE), tid(STANDARD_VECTOR_SIZE),
          vid(STANDARD_VECTOR_SIZE);
      std::vector<uint8_t> hit(STANDARD_VECTOR_SIZE);
      std::vector<Q24Str> a, t, v, u;
      std::vector<double> value;
      std::vector<int32_t> ddate;
      NativeScan(CreateSecEdgarQ24AuxNumScanSpec(), [&](::duckdb::DataChunk &chunk) {
        delivered = true;
        const auto n = chunk.size();
        if (n == 0) return;
        if (aid.size() < n) {
          aid.resize(n); tid.resize(n); vid.resize(n); hit.resize(n);
        }
        adsh_ids.Resolve(chunk.data[0], n, find_adsh, batch_adsh, aid.data());
        tag_ids.Resolve(chunk.data[1], n, find_tag, batch_tag, tid.data());
        version_ids.Resolve(chunk.data[2], n, find_version, batch_version, vid.data());
        base.ContainsIdsBatch(aid.data(), tid.data(), vid.data(), n, hit.data());
        std::size_t misses = 0;
        for (::duckdb::idx_t i = 0; i < n; ++i) misses += hit[i] == 0;
        if (misses == 0) return;
        ChunkColumn<::duckdb::string_t> ca, ct, cv, cu;
        ca.Init(chunk.data[0], n);
        ct.Init(chunk.data[1], n);
        cv.Init(chunk.data[2], n);
        cu.Init(chunk.data[4], n);
        ::duckdb::UnifiedVectorFormat value_fmt, date_fmt;
        chunk.data[3].ToUnifiedFormat(n, value_fmt);
        chunk.data[5].ToUnifiedFormat(n, date_fmt);
        const auto *values = ::duckdb::UnifiedVectorFormat::GetData<double>(value_fmt);
        const auto *dates = ::duckdb::UnifiedVectorFormat::GetData<int32_t>(date_fmt);
        a.resize(misses); t.resize(misses); v.resize(misses); u.resize(misses);
        value.resize(misses); ddate.resize(misses);
        std::size_t out = 0;
        for (::duckdb::idx_t i = 0; i < n; ++i) {
          if (hit[i]) continue;
          const auto vi = value_fmt.sel->get_index(i);
          const auto di = date_fmt.sel->get_index(i);
          if (!value_fmt.validity.RowIsValid(vi) || !date_fmt.validity.RowIsValid(di))
            continue;
          const Q24Str uom = cu.At(i);
          if (uom.null) continue;
          u[out] = uom;
          ddate[out] = dates[di];
          a[out] = ca.At(i);
          t[out] = ct.At(i);
          v[out] = cv.At(i);
          value[out] = values[vi];
          ++out;
        }
        if (out > 0)
          consumer({out, a.data(), t.data(), v.data(), value.data(), u.data(), ddate.data()});
      });
      return true;
    } catch (const std::exception &) {
      if (delivered) throw;
    }
    return false;
  }

  bool ScanPreIds(Q24RawPre &raw) override {
    if (!raw.rows.empty()) return false;
    try {
      DictionaryIdCache adsh_ids(Q24StringDict::kNone), tag_ids(Q24StringDict::kNone),
          version_ids(Q24StringDict::kNone);
      const auto add_to = [](Q24StringDict &dict) {
        return [&dict](const char *data, uint32_t size) { return dict.FindOrAdd(data, size); };
      };
      const auto add_adsh = add_to(raw.adsh);
      const auto add_tag = add_to(raw.tag);
      const auto add_version = add_to(raw.version);
      std::vector<uint32_t> aid(STANDARD_VECTOR_SIZE), tid(STANDARD_VECTOR_SIZE),
          vid(STANDARD_VECTOR_SIZE);
      NativeScan(CreateSecEdgarQ24PreScanSpec(), [&](::duckdb::DataChunk &chunk) {
        const auto n = chunk.size();
        if (aid.size() < n) {
          aid.resize(n); tid.resize(n); vid.resize(n);
        }
        adsh_ids.Resolve(chunk.data[0], n, add_adsh, aid.data());
        tag_ids.Resolve(chunk.data[1], n, add_tag, tid.data());
        version_ids.Resolve(chunk.data[2], n, add_version, vid.data());
        for (::duckdb::idx_t i = 0; i < n; ++i) {
          // A pre row with a NULL join key can never match a num row.
          if (aid[i] == Q24StringDict::kNone || tid[i] == Q24StringDict::kNone ||
              vid[i] == Q24StringDict::kNone)
            continue;
          raw.rows.push_back(aid[i]);
          raw.rows.push_back(tid[i]);
          raw.rows.push_back(vid[i]);
        }
      });
      return true;
    } catch (const std::exception &) {
      if (!raw.rows.empty() || raw.adsh.size() || raw.tag.size() || raw.version.size()) throw;
    }
    return false;
  }

  bool ScanPreBatches(
      const std::function<void(const SecEdgarQ24PreBatch &)> &consumer) override {
    bool delivered = false;
    try {
      std::vector<Q24Str> a, t, v;
      NativeScan(CreateSecEdgarQ24PreScanSpec(), [&](::duckdb::DataChunk &chunk) {
        delivered = true;
        const auto n = chunk.size();
        ChunkColumn<::duckdb::string_t> ca, ct, cv;
        ca.Init(chunk.data[0], n);
        ct.Init(chunk.data[1], n);
        cv.Init(chunk.data[2], n);
        a.resize(n); t.resize(n); v.resize(n);
        for (::duckdb::idx_t i = 0; i < n; ++i) {
          a[i] = ca.At(i);
          t[i] = ct.At(i);
          v[i] = cv.At(i);
        }
        consumer({static_cast<std::size_t>(n), a.data(), t.data(), v.data()});
      });
      return true;
    } catch (const std::exception &) {
      if (delivered) throw;
    }
    StreamQ24Columns(connection, "SELECT adsh, tag, version FROM pre",
                     /*with_value=*/false,
                     [&](std::size_t n, const Q24Str *a, const Q24Str *t,
                         const Q24Str *v, const double *) {
                       consumer({n, a, t, v});
                     });
    return true;
  }

  bool LoadTripleStates(const std::vector<SecEdgarQ24TripleKey> &keys,
                        std::vector<SecEdgarQ24TripleState> &out) override {
    out.assign(keys.size(), {});
    if (keys.empty()) return true;
    const auto quote = [](const std::string &s) {
      std::string q = "'";
      for (const char c : s) {
        if (c == '\'') q += "''";
        else q += c;
      }
      return q + "'";
    };
    std::string values;
    for (std::size_t i = 0; i < keys.size(); ++i) {
      if (i) values += ",";
      values += "(" + std::to_string(i) + "," + quote(keys[i].adsh) + "," +
                quote(keys[i].tag) + "," + quote(keys[i].version) + ")";
    }
    const std::string k = "(VALUES " + values + ") k(i, a, t, v)";
    // Each lookup sees the committing transaction's own writes.
    ScanQuery(connection,
              "SELECT k.i, COUNT(p.adsh)::BIGINT FROM " + k +
                  " LEFT JOIN pre p ON p.adsh = k.a AND p.tag = k.t AND "
                  "p.version = k.v GROUP BY k.i",
              [&](auto &result, ::duckdb::idx_t row) {
                out[static_cast<std::size_t>(result.GetValue(0, row).template GetValue<int64_t>())]
                    .pre_rows = result.GetValue(1, row).template GetValue<int64_t>();
              });
    // Per (year, uom) bucket: year = floor(ddate / 10000), month/day in
    // [101, 1231] (see Q24DdateYear). Computed in BIGINT to avoid overflow.
    ScanQuery(connection,
              "SELECT i, uom, (dd - r) // 10000 AS y, COUNT(*)::BIGINT, SUM(value)::DOUBLE FROM ("
              "SELECT k.i AS i, n.uom AS uom, n.ddate::BIGINT AS dd, "
              "((n.ddate::BIGINT % 10000) + 10000) % 10000 AS r, n.value AS value FROM " + k +
                  " JOIN num n ON n.adsh = k.a AND n.tag = k.t AND "
                  "n.version = k.v WHERE n.uom IS NOT NULL AND n.ddate IS NOT NULL "
                  "AND n.value IS NOT NULL) WHERE r BETWEEN 101 AND 1231 GROUP BY i, uom, y",
              [&](auto &result, ::duckdb::idx_t row) {
                auto &s = out[static_cast<std::size_t>(
                    result.GetValue(0, row).template GetValue<int64_t>())];
                SecEdgarQ24TripleBucket b;
                b.uom = result.GetValue(1, row).ToString();
                b.year = static_cast<int32_t>(result.GetValue(2, row).template GetValue<int64_t>());
                b.num_rows = result.GetValue(3, row).template GetValue<int64_t>();
                b.num_sum = result.GetValue(4, row).template GetValue<double>();
                s.buckets.push_back(std::move(b));
              });
    return true;
  }

private:
  template <typename Consumer>
  void NativeScan(const NativeScanSpec &spec, Consumer &&consume) {
    auto &context = *ScanConnection(connection).context;
    const bool started = !context.transaction.HasActiveTransaction();
    if (started) {
      context.transaction.BeginTransaction();
    }
    try {
      ScanNativeBaseTable(context, spec, consume);
      if (started) {
        context.transaction.Commit();
      }
    } catch (...) {
      if (started && context.transaction.HasActiveTransaction()) {
        context.transaction.Rollback(nullptr);
      }
      throw;
    }
  }

public:
  ::duckdb::Connection &connection;
};

// accel_sec_edgar_q24_aux() uses the defaults (2023, 'USD');
// accel_sec_edgar_q24_aux(year BIGINT, uom VARCHAR) binds the query's parameters.
const std::vector<std::vector<::duckdb::LogicalType>> kOverloads{
    {::duckdb::LogicalType::BIGINT, ::duckdb::LogicalType::VARCHAR}};

// num's full rows; pre's adsh, tag, version.
SecEdgarQ24Config Config() {
  SecEdgarQ24Config c{101, 102};
  c.pre_attnums = {0, 6, 7};
  return c;
}

CaptureTableList CaptureTables(const SecEdgarQ24Config &c) {
  return {{"num", c.num_table}, {"pre", c.pre_table}};
}

const AuxQueryRegistrar kQuery{AuxQuery{
    core::SecEdgarQ24Layout::kAccelerator,
    "accel_sec_edgar_q24_aux",
    {},
    [](::duckdb::Connection &connection) {
      const auto c = Config();
      return MakeBinding<SecEdgarQ24Scan, SecEdgarQ24Module>(connection, CaptureTables(c), c);
    },
    kOverloads}};

#ifdef ACCEL_ENABLE_PARALLEL_AUX
const AuxQueryRegistrar kParallelQuery{AuxQuery{
    std::string(core::SecEdgarQ24Layout::kAccelerator) + "_aux_parallel",
    std::string("accel_") + core::SecEdgarQ24Layout::kAccelerator + "_aux_parallel",
    {},
    [](::duckdb::Connection &connection) {
      const auto c = Config();
      return MakeBinding<SecEdgarQ24Scan, SecEdgarQ24ParallelModule>(connection, CaptureTables(c), c);
    },
    kOverloads}};
#endif

} // namespace
} // namespace accelerator_codegen::aux_state::engine::duckdb
