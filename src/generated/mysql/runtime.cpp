// GENERATED BACKEND
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q1/mysql/aux/module.hpp"
#include "tpch/q2/mysql/aux/module.hpp"
#include "tpch/q3/mysql/aux/module.hpp"
#include "tpch/q4/mysql/aux/module.hpp"
#include "tpch/q5/mysql/aux/module.hpp"
#include "tpch/q6/mysql/aux/module.hpp"
#include "tpch/q7/mysql/aux/module.hpp"
#include "tpch/q8/mysql/aux/module.hpp"
#include "tpch/q9/mysql/aux/module.hpp"
#include "tpch/q10/mysql/aux/module.hpp"
#include "tpch/q11/mysql/aux/module.hpp"
#include "tpch/q12/mysql/aux/module.hpp"
#include "tpch/q13/mysql/aux/module.hpp"
#include "tpch/q14/mysql/aux/module.hpp"
#include "tpch/q15/mysql/aux/module.hpp"
#include "tpch/q16/mysql/aux/module.hpp"
#include "tpch/q17/mysql/aux/module.hpp"
#include "tpch/q18/mysql/aux/module.hpp"
#include "tpch/q19/mysql/aux/module.hpp"
#include "tpch/q20/mysql/aux/module.hpp"
#include "tpch/q21/mysql/aux/module.hpp"
#include "tpch/q22/mysql/aux/module.hpp"
#if defined(ACCEL_HAVE_Q21_VERUS)
#include "tpch/q21/verus/module.hpp"
#endif

#include "accelerator_codegen/stateful/core/aux_txn_buffer.hpp"
#include "accelerator_codegen/stateful/core/aux_calibration.hpp"
#include "accelerator_codegen/stateful/mysql/mysql_xact_callbacks.hpp"
#include "accelerator_codegen/stateful/mysql/mysql_aux_backend.hpp"
#include "accelerator_codegen/stateful/mysql/mysql_state_store.hpp"
#include "accelerator_codegen/stateful/mysql/mysql_tpch_table_ids.hpp"

extern "C" int mysql_accel_get_observer_mutation_count();

// xact + mutation registration uses the current substrate APIs
// (SetPostgresActiveBackend / RegisterPostgresXactCallbacks).

#include "mysql/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace detail {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;

bool FileReadable(const std::string &path) {
  std::ifstream input(path);
  return input.good();
}

std::string Basename(const char *path) {
  if (!path || !*path) {
    return "";
  }
  const std::string text(path);
  const auto slash = text.find_last_of('/');
  return slash == std::string::npos ? text : text.substr(slash + 1);
}

std::string MysqlCostModelBasename(const std::string &basename) {
  // q1_aux_cost_model.json -> q1_aux_cost_model_mysql.json
  static constexpr char kSuffix[] = "_aux_cost_model.json";
  const auto pos = basename.rfind(kSuffix);
  if (pos == std::string::npos || pos + sizeof(kSuffix) - 1 != basename.size()) {
    return "";
  }
  return basename.substr(0, pos) + "_aux_cost_model_mysql.json";
}

std::string ResolveCostModelPath(const char *env_name,
                                 const char *default_relative_path) {
  if (const char *path = std::getenv(env_name); path && *path) {
    return path;
  }
  if (!default_relative_path || !*default_relative_path) {
    return "";
  }
  std::vector<std::string> candidates;
  const std::string basename = Basename(default_relative_path);
  const std::string pg_basename = MysqlCostModelBasename(basename);
  auto PushPair = [&](const std::string &dir_prefix, const std::string &rel) {
    if (!pg_basename.empty()) {
      // Prefer MySQL-specific calibration when present.
      if (!dir_prefix.empty()) {
        candidates.push_back(dir_prefix + pg_basename);
      } else if (rel.size() >= basename.size()) {
        candidates.push_back(rel.substr(0, rel.size() - basename.size()) +
                             pg_basename);
      }
    }
    if (!dir_prefix.empty()) {
      candidates.push_back(dir_prefix + basename);
    } else {
      candidates.push_back(rel);
    }
  };
  if (const char *config_dir = std::getenv("ACCELERATOR_CODEGEN_CONFIG_DIR");
      config_dir && *config_dir) {
    const std::string prefix = std::string(config_dir) + "/";
    PushPair(prefix, prefix + basename);
  }
  PushPair("", default_relative_path);
  PushPair("/accel-src/", std::string("/accel-src/") + default_relative_path);
  PushPair("/accel-src/config/",
           std::string("/accel-src/config/") + basename);
  PushPair("/tmp/accel-codegen-src/",
           std::string("/tmp/accel-codegen-src/") + default_relative_path);
  for (const auto &candidate : candidates) {
    if (FileReadable(candidate)) {
      return candidate;
    }
  }
  return "";
}


// AuxDate uses Unix-epoch days. MySQL DATE text is YYYY-MM-DD.
constexpr int kUnixEpochDays = 719468;

int DaysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int>(doe) - kUnixEpochDays;
}

int32_t ParseUnixDays(const std::string &text) {
  if (text.size() < 10) {
    return 0;
  }
  int y = std::atoi(text.substr(0, 4).c_str());
  unsigned m = static_cast<unsigned>(std::atoi(text.substr(5, 2).c_str()));
  unsigned d = static_cast<unsigned>(std::atoi(text.substr(8, 2).c_str()));
  if (y == 0 || m == 0 || d == 0) {
    return 0;
  }
  return DaysFromCivil(y, m, d);
}

std::string MysqlCliPrefix() {
  const char *host = std::getenv("MYSQL_HOST");
  const char *port = std::getenv("MYSQL_PORT");
  const char *user = std::getenv("MYSQL_USER");
  const char *password = std::getenv("MYSQL_PWD");
  const char *db = std::getenv("TPCH_MYSQL_DB");
  if (!host || !*host) host = "127.0.0.1";
  if (!port || !*port) port = "3306";
  if (!user || !*user) user = "root";
  if (!password) password = "mysql";
  if (!db || !*db) db = "accel_mysql_test";
  std::ostringstream cmd;
  cmd << "MYSQL_PWD=" << password << " mysql -h " << host << " -P " << port
      << " -u" << user << " -D " << db
      << " --batch --raw --skip-column-names -e ";
  return cmd.str();
}

std::string ShellEscape(const std::string &s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'')
      out += "'\\''";
    else
      out += c;
  }
  out += "'";
  return out;
}

std::vector<std::string> SplitTsv(const std::string &line) {
  std::vector<std::string> cols;
  std::string cur;
  for (char c : line) {
    if (c == '\t') {
      cols.push_back(cur);
      cur.clear();
    } else if (c != '\r') {
      cur += c;
    }
  }
  cols.push_back(cur);
  return cols;
}

template <typename Callback>
void ScanTypedTuples(const std::string &sql, Callback &&callback) {
  const std::string cmd = MysqlCliPrefix() + ShellEscape(sql) + " 2>/dev/null";
  FILE *pipe = popen(cmd.c_str(), "r");
  if (!pipe) {
    throw std::runtime_error("mysql scan popen failed");
  }
  char buf[1 << 16];
  while (fgets(buf, sizeof(buf), pipe)) {
    std::string line(buf);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    callback(SplitTsv(line));
  }
  const int rc = pclose(pipe);
  if (rc != 0) {
    throw std::runtime_error("mysql scan failed: " + sql);
  }
}

const std::string &Col(const std::vector<std::string> &cols, int attribute) {
  static const std::string kEmpty;
  if (attribute < 0 || static_cast<std::size_t>(attribute) >= cols.size()) {
    return kEmpty;
  }
  return cols[static_cast<std::size_t>(attribute)];
}

int64_t TypedInt(const std::vector<std::string> &cols, int attribute) {
  const auto &text = Col(cols, attribute);
  if (text.empty() || text == "NULL") {
    return 0;
  }
  return std::strtoll(text.c_str(), nullptr, 10);
}

int32_t TypedDate(const std::vector<std::string> &cols, int attribute) {
  const auto &text = Col(cols, attribute);
  if (text.empty() || text == "NULL") {
    return 0;
  }
  return ParseUnixDays(text);
}

std::string TypedText(const std::vector<std::string> &cols, int attribute) {
  const auto &text = Col(cols, attribute);
  if (text == "NULL") {
    return "";
  }
  return text;
}

int64_t TypedDecimalRaw(const std::vector<std::string> &cols, int attribute) {
  const auto &text = Col(cols, attribute);
  if (text.empty() || text == "NULL") {
    return 0;
  }
  char *end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (end == text.c_str()) {
    return 0;
  }
  return static_cast<int64_t>(std::llround(value * 100.0));
}

char TypedChar(const std::vector<std::string> &cols, int attribute) {
  const auto text = TypedText(cols, attribute);
  return text.empty() ? '\0' : text[0];
}

namespace scans {

void ScanTpchQ1Lineitems(
    const std::function<void(const query::TpchQ1LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_shipdate, l_quantity, l_extendedprice, l_discount, l_tax, "
      "l_returnflag, l_linestatus FROM lineitem",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedDate(cols, 0),
                  TypedDecimalRaw(cols, 1), 2,
                  TypedDecimalRaw(cols, 2), 2,
                  TypedDecimalRaw(cols, 3), 2,
                  TypedDecimalRaw(cols, 4), 2,
                  TypedChar(cols, 5),
                  TypedChar(cols, 6)});
      });
}

