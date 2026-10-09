#include "sec_edgar/q6/duckdb/aux/duckdb_scan.hpp"

#include <map>
#include <stdexcept>
#include <string>

#include "accelerator_codegen/runtime/duckdb/aux_query_registry.hpp"
#include "duckdb.hpp"

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

} // namespace

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
