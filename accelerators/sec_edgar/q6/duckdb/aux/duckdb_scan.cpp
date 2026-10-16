#include "sec_edgar/q6/duckdb/aux/duckdb_scan.hpp"

#include "sec_edgar/q6/common/build.hpp"

#include <map>
#include <stdexcept>
#include <string>

#include "backends/duckdb/api/aux_query_registry.hpp"
#include "backends/duckdb/engine/build_scan.hpp"
#include "backends/duckdb/engine/duckdb_scan_adapter.hpp"
#include "substrate/aux/core/partitioned_build.hpp"
#include "duckdb.hpp"
#include "duckdb/main/client_context.hpp"

namespace accelerator_codegen::aux_state::query {
namespace {

template <typename Consumer>
void StreamQuery(::duckdb::Connection &connection_p, const std::string &sql,
                 Consumer &&consumer) {
  // The runtime's connection, or a reader thread's private one.
  auto &connection = engine::duckdb::scan::ScanConnection(connection_p);
  auto result = connection.SendQuery(sql);
  if (!result || result->HasError()) {
    throw std::runtime_error("SEC EDGAR Q6 scan failed: " +
                             (result ? result->GetError() : std::string("null")));
  }
  while (true) {
    auto chunk = result->Fetch();
    if (!chunk || chunk->size() == 0) {
      break;
    }
    consumer(*chunk);
  }
  if (result->HasError()) {
    throw std::runtime_error("SEC EDGAR Q6 scan failed: " + result->GetError());
  }
}

struct StringColumn {
  const ::duckdb::string_t *data;
  const ::duckdb::ValidityMask *validity;
  StringColumn(::duckdb::DataChunk &chunk, ::duckdb::idx_t column)
      : data(::duckdb::FlatVector::GetData<::duckdb::string_t>(chunk.data[column])),
        validity(&::duckdb::FlatVector::Validity(chunk.data[column])) {}
  bool IsNull(::duckdb::idx_t row) const { return !validity->RowIsValid(row); }
  std::string Get(::duckdb::idx_t row) const {
    return IsNull(row) ? std::string()
                       : std::string(data[row].GetData(), data[row].GetSize());
  }
};

std::string InList(const std::vector<std::string> &values) {
  std::string out = "(";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i) out += ",";
    out += "'";
    for (char c : values[i]) {
      if (c == '\'') out += "''";
      else out += c;
    }
    out += "'";
  }
  return out + ")";
}

// Native storage scan of `table` inside the connection's transaction (or a
// private one).
template <typename Consumer>
void NativeScan(::duckdb::Connection &connection_p, const char *table,
                std::initializer_list<std::pair<const char *, ::duckdb::LogicalType>> columns,
                Consumer &&consume) {
  auto &context = *engine::duckdb::scan::ScanConnection(connection_p).context;
  NativeScanSpec spec;
  spec.table_name = table;
  for (const auto &column : columns) {
    spec.projected_columns.push_back(NativeColumnSpec{
        column.first, column.first, column.second, column.second.InternalType()});
  }
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

// A VARCHAR vector as a SecEdgarQ6StrColumn. Dictionary vectors (dictionary
// compressed storage) are passed through with their codes; the dictionary id
// DuckDB assigns per dictionary becomes the column's token.
class StrColumnAdapter {
public:
  void Load(::duckdb::Vector &vector, ::duckdb::idx_t n, SecEdgarQ6StrColumn &out) {
    out = SecEdgarQ6StrColumn{};
    if (vector.GetVectorType() == ::duckdb::VectorType::DICTIONARY_VECTOR) {
      auto &child = ::duckdb::DictionaryVector::Child(vector);
      const auto size = ::duckdb::DictionaryVector::DictionarySize(vector);
      if (child.GetVectorType() == ::duckdb::VectorType::FLAT_VECTOR &&
          size.IsValid() && size.GetIndex() > 0 &&
          size.GetIndex() < 0xFFFFFFFFull) {
        const auto &sel = ::duckdb::DictionaryVector::SelVector(vector);
        codes_.resize(n);
        for (::duckdb::idx_t i = 0; i < n; ++i) {
          codes_[i] = static_cast<uint32_t>(sel.get_index(i));
        }
        dict_.data = ::duckdb::FlatVector::GetData<::duckdb::string_t>(child);
        dict_.validity = &::duckdb::FlatVector::Validity(child);
        const auto &id = ::duckdb::DictionaryVector::DictionaryId(vector);
        if (id.empty()) {
          token_ = 0;
          last_id_.clear();
        } else if (id != last_id_ || token_ == 0) {
          last_id_ = id;
          token_ = ++next_token_;
        }
        out.codes = codes_.data();
        out.dict = &dict_;
        out.dict_get = &DictGet;
        out.dict_size = static_cast<uint32_t>(size.GetIndex());
        out.dict_token = token_;
        return;
      }
    }
    ::duckdb::UnifiedVectorFormat fmt;
    vector.ToUnifiedFormat(n, fmt);
    const auto *data = ::duckdb::UnifiedVectorFormat::GetData<::duckdb::string_t>(fmt);
    flat_.resize(n);
    for (::duckdb::idx_t i = 0; i < n; ++i) {
      const auto idx = fmt.sel->get_index(i);
      if (fmt.validity.RowIsValid(idx)) {
        flat_[i] = {data[idx].GetData(), static_cast<uint32_t>(data[idx].GetSize())};
      } else {
        flat_[i] = {};
      }
    }
    out.flat = flat_.data();
  }

private:
  struct Dict {
    const ::duckdb::string_t *data = nullptr;
    const ::duckdb::ValidityMask *validity = nullptr;
  };
  static SecEdgarQ6Str DictGet(const void *p, uint32_t code) {
    const auto &d = *static_cast<const Dict *>(p);
    if (!d.validity->RowIsValid(code)) {
      return {};
    }
    const auto &s = d.data[code];
    return {s.GetData(), static_cast<uint32_t>(s.GetSize())};
  }

  std::vector<uint32_t> codes_;
  std::vector<SecEdgarQ6Str> flat_;
  Dict dict_;
  std::string last_id_;
  uint64_t token_ = 0;
  static inline uint64_t next_token_ = 0; // tokens unique across adapters
};

} // namespace