void ScanTpchQ3Customers(
    const std::function<void(const query::TpchQ3CustomerInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT c_custkey, c_mktsegment FROM customer",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedText(cols, 1)});
      });
}

void ScanTpchQ3Orders(
    const std::function<void(const query::TpchQ3OrderInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_orderkey, o_orderdate, o_custkey, o_shippriority FROM orders",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedDate(cols, 1),
                  TypedInt(cols, 2),
                  static_cast<int32_t>(TypedInt(cols, 3))});
      });
}

void ScanTpchQ3Lineitems(
    const std::function<void(const query::TpchQ3LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_orderkey, l_extendedprice, l_discount, l_shipdate FROM lineitem",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedDecimalRaw(cols, 1), 2,
                  TypedDecimalRaw(cols, 2), 2,
                  TypedDate(cols, 3)});
      });
}

void ScanTpchQ6Lineitems(
    const std::function<void(const query::TpchQ6LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_shipdate, l_discount, l_quantity, l_extendedprice FROM lineitem",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedDate(cols, 0),
                  TypedDecimalRaw(cols, 1), 2,
                  TypedDecimalRaw(cols, 2), 2,
                  TypedDecimalRaw(cols, 3), 2});
      });
}

void ScanTpchQ10Customers(
    const std::function<void(const query::TpchQ10CustomerInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT c_custkey, c_name, c_acctbal, c_nationkey, c_address, "
      "c_phone, c_comment FROM customer",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedText(cols, 1),
                  TypedDecimalRaw(cols, 2),
                  static_cast<int32_t>(TypedInt(cols, 3)),
                  TypedText(cols, 4),
                  TypedText(cols, 5),
                  TypedText(cols, 6)});
      });
}

void LookupTpchQ10Customers(
    const std::vector<int64_t> &custkeys,
    std::unordered_map<int64_t, query::TpchQ10Customer> &out) {
  out.clear();
  if (custkeys.empty())
    return;
  std::ostringstream sql;
  sql << "SELECT c_custkey, c_name, c_acctbal, c_nationkey, c_address, "
         "c_phone, c_comment FROM customer WHERE c_custkey IN (";
  for (std::size_t i = 0; i < custkeys.size(); ++i) {
    if (i > 0)
      sql << ',';
    sql << custkeys[i];
  }
  sql << ')';
  ScanTypedTuples(sql.str(), [&](const std::vector<std::string> &cols) {
    const auto key = TypedInt(cols, 0);
    out.emplace(key, query::TpchQ10Customer{
                         TypedText(cols, 1),
                         TypedDecimalRaw(cols, 2),
                         static_cast<int32_t>(TypedInt(cols, 3)),
                         TypedText(cols, 4),
                         TypedText(cols, 5),
                         TypedText(cols, 6)});
  });
}

void ScanTpchQ10Nations(
    const std::function<void(const query::TpchQ10NationInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT n_nationkey, n_name FROM nation",
      [&](const std::vector<std::string> &cols) {
        consumer({static_cast<int32_t>(TypedInt(cols, 0)),
                  TypedText(cols, 1)});
      });
}

void ScanTpchQ10Orders(
    const std::function<void(const query::TpchQ10OrderInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_orderkey, o_custkey, o_orderdate FROM orders",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedInt(cols, 1),
                  TypedDate(cols, 2)});
      });
}

void ScanTpchQ10Lineitems(
    const std::function<void(const query::TpchQ10LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_orderkey, l_extendedprice, l_discount, l_returnflag "
      "FROM lineitem",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedDecimalRaw(cols, 1), 2,
                  TypedDecimalRaw(cols, 2), 2,
                  TypedText(cols, 3) == "R"});
      });
}

void ScanTpchQ12Orders(
    const std::function<void(const query::TpchQ12OrderInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_orderkey, o_orderpriority FROM orders",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedText(cols, 1)});
      });
}

void ScanTpchQ12Lineitems(
    const std::function<void(const query::TpchQ12LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_orderkey, l_shipmode, l_commitdate, l_receiptdate, l_shipdate "
      "FROM lineitem",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedText(cols, 1),
                  TypedDate(cols, 2),
                  TypedDate(cols, 3),
                  TypedDate(cols, 4)});
      });
}

void ScanTpchQ14Parts(
    const std::function<void(const query::TpchQ14PartInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT p_partkey, p_type FROM part",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedText(cols, 1)});
      });
}

void ScanTpchQ14Lineitems(
    const std::function<void(const query::TpchQ14LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_partkey, l_extendedprice, l_discount, l_shipdate FROM lineitem",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedDecimalRaw(cols, 1), 2,
                  TypedDecimalRaw(cols, 2), 2,
                  TypedDate(cols, 3)});
      });
}

void ScanTpchQ9Parts(
    const std::function<void(const query::TpchQ9PartInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT p_partkey, p_name FROM part",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedText(cols, 1)});
      });
}

void ScanTpchQ9Suppliers(
    const std::function<void(const query::TpchQ9SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_nationkey FROM supplier",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  static_cast<int32_t>(TypedInt(cols, 1))});
      });
}

void ScanTpchQ9Nations(
    const std::function<void(const query::TpchQ9NationInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT n_nationkey, n_name FROM nation",
      [&](const std::vector<std::string> &cols) {
        consumer({static_cast<int32_t>(TypedInt(cols, 0)),
                  TypedText(cols, 1)});
      });
}

void ScanTpchQ9Orders(
    const std::function<void(const query::TpchQ9OrdersInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_orderkey, o_orderdate FROM orders",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedDate(cols, 1)});
      });
}

void ScanTpchQ9Partsupps(
    const std::function<void(const query::TpchQ9PartsuppInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT ps_partkey, ps_suppkey, ps_supplycost FROM partsupp",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedInt(cols, 1),
                  TypedDecimalRaw(cols, 2)});
      });
}

void ScanTpchQ9Lineitems(
    const std::function<void(const query::TpchQ9LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_partkey, l_suppkey, l_orderkey, l_quantity, l_extendedprice, "
      "l_discount FROM lineitem",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedInt(cols, 1),
                  TypedInt(cols, 2),
                  TypedDecimalRaw(cols, 3),
                  TypedDecimalRaw(cols, 4),
                  TypedDecimalRaw(cols, 5)});
      });
}

void ScanTpchQ11Partsupp(
    const std::function<void(const query::TpchQ11PartsuppInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT ps_partkey, ps_suppkey, ps_availqty, ps_supplycost FROM partsupp",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedInt(cols, 1),
                  TypedInt(cols, 2),
                  TypedDecimalRaw(cols, 3), 2});
      });
}

void ScanTpchQ11Suppliers(
    const std::function<void(const query::TpchQ11SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_nationkey FROM supplier",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  static_cast<int32_t>(TypedInt(cols, 1))});
      });
}

void ScanTpchQ11Nations(
    const std::function<void(const query::TpchQ11NationInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT n_nationkey, n_name FROM nation",
      [&](const std::vector<std::string> &cols) {
        consumer({static_cast<int32_t>(TypedInt(cols, 0)),
                  TypedText(cols, 1)});
      });
}

void ScanTpchQ17Parts(
    const std::function<void(const query::TpchQ17PartInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT p_partkey, p_brand, p_container FROM part",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedText(cols, 1),
                  TypedText(cols, 2)});
      });
}

void ScanTpchQ17Lineitems(
    const std::function<void(const query::TpchQ17LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_partkey, l_quantity, l_extendedprice FROM lineitem",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedDecimalRaw(cols, 1),
                  TypedDecimalRaw(cols, 2)});
      });
}

void ScanTpchQ17PartQuantityAggregatesForPart(
    int64_t partkey,
    const std::function<void(const query::TpchQ17PartQuantityAggregate &)>
        &consumer) {
  const std::string sql =
      "SELECT CAST(ROUND(l_quantity * 100) AS SIGNED), "
      "SUM(CAST(ROUND(l_extendedprice * 100) AS SIGNED)), "
      "CAST(COUNT(*) AS SIGNED) "
      "FROM lineitem WHERE l_partkey = " +
      std::to_string(partkey) + " GROUP BY 1";
  ScanTypedTuples(sql, [&](const std::vector<std::string> &cols) {
    consumer({partkey, TypedInt(cols, 0),
              TypedInt(cols, 1), TypedInt(cols, 2)});
  });
}

void ScanTpchQ18Lineitems(
    const std::function<void(const query::TpchQ18LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_orderkey, l_quantity FROM lineitem",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedDecimalRaw(cols, 1)});
      });
}

void ScanTpchQ18Orders(
    const std::function<void(const query::TpchQ18OrderInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_orderkey, o_custkey, o_orderdate, o_totalprice FROM orders",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedInt(cols, 1),
                  TypedDate(cols, 2),
                  TypedDecimalRaw(cols, 3)});
      });
}

