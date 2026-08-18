// GENERATED BACKEND
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include "accelerator_codegen/runtime/postgres/bind_helper.hpp"

#include <sstream>
#include <stdexcept>
#include <string>

extern "C" {
#include "postgres.h"
#include "executor/spi.h"
}

#ifdef ERROR
#undef ERROR
#endif
#ifdef Max
#undef Max
#endif
#ifdef Min
#undef Min
#endif

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

std::string EscapeSqlLiteral(const std::string &value) {
  std::string quoted = "'";
  for (const char ch : value) {
    if (ch == '\'') {
      quoted += "''";
    } else {
      quoted += ch;
    }
  }
  quoted += "'";
  return quoted;
}

class SpiSession {
public:
  SpiSession() {
    const int rc = SPI_connect();
    if (rc == SPI_OK_CONNECT) {
      connected_ = true;
    } else if (rc == SPI_ERROR_CONNECT) {
      connected_ = false;
    } else {
      throw std::runtime_error("SPI_connect failed with status " + std::to_string(rc));
    }
  }

  ~SpiSession() {
    if (connected_) {
      SPI_finish();
    }
  }

  SpiSession(const SpiSession &) = delete;
  SpiSession &operator=(const SpiSession &) = delete;

  void Execute(const std::string &sql) {
    const int status = SPI_execute(sql.c_str(), true, 0);
    if (status != SPI_OK_SELECT) {
      throw std::runtime_error("SPI_execute failed with status " + std::to_string(status) +
                               " for query: " + sql);
    }
  }

private:
  bool connected_ = false;
};

} // namespace

int32_t ResolveNationKey(const std::string &nation_name) {
  const std::string sql =
      "SELECT DISTINCT n_nationkey FROM nation WHERE n_name = " + EscapeSqlLiteral(nation_name);
  SpiSession session;
  session.Execute(sql);
  if (SPI_processed != 1) {
    throw std::runtime_error("nation name \"" + nation_name +
                             "\" did not resolve to exactly one nation key (got " +
                             std::to_string(SPI_processed) + " rows)");
  }
  bool is_null = false;
  const Datum datum = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &is_null);
  if (is_null) {
    throw std::runtime_error("nation key lookup returned NULL");
  }
  return static_cast<int32_t>(DatumGetInt32(datum));
}

void FillRegionNations(const std::string &region_name, std::array<bool, 25> &region_nations) {
  region_nations.fill(false);
  const std::string sql =
      "SELECT n.n_nationkey FROM nation n JOIN region r ON n.n_regionkey = r.r_regionkey "
      "WHERE r.r_name = " +
      EscapeSqlLiteral(region_name);
  SpiSession session;
  session.Execute(sql);
  for (uint64_t row = 0; row < SPI_processed; ++row) {
    bool is_null = false;
    const Datum datum =
        SPI_getbinval(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 1, &is_null);
    if (is_null) {
      continue;
    }
    const auto nationkey = static_cast<int32_t>(DatumGetInt32(datum));
    if (nationkey >= 0 && nationkey < static_cast<int32_t>(region_nations.size())) {
      region_nations[static_cast<size_t>(nationkey)] = true;
    }
  }
}

void FillRegionNationsAndNames(const std::string &region_name,
                               std::array<bool, 25> &region_nations,
                               std::unordered_map<int32_t, std::string> &nation_names) {
  region_nations.fill(false);
  nation_names.clear();
  const std::string sql =
      "SELECT n.n_nationkey, n.n_name FROM nation n JOIN region r ON n.n_regionkey = r.r_regionkey "
      "WHERE r.r_name = " +
      EscapeSqlLiteral(region_name);
  SpiSession session;
  session.Execute(sql);
  for (uint64_t row = 0; row < SPI_processed; ++row) {
    bool is_null = false;
    const Datum key_datum =
        SPI_getbinval(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 1, &is_null);
    if (is_null) {
      continue;
    }
    const auto nationkey = static_cast<int32_t>(DatumGetInt32(key_datum));
    is_null = false;
    const Datum name_datum =
        SPI_getbinval(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 2, &is_null);
    std::string nation_name;
    if (!is_null) {
      char *text = SPI_getvalue(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 2);
      if (text) {
        nation_name = text;
      }
    }
    if (nationkey >= 0 && nationkey < static_cast<int32_t>(region_nations.size())) {
      region_nations[static_cast<size_t>(nationkey)] = true;
    }
    nation_names.emplace(nationkey, nation_name);
  }
}

void ResolvePartKeys(const std::string &part_type, std::vector<int64_t> &part_keys) {
  part_keys.clear();
  std::ostringstream like_sql;
  like_sql << "SELECT p_partkey FROM part WHERE p_type LIKE '%";
  for (const char ch : part_type) {
    if (ch == '\'') {
      like_sql << "''";
    } else {
      like_sql << ch;
    }
  }
  like_sql << "%'";
  SpiSession session;
  session.Execute(like_sql.str());
  part_keys.reserve(SPI_processed);
  for (uint64_t row = 0; row < SPI_processed; ++row) {
    bool is_null = false;
    const Datum datum =
        SPI_getbinval(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 1, &is_null);
    if (is_null) {
      continue;
    }
    part_keys.push_back(DatumGetInt64(datum));
  }
}

} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