bool SecEdgarQ6DuckdbScan::ScanPreBatches(
    const std::function<void(const SecEdgarQ6PreBatch &)> &consumer) {
  if (!engine::duckdb::scan::ScanConnection(connection).context) {
    return false;
  }
  StrColumnAdapter adsh, tag, version, stmt, plabel;
  const auto varchar = ::duckdb::LogicalType::VARCHAR;
  NativeScan(connection, "pre",
             {{"adsh", varchar}, {"tag", varchar}, {"version", varchar},
              {"stmt", varchar}, {"plabel", varchar}},
             [&](::duckdb::DataChunk &chunk) {
               const auto n = chunk.size();
               if (n == 0) {
                 return;
               }
               SecEdgarQ6PreBatch batch;
               batch.rows = n;
               adsh.Load(chunk.data[0], n, batch.adsh);
               tag.Load(chunk.data[1], n, batch.tag);
               version.Load(chunk.data[2], n, batch.version);
               stmt.Load(chunk.data[3], n, batch.stmt);
               plabel.Load(chunk.data[4], n, batch.plabel);
               consumer(batch);
             });
  return true;
}

bool SecEdgarQ6DuckdbScan::ScanNumPartitioned(const core::AuxEngineServices &services,
                                              SecEdgarQ6Builder &builder) {
  using namespace engine::duckdb::scan;
  using Partial = SecEdgarQ6Builder::NumPartial;
  builder.MergeNum(core::PartitionedBuild<Partial>(
      services, [] { return Partial{}; },
      [&](std::size_t partitions, auto &&sink) {
        ScanPartitioned(services, connection, "num",
                        {Nullable(TextColumn("adsh")), Nullable(TextColumn("tag")),
                         Nullable(TextColumn("version")), Nullable(TextColumn("uom")),
                         Nullable(DoubleColumn("value"))},
                        partitions, sink);
      },
      [&](Partial &partial, const auto &row) { AddNumRow(builder, partial, row); },
      [](Partial &into, Partial &&from) { into.Merge(std::move(from)); },
      core::kFloatBuildPartitions));
  return true;
}

bool SecEdgarQ6DuckdbScan::ScanNumBatches(
    const std::function<void(const SecEdgarQ6NumBatch &)> &consumer) {
  if (!engine::duckdb::scan::ScanConnection(connection).context) {
    return false;
  }
  StrColumnAdapter adsh, tag, version, uom;
  std::vector<double> values;
  std::vector<uint8_t> valid;
  const auto varchar = ::duckdb::LogicalType::VARCHAR;
  NativeScan(connection, "num",
             {{"adsh", varchar}, {"tag", varchar}, {"version", varchar},
              {"uom", varchar}, {"value", ::duckdb::LogicalType::DOUBLE}},
             [&](::duckdb::DataChunk &chunk) {
               const auto n = chunk.size();
               if (n == 0) {
                 return;
               }
               SecEdgarQ6NumBatch batch;
               batch.rows = n;
               adsh.Load(chunk.data[0], n, batch.adsh);
               tag.Load(chunk.data[1], n, batch.tag);
               version.Load(chunk.data[2], n, batch.version);
               uom.Load(chunk.data[3], n, batch.uom);
               ::duckdb::UnifiedVectorFormat fmt;
               chunk.data[4].ToUnifiedFormat(n, fmt);
               const auto *data = ::duckdb::UnifiedVectorFormat::GetData<double>(fmt);
               values.resize(n);
               valid.resize(n);
               bool all_valid = true;
               for (::duckdb::idx_t i = 0; i < n; ++i) {
                 const auto idx = fmt.sel->get_index(i);
                 const bool ok = fmt.validity.RowIsValid(idx);
                 values[i] = ok ? data[idx] : 0.0;
                 valid[i] = ok ? 1 : 0;
                 all_valid &= ok;
               }
               batch.value = values.data();
               batch.value_valid = all_valid ? nullptr : valid.data();
               consumer(batch);
             });
  return true;
}