void ScanTpchQ18Customers(
    const std::function<void(const query::TpchQ18CustomerInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT c_custkey, c_name FROM customer",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedText(cols, 1)});
      });
}

bool LookupTpchQ18Order(int64_t orderkey, query::TpchQ18OrderInput &out) {
  bool found = false;
  const std::string sql =
      "SELECT o_orderkey, o_custkey, o_orderdate, o_totalprice "
      "FROM orders WHERE o_orderkey = " +
      std::to_string(orderkey);
  ScanTypedTuples(sql, [&](const std::vector<std::string> &cols) {
    out = {TypedInt(cols, 0), TypedInt(cols, 1), TypedDate(cols, 2),
           TypedDecimalRaw(cols, 3)};
    found = out.key == orderkey;
  });
  return found;
}

void ScanTpchQ21Suppliers(
    const std::function<void(const query::TpchQ21SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_name, s_nationkey FROM supplier",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedText(cols, 1),
                  static_cast<int32_t>(TypedInt(cols, 2))});
      });
}

void ScanTpchQ21Nations(
    const std::function<void(const query::TpchQ21NationInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT n_nationkey, n_name FROM nation",
      [&](const std::vector<std::string> &cols) {
        consumer({static_cast<int32_t>(TypedInt(cols, 0)),
                  TypedText(cols, 1)});
      });
}

void ScanTpchQ21Orders(
    const std::function<void(const query::TpchQ21OrderInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_orderkey, o_orderstatus FROM orders WHERE o_orderstatus = 'F'",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedChar(cols, 1)});
      });
}

void ScanTpchQ21Lineitems(
    const std::function<void(const query::TpchQ21LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l.l_orderkey, l.l_suppkey, l.l_commitdate, l.l_receiptdate "
      "FROM lineitem l "
      "INNER JOIN orders o ON l.l_orderkey = o.o_orderkey AND "
      "o.o_orderstatus = 'F'",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedInt(cols, 1),
                  TypedDate(cols, 2),
                  TypedDate(cols, 3)});
      });
}

void ScanTpchQ22Customers(
    const std::function<void(const query::TpchQ22CustomerInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT c_custkey, c_phone, c_acctbal FROM customer",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedText(cols, 1),
                  TypedDecimalRaw(cols, 2), 2});
      });
}

void ScanTpchQ22Orders(
    const std::function<void(const query::TpchQ22OrderInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_custkey FROM orders",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0)});
      });
}

void ScanTpchQ2Parts(
    const std::function<void(const query::TpchQ2PartInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT p_partkey, p_mfgr, p_type, p_size FROM part",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedText(cols, 1),
                  TypedText(cols, 2),
                  static_cast<int32_t>(TypedInt(cols, 3))});
      });
}

void ScanTpchQ2Suppliers(
    const std::function<void(const query::TpchQ2SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_name, s_address, s_nationkey, s_phone, s_acctbal, "
      "s_comment FROM supplier",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedText(cols, 1),
                  TypedText(cols, 2),
                  static_cast<int32_t>(TypedInt(cols, 3)),
                  TypedText(cols, 4),
                  TypedDecimalRaw(cols, 5),
                  TypedText(cols, 6)});
      });
}

void ScanTpchQ2Partsupps(
    const std::function<void(const query::TpchQ2PartsuppInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT ps_partkey, ps_suppkey, ps_supplycost FROM partsupp",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedInt(cols, 1),
                  TypedDecimalRaw(cols, 2)});
      });
}

void ScanTpchQ2Nations(
    const std::function<void(const query::TpchQ2NationInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT n_nationkey, n_name, n_regionkey FROM nation",
      [&](const std::vector<std::string> &cols) {
        consumer({static_cast<int32_t>(TypedInt(cols, 0)),
                  TypedText(cols, 1),
                  static_cast<int32_t>(TypedInt(cols, 2))});
      });
}

void ScanTpchQ2Regions(
    const std::function<void(const query::TpchQ2RegionInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT r_regionkey, r_name FROM region",
      [&](const std::vector<std::string> &cols) {
        consumer({static_cast<int32_t>(TypedInt(cols, 0)),
                  TypedText(cols, 1)});
      });
}

void ScanTpchQ4Orders(
    const std::function<void(const query::TpchQ4OrderInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_orderkey, o_orderdate, o_orderpriority FROM orders",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedDate(cols, 1),
                  TypedText(cols, 2)});
      });
}

void ScanTpchQ4Lineitems(
    const std::function<void(const query::TpchQ4LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_orderkey, l_commitdate, l_receiptdate FROM lineitem",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedDate(cols, 1),
                  TypedDate(cols, 2)});
      });
}

void ScanTpchQ5Regions(
    const std::function<void(const query::TpchQ5RegionInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT r_regionkey, r_name FROM region",
      [&](const std::vector<std::string> &cols) {
        consumer({static_cast<int32_t>(TypedInt(cols, 0)),
                  TypedText(cols, 1)});
      });
}

void ScanTpchQ5Nations(
    const std::function<void(const query::TpchQ5NationInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT n_nationkey, n_name, n_regionkey FROM nation",
      [&](const std::vector<std::string> &cols) {
        consumer({static_cast<int32_t>(TypedInt(cols, 0)),
                  TypedText(cols, 1),
                  static_cast<int32_t>(TypedInt(cols, 2))});
      });
}

void ScanTpchQ5Customers(
    const std::function<void(const query::TpchQ5CustomerInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT c_custkey, c_nationkey FROM customer",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  static_cast<int32_t>(TypedInt(cols, 1))});
      });
}

void ScanTpchQ5Suppliers(
    const std::function<void(const query::TpchQ5SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_nationkey FROM supplier",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  static_cast<int32_t>(TypedInt(cols, 1))});
      });
}

void ScanTpchQ5Orders(
    const std::function<void(const query::TpchQ5OrderInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_orderkey, o_custkey, o_orderdate FROM orders",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedInt(cols, 1),
                  TypedDate(cols, 2)});
      });
}

void ScanTpchQ5Lineitems(
    const std::function<void(const query::TpchQ5LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_orderkey, l_suppkey, l_extendedprice, l_discount FROM lineitem",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedInt(cols, 1),
                  TypedDecimalRaw(cols, 2), 2,
                  TypedDecimalRaw(cols, 3), 2});
      });
}

void ScanTpchQ7Nations(
    const std::function<void(const query::TpchQ7NationInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT n_nationkey, n_name FROM nation",
      [&](const std::vector<std::string> &cols) {
        consumer({static_cast<int32_t>(TypedInt(cols, 0)),
                  TypedText(cols, 1)});
      });
}

void ScanTpchQ7Suppliers(
    const std::function<void(const query::TpchQ7SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_nationkey FROM supplier",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  static_cast<int32_t>(TypedInt(cols, 1))});
      });
}

void ScanTpchQ7Customers(
    const std::function<void(const query::TpchQ7CustomerInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT c_custkey, c_nationkey FROM customer",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  static_cast<int32_t>(TypedInt(cols, 1))});
      });
}

void ScanTpchQ7Orders(
    const std::function<void(const query::TpchQ7OrderInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_orderkey, o_custkey FROM orders",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedInt(cols, 1)});
      });
}

void ScanTpchQ7Lineitems(
    const std::function<void(const query::TpchQ7LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_orderkey, l_suppkey, l_extendedprice, l_discount, l_shipdate "
      "FROM lineitem",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedInt(cols, 1),
                  TypedDecimalRaw(cols, 2), 2,
                  TypedDecimalRaw(cols, 3), 2,
                  TypedDate(cols, 4)});
      });
}

void ScanTpchQ8Parts(
    const std::function<void(const query::TpchQ8PartInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT p_partkey, p_type FROM part",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedText(cols, 1)});
      });
}

void ScanTpchQ8Suppliers(
    const std::function<void(const query::TpchQ8SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_nationkey FROM supplier",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  static_cast<int32_t>(TypedInt(cols, 1))});
      });
}

void ScanTpchQ8Customers(
    const std::function<void(const query::TpchQ8CustomerInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT c_custkey, c_nationkey FROM customer",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  static_cast<int32_t>(TypedInt(cols, 1))});
      });
}

void ScanTpchQ8Orders(
    const std::function<void(const query::TpchQ8OrdersInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_orderkey, o_custkey, o_orderdate FROM orders",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedInt(cols, 1),
                  TypedDate(cols, 2)});
      });
}

void ScanTpchQ8Lineitems(
    const std::function<void(const query::TpchQ8LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_partkey, l_orderkey, l_suppkey, l_extendedprice, l_discount "
      "FROM lineitem",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedInt(cols, 1),
                  TypedInt(cols, 2),
                  TypedDecimalRaw(cols, 3),
                  TypedDecimalRaw(cols, 4)});
      });
}

void ScanTpchQ8Nations(
    const std::function<void(const query::TpchQ8NationInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT n_nationkey, n_name, n_regionkey FROM nation",
      [&](const std::vector<std::string> &cols) {
        consumer({static_cast<int32_t>(TypedInt(cols, 0)),
                  TypedText(cols, 1),
                  static_cast<int32_t>(TypedInt(cols, 2))});
      });
}

