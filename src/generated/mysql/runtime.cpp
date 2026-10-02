// MySQL engine backend: engine-level scan/parse helpers and the UDF registry.
// Names no query; see runtime.hpp.
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "mysql/runtime.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <stdexcept>

namespace accelerator_codegen::aux_state::engine::mysql {

namespace {
// Function-local static storage; std::map keeps entries address-stable.
std::map<std::string, MysqlUdfEntry> &UdfRegistry() {
  static std::map<std::string, MysqlUdfEntry> registry;
  return registry;
}
std::mutex &UdfRegistryMutex() {
  static std::mutex mu;
  return mu;
}
} // namespace

void RegisterMysqlUdf(MysqlUdfEntry entry) {
  std::lock_guard<std::mutex> lock(UdfRegistryMutex());
  const std::string name = entry.name;
  UdfRegistry()[name] = std::move(entry);
}

const MysqlUdfEntry *FindMysqlUdf(const std::string &name) {
  std::lock_guard<std::mutex> lock(UdfRegistryMutex());
  auto it = UdfRegistry().find(name);
  return it == UdfRegistry().end() ? nullptr : &it->second;
}

std::vector<std::string> RegisteredMysqlUdfNames() {
  std::lock_guard<std::mutex> lock(UdfRegistryMutex());
  std::vector<std::string> names;
  for (const auto &kv : UdfRegistry()) names.push_back(kv.first);
  return names;
}

std::string ExecuteMysqlUdf(const std::string &name, const core::QueryArguments &arguments) {
  const MysqlUdfEntry *entry = FindMysqlUdf(name);
  if (!entry || !entry->execute) {
    throw std::runtime_error("no MySQL UDF registered: " + name);
  }
  return entry->execute(arguments);
}

namespace detail {

namespace core = accelerator_codegen::aux_state::core;

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

// Table scans this thread ran (a query that scans answered from SQL).
thread_local uint64_t t_table_scans = 0; // declared in runtime.hpp

void ScanTypedTuples(const std::string &sql,
                     const std::function<void(const std::vector<std::string> &)> &callback) {
  ++t_table_scans;
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

// A text value as an SQL literal that reaches the server byte for byte (no
// quoting or client re-encoding): CONVERT(X'<hex>' USING utf8mb4).
std::string ByteExactLiteral(const std::string &text) {
  static const char *kHex = "0123456789ABCDEF";
  std::string out = "CONVERT(X'";
  for (unsigned char c : text) {
    out.push_back(kHex[c >> 4]);
    out.push_back(kHex[c & 15]);
  }
  return out + "' USING utf8mb4)";
}

// `column IN (k1, k2, ...)`, or FALSE for no keys.
std::string KeyListPredicate(const char *column, const std::vector<int64_t> &keys) {
  if (keys.empty())
    return "FALSE";
  std::string sql = std::string(column) + " IN (";
  for (std::size_t i = 0; i < keys.size(); ++i) {
    if (i)
      sql += ',';
    sql += std::to_string(keys[i]);
  }
  return sql + ")";
}

// Whether every (table, column) of the current database compares text byte
// by byte: = is byte equality and ORDER BY byte order, as in a C collation.
// MySQL does that only under utf8mb4_0900_bin (NO PAD, memcmp); the default
// collations ignore case and accents, and *_bin ones ignore trailing spaces.
bool ColumnsCompareBytewise(const std::vector<std::pair<std::string, std::string>> &columns) {
  if (columns.empty())
    return true;
  // Plain string literals (coercible to information_schema's utf8mb3).
  auto quote = [](const std::string &name) {
    std::string out = "'";
    for (char c : name) {
      if (c == '\'' || c == '\\')
        out.push_back(c);
      out.push_back(c);
    }
    return out + "'";
  };
  std::string which;
  for (const auto &[table, column] : columns) {
    which += (which.empty() ? "" : " OR ");
    which += "(table_name = " + quote(table) + " AND column_name = " + quote(column) + ")";
  }
  int64_t n = 0;
  ScanTypedTuples("SELECT COUNT(*) FROM information_schema.columns WHERE "
                  "table_schema = DATABASE() AND collation_name = 'utf8mb4_0900_bin' AND (" +
                      which + ")",
                  [&](const std::vector<std::string> &cols) { n = TypedInt(cols, 0); });
  return n == static_cast<int64_t>(columns.size());
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

} // namespace detail
} // namespace accelerator_codegen::aux_state::engine::mysql

#endif