bool SecEdgarQ6DuckdbScan::LoadAdshSnapshots(
    const std::vector<std::string> &adshs, std::vector<SecEdgarQ6AdshSnapshot> &out) {
  out.assign(adshs.size(), {});
  std::map<std::string, std::size_t> index;
  for (std::size_t i = 0; i < adshs.size(); ++i) index[adshs[i]] = i;
  const std::string list = InList(adshs);
  StreamQuery(connection,
              "SELECT adsh, name, fy FROM sub WHERE fy IS NOT NULL AND adsh IN " + list,
              [&](::duckdb::DataChunk &chunk) {
                StringColumn adsh(chunk, 0), name(chunk, 1);
                const auto *fy = ::duckdb::FlatVector::GetData<int32_t>(chunk.data[2]);
                for (::duckdb::idx_t row = 0; row < chunk.size(); ++row) {
                  auto it = index.find(adsh.Get(row));
                  if (it == index.end()) continue;
                  auto &snap = out[it->second];
                  snap.in_cohort = true;
                  snap.fy = fy[row];
                  snap.name_is_null = name.IsNull(row);
                  snap.name = name.Get(row);
                }
              });
  StreamQuery(
      connection,
      "SELECT adsh, tag, version, stmt, plabel, COUNT(*)::BIGINT FROM pre WHERE "
      "stmt IS NOT NULL AND tag IS NOT NULL AND version IS NOT NULL AND adsh IN " +
          list + " GROUP BY adsh, tag, version, stmt, plabel",
      [&](::duckdb::DataChunk &chunk) {
        StringColumn adsh(chunk, 0), tag(chunk, 1), version(chunk, 2), stmt(chunk, 3),
            plabel(chunk, 4);
        const auto *count = ::duckdb::FlatVector::GetData<int64_t>(chunk.data[5]);
        for (::duckdb::idx_t row = 0; row < chunk.size(); ++row) {
          auto it = index.find(adsh.Get(row));
          if (it == index.end()) continue;
          SecEdgarQ6PreEntry entry;
          entry.tag = tag.Get(row);
          entry.version = version.Get(row);
          entry.stmt = stmt.Get(row);
          entry.plabel_is_null = plabel.IsNull(row);
          entry.plabel = plabel.Get(row);
          entry.mult = static_cast<uint32_t>(count[row]);
          out[it->second].pre.push_back(std::move(entry));
        }
      });
  StreamQuery(
      connection,
      "SELECT adsh, tag, version, uom, SUM(value)::DOUBLE, COUNT(*)::BIGINT FROM num "
      "WHERE uom IS NOT NULL AND value IS NOT NULL AND adsh IN " + list +
          " GROUP BY adsh, tag, version, uom",
      [&](::duckdb::DataChunk &chunk) {
        StringColumn adsh(chunk, 0), tag(chunk, 1), version(chunk, 2), uom(chunk, 3);
        const auto *sum = ::duckdb::FlatVector::GetData<double>(chunk.data[4]);
        const auto *count = ::duckdb::FlatVector::GetData<int64_t>(chunk.data[5]);
        for (::duckdb::idx_t row = 0; row < chunk.size(); ++row) {
          if (tag.IsNull(row) || version.IsNull(row)) continue;
          auto it = index.find(adsh.Get(row));
          if (it == index.end()) continue;
          out[it->second].nums.push_back(
              {tag.Get(row), version.Get(row), uom.Get(row), sum[row], count[row]});
        }
      });
  return true;
}