void ScanTpchQ8Regions(
    const std::function<void(const query::TpchQ8RegionInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT r_regionkey, r_name FROM region",
      [&](const std::vector<std::string> &cols) {
        consumer({static_cast<int32_t>(TypedInt(cols, 0)),
                  TypedText(cols, 1)});
      });
}

void ScanTpchQ13Customers(
    const std::function<void(const query::TpchQ13CustomerInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT c_custkey FROM customer",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0)});
      });
}

void ScanTpchQ13Orders(
    const std::function<void(const query::TpchQ13OrderInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_custkey, o_comment FROM orders",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedText(cols, 1)});
      });
}

void ScanTpchQ15Suppliers(
    const std::function<void(const query::TpchQ15SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_name, s_address, s_phone FROM supplier",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedText(cols, 1),
                  TypedText(cols, 2),
                  TypedText(cols, 3)});
      });
}

void ScanTpchQ15Lineitems(
    const std::function<void(const query::TpchQ15LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_suppkey, l_extendedprice, l_discount, l_shipdate FROM lineitem",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedDate(cols, 3),
                  TypedInt(cols, 0),
                  TypedDecimalRaw(cols, 1), 2,
                  TypedDecimalRaw(cols, 2), 2});
      });
}

void ScanTpchQ15LineitemsForShipdateRange(
    int32_t start_days, int32_t end_days,
    const std::function<void(const query::TpchQ15LineitemInput &)> &consumer) {
  if (end_days <= start_days) {
    return;
  }
  const std::string sql =
      "SELECT l_suppkey, l_extendedprice, l_discount, l_shipdate FROM lineitem "
      "WHERE l_shipdate >= DATE_ADD('1970-01-01', INTERVAL " +
      std::to_string(start_days) +
      " DAY) AND l_shipdate < DATE_ADD('1970-01-01', INTERVAL " + std::to_string(end_days) +
      " DAY)";
  ScanTypedTuples(sql, [&](const std::vector<std::string> &cols) {
    consumer({TypedDate(cols, 3), TypedInt(cols, 0),
              TypedDecimalRaw(cols, 1), 2,
              TypedDecimalRaw(cols, 2), 2});
  });
}

void ScanTpchQ16Suppliers(
    const std::function<void(const query::TpchQ16SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_comment FROM supplier",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedText(cols, 1)});
      });
}

void ScanTpchQ16Parts(
    const std::function<void(const query::TpchQ16PartInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT p_partkey, p_brand, p_type, p_size FROM part",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedText(cols, 1),
                  TypedText(cols, 2),
                  static_cast<int32_t>(TypedInt(cols, 3))});
      });
}

void ScanTpchQ16Partsupp(
    const std::function<void(const query::TpchQ16PartsuppInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT ps_partkey, ps_suppkey FROM partsupp",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedInt(cols, 1)});
      });
}

void ScanTpchQ19Parts(
    const std::function<void(const query::TpchQ19PartInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT p_partkey, p_brand, p_container, p_size FROM part",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedText(cols, 1),
                  TypedText(cols, 2),
                  static_cast<int32_t>(TypedInt(cols, 3))});
      });
}

void ScanTpchQ19Lineitems(
    const std::function<void(const query::TpchQ19LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_partkey, l_quantity, l_extendedprice, l_discount, l_shipmode, "
      "l_shipinstruct FROM lineitem",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedDecimalRaw(cols, 1),
                  TypedDecimalRaw(cols, 2),
                  TypedDecimalRaw(cols, 3),
                  TypedText(cols, 4),
                  TypedText(cols, 5)});
      });
}

bool LookupTpchQ19Part(int64_t partkey, query::TpchQ19PartInput &out) {
  bool found = false;
  const std::string sql =
      "SELECT p_brand, p_container, p_size FROM part WHERE p_partkey = " +
      std::to_string(partkey);
  ScanTypedTuples(sql, [&](const std::vector<std::string> &cols) {
    out = {partkey, TypedText(cols, 0), TypedText(cols, 1),
           static_cast<int32_t>(TypedInt(cols, 2))};
    found = true;
  });
  return found;
}

void ScanTpchQ20Lineitems(
    const std::function<void(const query::TpchQ20LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_shipdate, l_partkey, l_suppkey, l_quantity FROM lineitem",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedDate(cols, 0),
                  TypedInt(cols, 1),
                  TypedInt(cols, 2),
                  TypedDecimalRaw(cols, 3)});
      });
}

void ScanTpchQ20Parts(
    const std::function<void(const query::TpchQ20PartInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT p_partkey, p_name FROM part",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedText(cols, 1)});
      });
}

void ScanTpchQ20Partsupps(
    const std::function<void(const query::TpchQ20PartsuppInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT ps_partkey, ps_suppkey, ps_availqty FROM partsupp",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedInt(cols, 1),
                  TypedInt(cols, 2)});
      });
}

void ScanTpchQ20Suppliers(
    const std::function<void(const query::TpchQ20SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_name, s_address, s_nationkey FROM supplier",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedText(cols, 1),
                  TypedText(cols, 2),
                  static_cast<int32_t>(TypedInt(cols, 3))});
      });
}

void ScanTpchQ20Nations(
    const std::function<void(const query::TpchQ20NationInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT n_nationkey, n_name FROM nation",
      [&](const std::vector<std::string> &cols) {
        consumer({static_cast<int32_t>(TypedInt(cols, 0)),
                  TypedText(cols, 1)});
      });
}

} // namespace scans

class TpchQ1Scan final : public query::TpchQ1InitialScan {
public:
  void ScanLineitems(
      const std::function<void(const query::TpchQ1LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ1Lineitems(consumer);
  }
};

class TpchQ3Scan final : public query::TpchQ3InitialScan {
public:
  void ScanCustomers(
      const std::function<void(const query::TpchQ3CustomerInput &)> &consumer)
      override {
    scans::ScanTpchQ3Customers(consumer);
  }
  void ScanOrders(
      const std::function<void(const query::TpchQ3OrderInput &)> &consumer)
      override {
    scans::ScanTpchQ3Orders(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ3LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ3Lineitems(consumer);
  }
};

class TpchQ6Scan final : public query::TpchQ6InitialScan {
public:
  void ScanLineitems(
      const std::function<void(const query::TpchQ6LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ6Lineitems(consumer);
  }
};

class TpchQ10Scan final : public query::TpchQ10InitialScan {
public:
  void ScanCustomers(
      const std::function<void(const query::TpchQ10CustomerInput &)> &consumer)
      override {
    scans::ScanTpchQ10Customers(consumer);
  }
  void ScanNations(
      const std::function<void(const query::TpchQ10NationInput &)> &consumer)
      override {
    scans::ScanTpchQ10Nations(consumer);
  }
  void ScanOrders(
      const std::function<void(const query::TpchQ10OrderInput &)> &consumer)
      override {
    scans::ScanTpchQ10Orders(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ10LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ10Lineitems(consumer);
  }
  bool LookupCustomers(
      const std::vector<int64_t> &custkeys,
      std::unordered_map<int64_t, query::TpchQ10Customer> &out) override {
    scans::LookupTpchQ10Customers(custkeys, out);
    return true;
  }
};

class TpchQ12Scan final : public query::TpchQ12InitialScan {
public:
  void ScanOrders(
      const std::function<void(const query::TpchQ12OrderInput &)> &consumer)
      override {
    scans::ScanTpchQ12Orders(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ12LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ12Lineitems(consumer);
  }
};

class TpchQ14Scan final : public query::TpchQ14InitialScan {
public:
  void ScanParts(
      const std::function<void(const query::TpchQ14PartInput &)> &consumer)
      override {
    scans::ScanTpchQ14Parts(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ14LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ14Lineitems(consumer);
  }
};

class TpchQ9Scan final : public query::TpchQ9InitialScan {
public:
  void ScanParts(
      const std::function<void(const query::TpchQ9PartInput &)> &consumer)
      override {
    scans::ScanTpchQ9Parts(consumer);
  }
  void ScanSuppliers(
      const std::function<void(const query::TpchQ9SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ9Suppliers(consumer);
  }
  void ScanNations(
      const std::function<void(const query::TpchQ9NationInput &)> &consumer)
      override {
    scans::ScanTpchQ9Nations(consumer);
  }
  void ScanOrders(
      const std::function<void(const query::TpchQ9OrdersInput &)> &consumer)
      override {
    scans::ScanTpchQ9Orders(consumer);
  }
  void ScanPartsupps(
      const std::function<void(const query::TpchQ9PartsuppInput &)> &consumer)
      override {
    scans::ScanTpchQ9Partsupps(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ9LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ9Lineitems(consumer);
  }
};

class TpchQ11Scan final : public query::TpchQ11InitialScan {
public:
  void ScanPartsupp(
      const std::function<void(const query::TpchQ11PartsuppInput &)> &consumer)
      override {
    scans::ScanTpchQ11Partsupp(consumer);
  }
  void ScanSuppliers(
      const std::function<void(const query::TpchQ11SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ11Suppliers(consumer);
  }
  void ScanNations(
      const std::function<void(const query::TpchQ11NationInput &)> &consumer)
      override {
    scans::ScanTpchQ11Nations(consumer);
  }
};

class TpchQ17Scan final : public query::TpchQ17InitialScan {
public:
  void ScanParts(
      const std::function<void(const query::TpchQ17PartInput &)> &consumer)
      override {
    scans::ScanTpchQ17Parts(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ17LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ17Lineitems(consumer);
  }
  bool ScanPartQuantityAggregatesForPart(
      int64_t partkey,
      const std::function<void(const query::TpchQ17PartQuantityAggregate &)>
          &consumer) override {
    scans::ScanTpchQ17PartQuantityAggregatesForPart(partkey, consumer);
    return true;
  }
};

class TpchQ18Scan final : public query::TpchQ18InitialScan {
public:
  void ScanLineitems(
      const std::function<void(const query::TpchQ18LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ18Lineitems(consumer);
  }
  void ScanOrders(
      const std::function<void(const query::TpchQ18OrderInput &)> &consumer)
      override {
    scans::ScanTpchQ18Orders(consumer);
  }
  void ScanCustomers(
      const std::function<void(const query::TpchQ18CustomerInput &)> &consumer)
      override {
    scans::ScanTpchQ18Customers(consumer);
  }
  bool LookupOrder(int64_t orderkey,
                   query::TpchQ18OrderInput &out) override {
    // SPI is unsafe inside PostgreSQL xact callbacks (PreCommit maintenance).
    // Returning false makes StageMaintenance fall back to RebuildAll.
    (void)orderkey;
    (void)out;
    return false;
  }
};

class TpchQ21Scan final : public query::TpchQ21InitialScan {
public:
  void ScanSuppliers(
      const std::function<void(const query::TpchQ21SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ21Suppliers(consumer);
  }
  void ScanNations(
      const std::function<void(const query::TpchQ21NationInput &)> &consumer)
      override {
    scans::ScanTpchQ21Nations(consumer);
  }
  void ScanOrders(
      const std::function<void(const query::TpchQ21OrderInput &)> &consumer)
      override {
    scans::ScanTpchQ21Orders(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ21LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ21Lineitems(consumer);
  }
};

#if defined(ACCEL_HAVE_Q21_VERUS)
// Scans for the Verus Q21 kernel.  Each implements the SQL its interface
// comment names (the kernel's trusted inputs).  They run on a separate CLI
// connection, so they see committed data only: the capture contract then
// holds for autocommit writes (maintenance runs after each statement).
std::string Q21VerusHex(const std::string &s) {
  static const char *kHex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    out.push_back(kHex[c >> 4]);
    out.push_back(kHex[c & 15]);
  }
  return out;
}

class TpchQ21VerusScanImpl final : public query::TpchQ21VerusScan {
public:
  static q21v_li_row Row(const std::vector<std::string> &cols) {
    return q21v_li_row{TypedInt(cols, 0), TypedInt(cols, 1), TypedDate(cols, 2),
                       TypedDate(cols, 3)};
  }
  void ScanSuppliers(
      const std::function<void(int64_t, const std::string &, int32_t)> &consumer)
      override {
    ScanTypedTuples("SELECT s_suppkey, s_name, s_nationkey FROM supplier",
                    [&](const std::vector<std::string> &cols) {
                      consumer(TypedInt(cols, 0), TypedText(cols, 1),
                               static_cast<int32_t>(TypedInt(cols, 2)));
                    });
  }
  void ScanNations(
      const std::function<void(int32_t, const std::string &)> &consumer) override {
    ScanTypedTuples("SELECT n_nationkey, n_name FROM nation",
                    [&](const std::vector<std::string> &cols) {
                      consumer(static_cast<int32_t>(TypedInt(cols, 0)),
                               TypedText(cols, 1));
                    });
  }
  void ScanFOrderKeys(const std::function<void(int64_t)> &consumer) override {
    ScanTypedTuples("SELECT o_orderkey FROM orders WHERE o_orderstatus = 'F'",
                    [&](const std::vector<std::string> &cols) {
                      consumer(TypedInt(cols, 0));
                    });
  }
  void ScanFLineitems(const std::function<void(const q21v_li_row &)> &consumer) override {
    ScanTypedTuples("SELECT l.l_orderkey, l.l_suppkey, l.l_commitdate, l.l_receiptdate "
                    "FROM lineitem l WHERE EXISTS (SELECT 1 FROM orders o "
                    "WHERE o.o_orderkey = l.l_orderkey AND o.o_orderstatus = 'F')",
                    [&](const std::vector<std::string> &cols) { consumer(Row(cols)); });
  }
  void ScanLineitemsForKeys(const std::vector<int64_t> &keys,
                            const std::function<void(const q21v_li_row &)> &consumer)
      override {
    std::string sql = "SELECT l_orderkey, l_suppkey, l_commitdate, l_receiptdate "
                      "FROM lineitem WHERE l_orderkey IN (";
    for (std::size_t i = 0; i < keys.size(); ++i) {
      if (i)
        sql += ',';
      sql += std::to_string(keys[i]);
    }
    sql += ")";
    ScanTypedTuples(sql, [&](const std::vector<std::string> &cols) { consumer(Row(cols)); });
  }
  void RunReference(const std::string &nation,
                    const std::function<void(const std::string &, int64_t)> &consumer)
      override {
    const std::string sql =
        "SELECT s_name, COUNT(*) AS numwait FROM supplier, lineitem l1, orders, "
        "nation WHERE s_suppkey = l1.l_suppkey AND o_orderkey = l1.l_orderkey AND "
        "o_orderstatus = 'F' AND l1.l_receiptdate > l1.l_commitdate AND EXISTS ("
        "SELECT * FROM lineitem l2 WHERE l2.l_orderkey = l1.l_orderkey AND "
        "l2.l_suppkey <> l1.l_suppkey) AND NOT EXISTS (SELECT * FROM lineitem l3 "
        "WHERE l3.l_orderkey = l1.l_orderkey AND l3.l_suppkey <> l1.l_suppkey AND "
        "l3.l_receiptdate > l3.l_commitdate) AND s_nationkey = n_nationkey AND "
        "n_name = CONVERT(X'" + Q21VerusHex(nation) + "' USING utf8mb4) "
        "GROUP BY s_name ORDER BY numwait DESC, s_name";
    ScanTypedTuples(sql, [&](const std::vector<std::string> &cols) {
      consumer(TypedText(cols, 0), TypedInt(cols, 1));
    });
  }
};

// The kernel's reference specification orders and compares text byte-wise.
// MySQL does exactly that only under utf8mb4_0900_bin (NO PAD, memcmp), so
// the kernel is used only when the three text columns Q21 reads have it.
bool Q21VerusBinaryCollation() {
  int64_t n = 0;
  ScanTypedTuples(
      "SELECT COUNT(*) FROM information_schema.columns WHERE table_schema = DATABASE() "
      "AND collation_name = 'utf8mb4_0900_bin' AND ((table_name = 'supplier' AND "
      "column_name = 's_name') OR (table_name = 'nation' AND column_name = 'n_name') "
      "OR (table_name = 'orders' AND column_name = 'o_orderstatus'))",
      [&](const std::vector<std::string> &cols) { n = TypedInt(cols, 0); });
  return n == 3;
}
#endif

class TpchQ22Scan final : public query::TpchQ22InitialScan {
public:
  void ScanCustomers(
      const std::function<void(const query::TpchQ22CustomerInput &)> &consumer)
      override {
    scans::ScanTpchQ22Customers(consumer);
  }
  void ScanOrders(
      const std::function<void(const query::TpchQ22OrderInput &)> &consumer)
      override {
    scans::ScanTpchQ22Orders(consumer);
  }
};

class TpchQ2Scan final : public query::TpchQ2InitialScan {
public:
  void ScanParts(
      const std::function<void(const query::TpchQ2PartInput &)> &consumer)
      override {
    scans::ScanTpchQ2Parts(consumer);
  }
  void ScanSuppliers(
      const std::function<void(const query::TpchQ2SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ2Suppliers(consumer);
  }
  void ScanPartsupps(
      const std::function<void(const query::TpchQ2PartsuppInput &)> &consumer)
      override {
    scans::ScanTpchQ2Partsupps(consumer);
  }
  void ScanNations(
      const std::function<void(const query::TpchQ2NationInput &)> &consumer)
      override {
    scans::ScanTpchQ2Nations(consumer);
  }
  void ScanRegions(
      const std::function<void(const query::TpchQ2RegionInput &)> &consumer)
      override {
    scans::ScanTpchQ2Regions(consumer);
  }
};

class TpchQ4Scan final : public query::TpchQ4InitialScan {
public:
  void ScanOrders(
      const std::function<void(const query::TpchQ4OrderInput &)> &consumer)
      override {
    scans::ScanTpchQ4Orders(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ4LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ4Lineitems(consumer);
  }
};

class TpchQ5Scan final : public query::TpchQ5InitialScan {
public:
  void ScanRegions(
      const std::function<void(const query::TpchQ5RegionInput &)> &consumer)
      override {
    scans::ScanTpchQ5Regions(consumer);
  }
  void ScanNations(
      const std::function<void(const query::TpchQ5NationInput &)> &consumer)
      override {
    scans::ScanTpchQ5Nations(consumer);
  }
  void ScanCustomers(
      const std::function<void(const query::TpchQ5CustomerInput &)> &consumer)
      override {
    scans::ScanTpchQ5Customers(consumer);
  }
  void ScanSuppliers(
      const std::function<void(const query::TpchQ5SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ5Suppliers(consumer);
  }
  void ScanOrders(
      const std::function<void(const query::TpchQ5OrderInput &)> &consumer)
      override {
    scans::ScanTpchQ5Orders(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ5LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ5Lineitems(consumer);
  }
};

class TpchQ7Scan final : public query::TpchQ7InitialScan {
public:
  void ScanNations(
      const std::function<void(const query::TpchQ7NationInput &)> &consumer)
      override {
    scans::ScanTpchQ7Nations(consumer);
  }
  void ScanSuppliers(
      const std::function<void(const query::TpchQ7SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ7Suppliers(consumer);
  }
  void ScanCustomers(
      const std::function<void(const query::TpchQ7CustomerInput &)> &consumer)
      override {
    scans::ScanTpchQ7Customers(consumer);
  }
  void ScanOrders(
      const std::function<void(const query::TpchQ7OrderInput &)> &consumer)
      override {
    scans::ScanTpchQ7Orders(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ7LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ7Lineitems(consumer);
  }
};

class TpchQ8Scan final : public query::TpchQ8InitialScan {
public:
  void ScanParts(
      const std::function<void(const query::TpchQ8PartInput &)> &consumer)
      override {
    scans::ScanTpchQ8Parts(consumer);
  }
  void ScanSuppliers(
      const std::function<void(const query::TpchQ8SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ8Suppliers(consumer);
  }
  void ScanCustomers(
      const std::function<void(const query::TpchQ8CustomerInput &)> &consumer)
      override {
    scans::ScanTpchQ8Customers(consumer);
  }
  void ScanOrders(
      const std::function<void(const query::TpchQ8OrdersInput &)> &consumer)
      override {
    scans::ScanTpchQ8Orders(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ8LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ8Lineitems(consumer);
  }
  void ScanNations(
      const std::function<void(const query::TpchQ8NationInput &)> &consumer)
      override {
    scans::ScanTpchQ8Nations(consumer);
  }
  void ScanRegions(
      const std::function<void(const query::TpchQ8RegionInput &)> &consumer)
      override {
    scans::ScanTpchQ8Regions(consumer);
  }
};

class TpchQ13Scan final : public query::TpchQ13InitialScan {
public:
  void ScanCustomers(
      const std::function<void(const query::TpchQ13CustomerInput &)> &consumer)
      override {
    scans::ScanTpchQ13Customers(consumer);
  }
  void ScanOrders(
      const std::function<void(const query::TpchQ13OrderInput &)> &consumer)
      override {
    scans::ScanTpchQ13Orders(consumer);
  }
};

class TpchQ15Scan final : public query::TpchQ15InitialScan {
public:
  void ScanLineitems(
      const std::function<void(const query::TpchQ15LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ15Lineitems(consumer);
  }
  void ScanLineitemsForShipdateRange(
      int32_t start_days, int32_t end_days,
      const std::function<void(const query::TpchQ15LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ15LineitemsForShipdateRange(start_days, end_days, consumer);
  }
  void ScanSuppliers(
      const std::function<void(const query::TpchQ15SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ15Suppliers(consumer);
  }
};

class TpchQ16Scan final : public query::TpchQ16InitialScan {
public:
  void ScanSuppliers(
      const std::function<void(const query::TpchQ16SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ16Suppliers(consumer);
  }
  void ScanParts(
      const std::function<void(const query::TpchQ16PartInput &)> &consumer)
      override {
    scans::ScanTpchQ16Parts(consumer);
  }
  void ScanPartsupp(
      const std::function<void(const query::TpchQ16PartsuppInput &)> &consumer)
      override {
    scans::ScanTpchQ16Partsupp(consumer);
  }
};

class TpchQ19Scan final : public query::TpchQ19InitialScan {
public:
  void ScanParts(
      const std::function<void(const query::TpchQ19PartInput &)> &consumer)
      override {
    scans::ScanTpchQ19Parts(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ19LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ19Lineitems(consumer);
  }
  bool LookupPart(int64_t partkey, query::TpchQ19PartInput &out) override {
    return scans::LookupTpchQ19Part(partkey, out);
  }
};

class TpchQ20Scan final : public query::TpchQ20InitialScan {
public:
  void ScanLineitems(
      const std::function<void(const query::TpchQ20LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ20Lineitems(consumer);
  }
  void ScanParts(
      const std::function<void(const query::TpchQ20PartInput &)> &consumer)
      override {
    scans::ScanTpchQ20Parts(consumer);
  }
  void ScanPartsupps(
      const std::function<void(const query::TpchQ20PartsuppInput &)> &consumer)
      override {
    scans::ScanTpchQ20Partsupps(consumer);
  }
  void ScanSuppliers(
      const std::function<void(const query::TpchQ20SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ20Suppliers(consumer);
  }
  void ScanNations(
      const std::function<void(const query::TpchQ20NationInput &)> &consumer)
      override {
    scans::ScanTpchQ20Nations(consumer);
  }
};

class Builder final : public core::StateBuilder {
public:
  void Write(core::StateImage image) override {
    images.push_back(std::move(image));
  }
  void Finish() override { finished = true; }
  std::vector<core::StateImage> images;
  bool finished = false;
};

class TsvSink final : public core::AuxRowSink {
public:
  explicit TsvSink(std::string &out_p) : out(out_p) {}

  void Begin(const core::OutputSchema &) override {}

  void WriteRow(const core::AuxRow &row) override {
    for (std::size_t column = 0; column < row.values.size(); ++column) {
      if (column > 0) {
        out.push_back('|');
      }
      if (!row.values[column].is_null) {
        out += CellText(row.values[column]);
      }
    }
    out.push_back('\n');
  }

  void End() override {}

private:
  static std::string DecimalText(const core::ScaledDecimal &decimal) {
    const bool negative = decimal.value < 0;
    uint64_t magnitude = negative
                            ? static_cast<uint64_t>(-(decimal.value + 1)) + 1
                            : static_cast<uint64_t>(decimal.value);
    std::string digits = std::to_string(magnitude);
    if (decimal.scale > 0) {
      if (digits.size() <= static_cast<std::size_t>(decimal.scale)) {
        digits.insert(0, static_cast<std::size_t>(decimal.scale) + 1 -
                               digits.size(),
                     '0');
      }
      digits.insert(digits.size() - decimal.scale, 1, '.');
    }
    if (negative) {
      digits.insert(digits.begin(), '-');
    }
    return digits;
  }

  static std::string FormatUnixDate(int32_t days) {
    int z = days + 719468;
    const int era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = static_cast<int>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    y += (m <= 2);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u", y, m, d);
    return buf;
  }

  static std::string CellText(const core::AuxValue &value) {
    if (std::holds_alternative<int64_t>(value.payload)) {
      return std::to_string(std::get<int64_t>(value.payload));
    }
    if (std::holds_alternative<double>(value.payload)) {
      std::ostringstream ss;
      ss.setf(std::ios::fixed);
      ss.precision(6);
      ss << std::get<double>(value.payload);
      return ss.str();
    }
    if (std::holds_alternative<core::ScaledDecimal>(value.payload)) {
      return DecimalText(std::get<core::ScaledDecimal>(value.payload));
    }
    if (std::holds_alternative<core::AuxDate>(value.payload)) {
      return FormatUnixDate(std::get<core::AuxDate>(value.payload).days_since_epoch);
    }
    if (std::holds_alternative<std::string>(value.payload)) {
      return std::get<std::string>(value.payload);
    }
    if (std::holds_alternative<bool>(value.payload)) {
      return std::get<bool>(value.payload) ? "t" : "f";
    }
    return "";
  }

  std::string &out;
};

template <typename Module, typename Config, typename ScanT>
class Runtime final {
public:
  Runtime(const char *cost_model_env, const char *default_config_path,
          core::AcceleratorId accelerator, Config config)
      : store(), coordinator(),
        backend(
            coordinator, store,
            [] { return core::VisibilityToken{}; },
            [cost_model_env, default_config_path] {
              return core::LoadAuxCostModelJson(
                  ResolveCostModelPath(cost_model_env, default_config_path));
            }),
        scan(), module(config, &scan), accelerator(std::move(accelerator)) {
    auto module_view = std::shared_ptr<Module>(&module, [](Module *) {});
    coordinator.Register(std::move(module_view));
    accelerator_codegen::aux_state::engine::mysql::
        RegisterMysqlMutationBackend(&backend);
    accelerator_codegen::aux_state::engine::mysql::
        RegisterMysqlXactCallbacks();
  }

  void EnsureBuilt() {
    if (built) {
      return;
    }
    Builder builder;
    module.Build(backend, builder);
    if (!builder.finished) {
      throw std::runtime_error("auxiliary build did not finish");
    }
    store.Install(std::move(builder.images), accelerator);
    store.ClearTransactionDirty();
    built = true;
    accelerator_codegen::aux_state::engine::mysql::
        RegisterMysqlBackendForMutations();
  }

  void Execute(const core::QueryArguments &arguments, std::string &out) {
    // Prefer this Runtime for subsequent trigger captures in this session.
    accelerator_codegen::aux_state::engine::mysql::
        RegisterMysqlMutationBackend(&backend);

    // If audit-driven commit was skipped (another accelerator threw), apply
    // this module's pending mutations now — after the DML statement has
    // become visible to a scan connection.
    if (coordinator.HasPendingWork()) {
      backend.OnPreCommit();
      backend.OnCommit();
      accelerator_codegen::aux_state::engine::mysql::
          NoteMysqlBackendLastAction(&backend);
    }

    EnsureBuilt();
    // Mutations are applied on UPDATE commit via CaptureMutation →
    // StageMaintenance (ApplyDelta / Rebuild*) → TryPublish. Empty QueryKey
    // means partition 0 in MysqlStateStore (layer contract), so use Read()
    // to detect a fully invalidated image and rebuild after commit.
    if (store.NeedsRebuild()) {
      built = false;
      EnsureBuilt();
    }
    observed_mutations = mysql_accel_get_observer_mutation_count();
    TsvSink sink(out);
    module.Execute(backend, arguments, sink);
  }

private:
  MysqlStateStore store;
  core::AuxTransactionCoordinator coordinator;
  accelerator_codegen::aux_state::engine::mysql::MysqlAuxBackend backend;
  ScanT scan;
  Module module;
  std::string accelerator;
  bool built = false;
  int observed_mutations = 0;
};


namespace mysql_tbl = accelerator_codegen::aux_state::engine::mysql::tpch_tables;

Runtime<query::TpchQ1Module, query::TpchQ1Config, TpchQ1Scan> &Q1Runtime() {
  static Runtime<query::TpchQ1Module, query::TpchQ1Config, TpchQ1Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q1_COST_MODEL",
              "config/q1_aux_cost_model.json",
              core::TpchQ1Layout::kAccelerator, query::TpchQ1Config{mysql_tbl::kLineitem});
  return runtime;
}

Runtime<query::TpchQ3Module, query::TpchQ3Config, TpchQ3Scan> &Q3Runtime() {
  static Runtime<query::TpchQ3Module, query::TpchQ3Config, TpchQ3Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q3_COST_MODEL",
              "config/q3_aux_cost_model.json",
              core::TpchQ3Layout::kAccelerator,
              query::TpchQ3Config{mysql_tbl::kCustomer, mysql_tbl::kOrders, mysql_tbl::kLineitem});
  return runtime;
}

Runtime<query::TpchQ6Module, query::TpchQ6Config, TpchQ6Scan> &Q6Runtime() {
  static Runtime<query::TpchQ6Module, query::TpchQ6Config, TpchQ6Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q6_COST_MODEL",
              "config/q6_aux_cost_model.json",
              core::TpchQ6Layout::kAccelerator, query::TpchQ6Config{mysql_tbl::kLineitem});
  return runtime;
}

Runtime<query::TpchQ10Module, query::TpchQ10Config, TpchQ10Scan> &
Q10Runtime() {
  static Runtime<query::TpchQ10Module, query::TpchQ10Config, TpchQ10Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q10_COST_MODEL",
              "config/q10_aux_cost_model.json",
              core::TpchQ10Layout::kAccelerator,
              query::TpchQ10Config{mysql_tbl::kCustomer, mysql_tbl::kNation, mysql_tbl::kOrders, mysql_tbl::kLineitem});
  return runtime;
}

Runtime<query::TpchQ12Module, query::TpchQ12Config, TpchQ12Scan> &
Q12Runtime() {
  static Runtime<query::TpchQ12Module, query::TpchQ12Config, TpchQ12Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q12_COST_MODEL",
              "config/q12_aux_cost_model.json",
              core::TpchQ12Layout::kAccelerator, query::TpchQ12Config{mysql_tbl::kOrders, mysql_tbl::kLineitem});
  return runtime;
}

Runtime<query::TpchQ14Module, query::TpchQ14Config, TpchQ14Scan> &
Q14Runtime() {
  static Runtime<query::TpchQ14Module, query::TpchQ14Config, TpchQ14Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q14_COST_MODEL",
              "config/q14_aux_cost_model.json",
              core::TpchQ14Layout::kAccelerator, query::TpchQ14Config{mysql_tbl::kPart, mysql_tbl::kLineitem});
  return runtime;
}

Runtime<query::TpchQ9Module, query::TpchQ9Config, TpchQ9Scan> &Q9Runtime() {
  static Runtime<query::TpchQ9Module, query::TpchQ9Config, TpchQ9Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q9_COST_MODEL",
              "config/q9_aux_cost_model.json",
              query::TpchQ9Layout::kAccelerator,
              query::TpchQ9Config{mysql_tbl::kPart, mysql_tbl::kSupplier, mysql_tbl::kNation, mysql_tbl::kOrders, mysql_tbl::kPartsupp, mysql_tbl::kLineitem});
  return runtime;
}

Runtime<query::TpchQ11Module, query::TpchQ11Config, TpchQ11Scan> &
Q11Runtime() {
  static Runtime<query::TpchQ11Module, query::TpchQ11Config, TpchQ11Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q11_COST_MODEL",
              "config/q11_aux_cost_model.json",
              query::TpchQ11Layout::kAccelerator,
              query::TpchQ11Config{mysql_tbl::kPartsupp, mysql_tbl::kSupplier, mysql_tbl::kNation});
  return runtime;
}

Runtime<query::TpchQ17Module, query::TpchQ17Config, TpchQ17Scan> &
Q17Runtime() {
  static Runtime<query::TpchQ17Module, query::TpchQ17Config, TpchQ17Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q17_COST_MODEL",
              "config/q17_aux_cost_model.json",
              query::TpchQ17Layout::kAccelerator,
              query::TpchQ17Config{mysql_tbl::kPart, mysql_tbl::kLineitem});
  return runtime;
}

Runtime<query::TpchQ18Module, query::TpchQ18Config, TpchQ18Scan> &
Q18Runtime() {
  static Runtime<query::TpchQ18Module, query::TpchQ18Config, TpchQ18Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q18_COST_MODEL",
              "config/q18_aux_cost_model.json",
              query::TpchQ18Layout::kAccelerator,
              query::TpchQ18Config{mysql_tbl::kLineitem, mysql_tbl::kOrders, mysql_tbl::kCustomer});
  return runtime;
}

Runtime<query::TpchQ21Module, query::TpchQ21Config, TpchQ21Scan> &
Q21Runtime() {
  static Runtime<query::TpchQ21Module, query::TpchQ21Config, TpchQ21Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q21_COST_MODEL",
              "config/q21_aux_cost_model.json",
              query::TpchQ21Layout::kAccelerator,
              query::TpchQ21Config{mysql_tbl::kSupplier, mysql_tbl::kOrders, mysql_tbl::kLineitem, mysql_tbl::kNation});
  return runtime;
}

#if defined(ACCEL_HAVE_Q21_VERUS)
Runtime<query::TpchQ21VerusModule, query::TpchQ21VerusConfig, TpchQ21VerusScanImpl> &
Q21VerusRuntime() {
  static Runtime<query::TpchQ21VerusModule, query::TpchQ21VerusConfig,
                 TpchQ21VerusScanImpl>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q21_COST_MODEL",
              "config/q21_aux_cost_model.json",
              query::TpchQ21VerusLayout::kAccelerator,
              query::TpchQ21VerusConfig{mysql_tbl::kSupplier, mysql_tbl::kOrders,
                                        mysql_tbl::kLineitem, mysql_tbl::kNation});
  return runtime;
}
#endif

Runtime<query::TpchQ22Module, query::TpchQ22Config, TpchQ22Scan> &
Q22Runtime() {
  static Runtime<query::TpchQ22Module, query::TpchQ22Config, TpchQ22Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q22_COST_MODEL",
              "config/q22_aux_cost_model.json",
              query::TpchQ22Layout::kAccelerator,
              query::TpchQ22Config{mysql_tbl::kOrders, mysql_tbl::kCustomer});
  return runtime;
}

Runtime<query::TpchQ2Module, query::TpchQ2Config, TpchQ2Scan> &Q2Runtime() {
  static Runtime<query::TpchQ2Module, query::TpchQ2Config, TpchQ2Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q2_COST_MODEL",
              "config/q2_aux_cost_model.json", core::TpchQ2Layout::kAccelerator,
              query::TpchQ2Config{mysql_tbl::kPart, mysql_tbl::kSupplier, mysql_tbl::kPartsupp, mysql_tbl::kNation, mysql_tbl::kRegion});
  return runtime;
}

Runtime<query::TpchQ4Module, query::TpchQ4Config, TpchQ4Scan> &Q4Runtime() {
  static Runtime<query::TpchQ4Module, query::TpchQ4Config, TpchQ4Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q4_COST_MODEL",
              "config/q4_aux_cost_model.json", query::TpchQ4Layout::kAccelerator,
              query::TpchQ4Config{mysql_tbl::kOrders, mysql_tbl::kLineitem});
  return runtime;
}

Runtime<query::TpchQ5Module, query::TpchQ5Config, TpchQ5Scan> &Q5Runtime() {
  static Runtime<query::TpchQ5Module, query::TpchQ5Config, TpchQ5Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q5_COST_MODEL",
              "config/q5_aux_cost_model.json", core::TpchQ5Layout::kAccelerator,
              query::TpchQ5Config{mysql_tbl::kRegion, mysql_tbl::kNation, mysql_tbl::kCustomer, mysql_tbl::kSupplier, mysql_tbl::kOrders, mysql_tbl::kLineitem});
  return runtime;
}

Runtime<query::TpchQ7Module, query::TpchQ7Config, TpchQ7Scan> &Q7Runtime() {
  static Runtime<query::TpchQ7Module, query::TpchQ7Config, TpchQ7Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q7_COST_MODEL",
              "config/q7_aux_cost_model.json", core::TpchQ7Layout::kAccelerator,
              query::TpchQ7Config{mysql_tbl::kNation, mysql_tbl::kSupplier, mysql_tbl::kCustomer, mysql_tbl::kOrders, mysql_tbl::kLineitem});
  return runtime;
}

Runtime<query::TpchQ8Module, query::TpchQ8Config, TpchQ8Scan> &Q8Runtime() {
  static Runtime<query::TpchQ8Module, query::TpchQ8Config, TpchQ8Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q8_COST_MODEL",
              "config/q8_aux_cost_model.json", query::TpchQ8Layout::kAccelerator,
              query::TpchQ8Config{mysql_tbl::kPart, mysql_tbl::kSupplier, mysql_tbl::kCustomer, mysql_tbl::kOrders, mysql_tbl::kLineitem});
  return runtime;
}

Runtime<query::TpchQ13Module, query::TpchQ13Config, TpchQ13Scan> &
Q13Runtime() {
  static Runtime<query::TpchQ13Module, query::TpchQ13Config, TpchQ13Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q13_COST_MODEL",
              "config/q13_aux_cost_model.json",
              core::TpchQ13Layout::kAccelerator, query::TpchQ13Config{mysql_tbl::kCustomer, mysql_tbl::kOrders});
  return runtime;
}

Runtime<query::TpchQ15Module, query::TpchQ15Config, TpchQ15Scan> &
Q15Runtime() {
  static Runtime<query::TpchQ15Module, query::TpchQ15Config, TpchQ15Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q15_COST_MODEL",
              "config/q15_aux_cost_model.json",
              query::TpchQ15Layout::kAccelerator,
              query::TpchQ15Config{mysql_tbl::kLineitem, mysql_tbl::kSupplier});
  return runtime;
}

Runtime<query::TpchQ16Module, query::TpchQ16Config, TpchQ16Scan> &
Q16Runtime() {
  static Runtime<query::TpchQ16Module, query::TpchQ16Config, TpchQ16Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q16_COST_MODEL",
              "config/q16_aux_cost_model.json",
              query::TpchQ16Layout::kAccelerator,
              query::TpchQ16Config{mysql_tbl::kPart, mysql_tbl::kPartsupp, mysql_tbl::kSupplier});
  return runtime;
}

Runtime<query::TpchQ19Module, query::TpchQ19Config, TpchQ19Scan> &
Q19Runtime() {
  static Runtime<query::TpchQ19Module, query::TpchQ19Config, TpchQ19Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q19_COST_MODEL",
              "config/q19_aux_cost_model.json",
              query::TpchQ19Layout::kAccelerator,
              query::TpchQ19Config{mysql_tbl::kLineitem, mysql_tbl::kPart});
  return runtime;
}

Runtime<query::TpchQ20Module, query::TpchQ20Config, TpchQ20Scan> &
Q20Runtime() {
  static Runtime<query::TpchQ20Module, query::TpchQ20Config, TpchQ20Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q20_COST_MODEL",
              "config/q20_aux_cost_model.json",
              query::TpchQ20Layout::kAccelerator,
              query::TpchQ20Config{mysql_tbl::kLineitem, mysql_tbl::kPart, mysql_tbl::kPartsupp, mysql_tbl::kSupplier, mysql_tbl::kNation});
  return runtime;
}

} // namespace detail

namespace core = accelerator_codegen::aux_state::core;


std::string ExecuteMysqlTpchQ1(const core::QueryArguments &arguments) {
  std::string out;
  detail::Q1Runtime().Execute(arguments, out);
  return out;
}

std::string ExecuteMysqlTpchQ2(const core::QueryArguments &arguments) {
  std::string out;
  detail::Q2Runtime().Execute(arguments, out);
  return out;
}

std::string ExecuteMysqlTpchQ3(const core::QueryArguments &arguments) {
  std::string out;
  detail::Q3Runtime().Execute(arguments, out);
  return out;
}

std::string ExecuteMysqlTpchQ4(const core::QueryArguments &arguments) {
  std::string out;
  detail::Q4Runtime().Execute(arguments, out);
  return out;
}

std::string ExecuteMysqlTpchQ5(const core::QueryArguments &arguments) {
  std::string out;
  detail::Q5Runtime().Execute(arguments, out);
  return out;
}

std::string ExecuteMysqlTpchQ6(const core::QueryArguments &arguments) {
  std::string out;
  detail::Q6Runtime().Execute(arguments, out);
  return out;
}

std::string ExecuteMysqlTpchQ7(const core::QueryArguments &arguments) {
  std::string out;
  detail::Q7Runtime().Execute(arguments, out);
  return out;
}

std::string ExecuteMysqlTpchQ8(const core::QueryArguments &arguments) {
  std::string out;
  detail::Q8Runtime().Execute(arguments, out);
  return out;
}

std::string ExecuteMysqlTpchQ9(const core::QueryArguments &arguments) {
  std::string out;
  detail::Q9Runtime().Execute(arguments, out);
  return out;
}

std::string ExecuteMysqlTpchQ10(const core::QueryArguments &arguments) {
  std::string out;
  detail::Q10Runtime().Execute(arguments, out);
  return out;
}

std::string ExecuteMysqlTpchQ11(const core::QueryArguments &arguments) {
  std::string out;
  detail::Q11Runtime().Execute(arguments, out);
  return out;
}

std::string ExecuteMysqlTpchQ12(const core::QueryArguments &arguments) {
  std::string out;
  detail::Q12Runtime().Execute(arguments, out);
  return out;
}

std::string ExecuteMysqlTpchQ13(const core::QueryArguments &arguments) {
  std::string out;
  detail::Q13Runtime().Execute(arguments, out);
  return out;
}

std::string ExecuteMysqlTpchQ14(const core::QueryArguments &arguments) {
  std::string out;
  detail::Q14Runtime().Execute(arguments, out);
  return out;
}

std::string ExecuteMysqlTpchQ15(const core::QueryArguments &arguments) {
  std::string out;
  detail::Q15Runtime().Execute(arguments, out);
  return out;
}

std::string ExecuteMysqlTpchQ16(const core::QueryArguments &arguments) {
  std::string out;
  detail::Q16Runtime().Execute(arguments, out);
  return out;
}

std::string ExecuteMysqlTpchQ17(const core::QueryArguments &arguments) {
  std::string out;
  detail::Q17Runtime().Execute(arguments, out);
  if (out.find_first_not_of(" \t\r\n") == std::string::npos) {
    out = "NULL\n";
  }
  return out;
}

std::string ExecuteMysqlTpchQ18(const core::QueryArguments &arguments) {
  std::string out;
  detail::Q18Runtime().Execute(arguments, out);
  return out;
}

std::string ExecuteMysqlTpchQ19(const core::QueryArguments &arguments) {
  std::string out;
  detail::Q19Runtime().Execute(arguments, out);
  return out;
}

std::string ExecuteMysqlTpchQ20(const core::QueryArguments &arguments) {
  std::string out;
  detail::Q20Runtime().Execute(arguments, out);
  return out;
}

std::string ExecuteMysqlTpchQ21(const core::QueryArguments &arguments) {
  std::string out;
  detail::Q21Runtime().Execute(arguments, out);
  return out;
}

#if defined(ACCEL_HAVE_Q21_VERUS)
std::string ExecuteMysqlTpchQ21Verus(const core::QueryArguments &arguments) {
  static const bool binary = detail::Q21VerusBinaryCollation();
  std::string out;
  if (!binary) {
    // Outside the kernel's verified domain: answer with the SQL itself.
    std::string nation;
    if (!arguments.values.empty() && !arguments.values[0].is_null &&
        std::holds_alternative<std::string>(arguments.values[0].payload))
      nation = std::get<std::string>(arguments.values[0].payload);
    detail::TsvSink sink(out);
    detail::TpchQ21VerusScanImpl scan;
    core::AuxRow row;
    scan.RunReference(nation, [&](const std::string &name, int64_t numwait) {
      row.values = {core::AuxValue::Of(core::AuxType::String, name),
                    core::AuxValue::Of(core::AuxType::Int64, numwait)};
      sink.WriteRow(row);
    });
    return out;
  }
  detail::Q21VerusRuntime().Execute(arguments, out);
  return out;
}
#endif

std::string ExecuteMysqlTpchQ22(const core::QueryArguments &arguments) {
  std::string out;
  detail::Q22Runtime().Execute(arguments, out);
  return out;
}

} // namespace accelerator_codegen::aux_state::engine::mysql

#endif