void SecEdgarQ6DuckdbScan::ScanNum(
    const std::function<void(const SecEdgarQ6NumInput &)> &consumer) {
  StreamQuery(connection,
              "SELECT adsh, tag, version, uom, value FROM num "
              "WHERE uom IS NOT NULL AND value IS NOT NULL",
              [&](::duckdb::DataChunk &chunk) {
                StringColumn adsh(chunk, 0), tag(chunk, 1), version(chunk, 2),
                    uom(chunk, 3);
                const auto *value =
                    ::duckdb::FlatVector::GetData<double>(chunk.data[4]);
                const auto &value_valid =
                    ::duckdb::FlatVector::Validity(chunk.data[4]);
                SecEdgarQ6NumInput input;
                for (::duckdb::idx_t row = 0; row < chunk.size(); ++row) {
                  if (adsh.IsNull(row) || tag.IsNull(row) || version.IsNull(row)) {
                    continue;
                  }
                  input.adsh = adsh.Get(row);
                  input.tag = tag.Get(row);
                  input.version = version.Get(row);
                  input.uom = uom.Get(row);
                  input.value_is_null = !value_valid.RowIsValid(row);
                  input.value = input.value_is_null ? 0.0 : value[row];
                  consumer(input);
                }
              });
}

void SecEdgarQ6DuckdbScan::ScanSub(
    const std::function<void(const SecEdgarQ6SubInput &)> &consumer) {
  StreamQuery(connection, "SELECT adsh, name, fy FROM sub WHERE fy IS NOT NULL",
              [&](::duckdb::DataChunk &chunk) {
                StringColumn adsh(chunk, 0), name(chunk, 1);
                const auto *fy = ::duckdb::FlatVector::GetData<int32_t>(chunk.data[2]);
                SecEdgarQ6SubInput input;
                for (::duckdb::idx_t row = 0; row < chunk.size(); ++row) {
                  if (adsh.IsNull(row)) {
                    continue;
                  }
                  input.fy = fy[row];
                  input.adsh = adsh.Get(row);
                  input.name_is_null = name.IsNull(row);
                  input.name = name.Get(row);
                  consumer(input);
                }
              });
}

void SecEdgarQ6DuckdbScan::ScanPre(
    const std::function<void(const SecEdgarQ6PreInput &)> &consumer) {
  StreamQuery(
      connection,
      "SELECT p.adsh, p.tag, p.version, p.stmt, p.plabel FROM pre p "
      "WHERE p.stmt IS NOT NULL AND p.tag IS NOT NULL AND p.version IS NOT NULL AND "
      "p.adsh IN (SELECT adsh FROM sub WHERE fy IS NOT NULL)",
      [&](::duckdb::DataChunk &chunk) {
        StringColumn adsh(chunk, 0), tag(chunk, 1), version(chunk, 2),
            stmt(chunk, 3), plabel(chunk, 4);
        SecEdgarQ6PreInput input;
        for (::duckdb::idx_t row = 0; row < chunk.size(); ++row) {
          input.adsh = adsh.Get(row);
          input.tag = tag.Get(row);
          input.version = version.Get(row);
          input.stmt = stmt.Get(row);
          input.plabel_is_null = plabel.IsNull(row);
          input.plabel = plabel.Get(row);
          consumer(input);
        }
      });
}

bool SecEdgarQ6DuckdbScan::ScanGroupAggregates(
    const std::function<void(const SecEdgarQ6GroupAggregateInput &)> &consumer) {
  StreamQuery(
      connection,
      "SELECT s.fy, s.name, p.stmt, n.tag, p.plabel, n.uom, SUM(n.value)::DOUBLE, "
      "COUNT(*)::BIGINT "
      "FROM num n JOIN sub s ON n.adsh = s.adsh "
      "JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version "
      "WHERE n.uom IS NOT NULL AND p.stmt IS NOT NULL AND s.fy IS NOT NULL "
      "AND n.value IS NOT NULL GROUP BY s.fy, s.name, p.stmt, n.tag, p.plabel, n.uom",
      [&](::duckdb::DataChunk &chunk) {
        const auto *fy = ::duckdb::FlatVector::GetData<int32_t>(chunk.data[0]);
        StringColumn name(chunk, 1), stmt(chunk, 2), tag(chunk, 3), plabel(chunk, 4),
            uom(chunk, 5);
        const auto *sum = ::duckdb::FlatVector::GetData<double>(chunk.data[6]);
        const auto *count = ::duckdb::FlatVector::GetData<int64_t>(chunk.data[7]);
        SecEdgarQ6GroupAggregateInput input;
        for (::duckdb::idx_t row = 0; row < chunk.size(); ++row) {
          input.fy = fy[row];
          input.name_is_null = name.IsNull(row);
          input.name = name.Get(row);
          input.stmt = stmt.Get(row);
          input.tag = tag.Get(row);
          input.plabel_is_null = plabel.IsNull(row);
          input.plabel = plabel.Get(row);
          input.uom = uom.Get(row);
          input.sum = sum[row];
          input.count = count[row];
          consumer(input);
        }
      });
  return true;
}

} // namespace accelerator_codegen::aux_state::query
