// MySQL engine backend: engine-level scan/parse helpers and the UDF registry.
// Names no query; see runtime.hpp.
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "backends/mysql/api/runtime.hpp"

#include "backends/mysql/engine/mysql_exact_parse.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>

#if defined(ACCEL_MYSQL_HAVE_CLIENT)
#include <dlfcn.h>
#include <mysql.h> // libmysqlclient / MariaDB Connector/C: types only, see ClientApi
#include <type_traits>
#endif

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

std::string ResolveCostModelPath(const char *env_name,
                                 const core::AuxCostModelLocation &location) {
  if (const char *path = std::getenv(env_name); path && *path) {
    return path;
  }
  core::AuxCostModelLookup lookup;
  lookup.engine = "mysql";
  lookup.dirs_after = {std::string("/accel-src/") + location.dir};
  return core::ResolveAuxCostModelPath(location, lookup);
}

// ---- Exact value parsing -------------------------------------------------
// Scan rows arrive as text; every Typed* accessor parses it exactly
// (mysql_exact_parse.hpp) and throws std::runtime_error on text it cannot
// represent.  Nothing is rounded, saturated or defaulted.

namespace ex = accelerator_codegen::aux_state::engine::mysql::exact;

// AuxDate uses Unix-epoch days. MySQL DATE text is YYYY-MM-DD.
int32_t ParseUnixDays(const std::string &text) {
  int32_t days = 0;
  if (!ex::ParseDate(text.data(), text.size(), days)) {
    throw std::runtime_error("invalid DATE value '" + text.substr(0, 64) + "'");
  }
  return days;
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

// ---- Connection settings --------------------------------------------------
// The scan connection is made back to the server this plugin runs in. It is
// configured only through the server process's environment (nothing is
// compiled in): MYSQL_HOST (default 127.0.0.1), MYSQL_PORT / MYSQL_TCP_PORT
// (3306), MYSQL_USER (root), the password from MYSQL_PWD, else the image's
// MYSQL_ROOT_PASSWORD (else empty), the database from ACCEL_MYSQL_DB, else
// MYSQL_DATABASE (else none; mysql_accel_set_scan_database overrides it), and
// optionally MYSQL_UNIX_PORT (a socket path, preferred over TCP when set).
struct ConnSettings {
  std::string host = "127.0.0.1";
  unsigned port = 3306;
  std::string user = "root";
  std::string password;
  std::string db;
  std::string socket;
};

static const char *EnvNonEmpty(const char *name) {
  const char *v = std::getenv(name);
  return (v && *v) ? v : nullptr;
}

namespace {
std::atomic<int> g_scan_mode_override{-1}; // -1: environment, 0: client, 1: cli
std::mutex g_scan_db_mutex;
std::string g_scan_db_override;
} // namespace

void SetScanMode(const std::string &mode) {
  g_scan_mode_override = mode == "cli" ? 1 : mode == "client" ? 0 : -1;
}

void SetScanDatabase(const std::string &db) {
  std::lock_guard<std::mutex> lock(g_scan_db_mutex);
  g_scan_db_override = db;
}

ConnSettings ReadConnSettings() {
  ConnSettings c;
  if (auto v = EnvNonEmpty("MYSQL_HOST")) c.host = v;
  if (auto v = EnvNonEmpty("MYSQL_PORT")) c.port = static_cast<unsigned>(std::strtoul(v, nullptr, 10));
  else if (auto v2 = EnvNonEmpty("MYSQL_TCP_PORT")) c.port = static_cast<unsigned>(std::strtoul(v2, nullptr, 10));
  if (auto v = EnvNonEmpty("MYSQL_USER")) c.user = v;
  if (const char *v = std::getenv("MYSQL_PWD")) c.password = v;
  else if (auto v2 = EnvNonEmpty("MYSQL_ROOT_PASSWORD")) c.password = v2;
  if (auto v = EnvNonEmpty("ACCEL_MYSQL_DB")) c.db = v;
  else if (auto v2 = EnvNonEmpty("MYSQL_DATABASE")) c.db = v2;
  if (auto v = EnvNonEmpty("MYSQL_UNIX_PORT")) c.socket = v;
  {
    std::lock_guard<std::mutex> lock(g_scan_db_mutex);
    if (!g_scan_db_override.empty()) c.db = g_scan_db_override;
  }
  return c;
}

std::string MysqlCliBase() {
  const ConnSettings c = ReadConnSettings();
  std::ostringstream cmd;
  cmd << "MYSQL_PWD=" << ShellEscape(c.password) << " mysql -h " << ShellEscape(c.host)
      << " -P " << c.port << " -u" << ShellEscape(c.user);
  if (!c.db.empty()) cmd << " -D " << ShellEscape(c.db);
  return cmd.str();
}

std::string MysqlCliPrefix() {
  return MysqlCliBase() + " --quick --batch --skip-column-names -e ";
}

// Tab-separated line from `mysql --batch` (without --raw): fields are
// separated by tabs and \0 \n \t \\ are backslash-escaped, so a text value
// holding a tab or newline cannot break the row.  NULL prints as the bare
// word NULL (a text value "NULL" is indistinguishable; the client path has
// no such ambiguity).
std::vector<std::string> SplitTsv(const std::string &line) {
  std::vector<std::string> cols;
  std::string cur;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (c == '\t') {
      cols.push_back(std::move(cur));
      cur.clear();
    } else if (c == '\\' && i + 1 < line.size()) {
      const char n = line[++i];
      cur += n == 'n' ? '\n' : n == 't' ? '\t' : n == '0' ? '\0' : n;
    } else {
      cur += c;
    }
  }
  cols.push_back(std::move(cur));
  return cols;
}

// ---- Row access ------------------------------------------------------------
// The row passed to a scan callback is reused for the next row, and carries
// which cells are SQL NULL out of band (so a text value "NULL" stays text).
// Cells that are NULL also hold the text "NULL" for callers that only look at
// the strings.
namespace {
thread_local const std::vector<std::string> *t_row = nullptr;
thread_local const std::vector<char> *t_row_nulls = nullptr;
thread_local const core::AuxEngineServices *t_scan_services = nullptr;
} // namespace

RowScope::RowScope(const std::vector<std::string> *row, const std::vector<char> *nulls)
    : prev_row(t_row), prev_nulls(t_row_nulls) {
  t_row = row;
  t_row_nulls = nulls;
}

RowScope::~RowScope() {
  t_row = prev_row;
  t_row_nulls = prev_nulls;
}

ScanServicesScope::ScanServicesScope(const core::AuxEngineServices &services) : saved(t_scan_services) {
  t_scan_services = &services;
}

ScanServicesScope::~ScanServicesScope() { t_scan_services = saved; }

const core::AuxEngineServices *CurrentScanServices() { return t_scan_services; }

// Table scans this thread ran (a query that scans answered from SQL).
thread_local uint64_t t_table_scans = 0; // declared in runtime.hpp

std::chrono::microseconds ReadWait() {
  static const auto wait = std::chrono::microseconds([] {
    const char *value = std::getenv("ACCEL_AUX_READ_WAIT_US");
    return value && *value ? std::strtoll(value, nullptr, 10) : 600'000'000LL;
  }());
  return wait;
}

#if defined(ACCEL_MYSQL_HAVE_CLIENT)

namespace {

// The client library is loaded with dlopen(RTLD_LOCAL | RTLD_DEEPBIND) and
// called through function pointers, never linked: mysqld itself exports the
// libmysqlclient symbols (mysql_real_connect, ...), and a plugin that merely
// links a client library would have its calls bound to the server's own copy
// (a different struct layout and option numbering than the headers this file
// was compiled against). The library found at build time is the one loaded.
struct ClientApi {
  decltype(&::mysql_init) init = nullptr;
  decltype(&::mysql_options) options = nullptr;
  decltype(&::mysql_real_connect) real_connect = nullptr;
  decltype(&::mysql_real_query) real_query = nullptr;
  decltype(&::mysql_use_result) use_result = nullptr;
  decltype(&::mysql_fetch_row) fetch_row = nullptr;
  decltype(&::mysql_fetch_lengths) fetch_lengths = nullptr;
  decltype(&::mysql_num_fields) num_fields = nullptr;
  decltype(&::mysql_free_result) free_result = nullptr;
  decltype(&::mysql_close) close = nullptr;
  decltype(&::mysql_error) error = nullptr;
  decltype(&::mysql_errno) err_no = nullptr;
};

const ClientApi *LoadClientApi() {
  static const ClientApi *api = [] () -> const ClientApi * {
    const char *override_path = std::getenv("ACCEL_MYSQL_CLIENT_LIB");
    const char *candidates[] = {override_path && *override_path ? override_path : nullptr,
#if defined(ACCEL_MYSQL_CLIENT_LIB_PATH)
                                ACCEL_MYSQL_CLIENT_LIB_PATH,
#endif
                                nullptr};
    void *handle = nullptr;
    for (const char *path : candidates) {
      if (path) {
        handle = dlopen(path, RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
        if (handle) break;
      }
    }
    if (!handle) return nullptr;
    static ClientApi loaded;
    bool ok = true;
    auto sym = [&](auto &slot, const char *name) {
      slot = reinterpret_cast<std::remove_reference_t<decltype(slot)>>(dlsym(handle, name));
      ok = ok && slot != nullptr;
    };
    sym(loaded.init, "mysql_init");
    sym(loaded.options, "mysql_options");
    sym(loaded.real_connect, "mysql_real_connect");
    sym(loaded.real_query, "mysql_real_query");
    sym(loaded.use_result, "mysql_use_result");
    sym(loaded.fetch_row, "mysql_fetch_row");
    sym(loaded.fetch_lengths, "mysql_fetch_lengths");
    sym(loaded.num_fields, "mysql_num_fields");
    sym(loaded.free_result, "mysql_free_result");
    sym(loaded.close, "mysql_close");
    sym(loaded.error, "mysql_error");
    sym(loaded.err_no, "mysql_errno");
    return ok ? &loaded : nullptr;
  }();
  return api;
}

const ClientApi &Api() {
  const ClientApi *api = LoadClientApi();
  if (!api) throw std::runtime_error("MySQL client library is not loadable");
  return *api;
}

struct Conn {
  MYSQL *m = nullptr;
  Conn() = default;
  Conn(const Conn &) = delete;
  Conn &operator=(const Conn &) = delete;
  ~Conn() { Close(); }
  void Close() {
    if (m) {
      Api().close(m); // implicit rollback of any open transaction, no ROLLBACK
      m = nullptr;    // statement (the audit plugin treats that as a txn event)
    }
  }
};

void Connect(Conn &conn) {
  const ClientApi &api = Api();
  const ConnSettings c = ReadConnSettings();
  conn.m = api.init(nullptr);
  if (!conn.m) throw std::runtime_error("mysql_init failed");
  unsigned timeout = 3600;
  api.options(conn.m, MYSQL_OPT_READ_TIMEOUT, &timeout);
  api.options(conn.m, MYSQL_OPT_WRITE_TIMEOUT, &timeout);
  api.options(conn.m, MYSQL_SET_CHARSET_NAME, "utf8mb4");
#if defined(ACCEL_MYSQL_CLIENT_PLUGIN_DIR)
  // MariaDB Connector/C loads its auth plugins (caching_sha2_password, ...) from here.
  api.options(conn.m, MYSQL_PLUGIN_DIR, ACCEL_MYSQL_CLIENT_PLUGIN_DIR);
#endif
  // The scan connection goes back to this very server (loopback, or a Unix
  // socket when MYSQL_UNIX_PORT is set) with the credentials the CLI path used.
  const bool use_socket = !c.socket.empty();
  if (!api.real_connect(conn.m, use_socket ? nullptr : c.host.c_str(), c.user.c_str(),
                        c.password.c_str(), c.db.c_str(), c.port,
                        use_socket ? c.socket.c_str() : nullptr, 0)) {
    const std::string err = api.error(conn.m);
    conn.Close();
    throw std::runtime_error("mysql scan connection failed: " + err);
  }
}

void Exec(Conn &conn, const char *sql) {
  if (Api().real_query(conn.m, sql, std::strlen(sql)) != 0) {
    throw std::runtime_error(std::string("mysql statement failed: ") + sql + ": " +
                             Api().error(conn.m));
  }
}

} // namespace

struct SnapshotLane : Conn {};

namespace {

// The scan session of this thread (SnapshotScanSession), if any: its own
// connection, and the extra lanes partitioned scans spread over.
thread_local SnapshotLane *t_session = nullptr;
thread_local std::vector<std::unique_ptr<SnapshotLane>> t_extra_lanes;

bool CliForced() {
  const int forced = g_scan_mode_override.load();
  if (forced >= 0) return forced == 1;
  const char *v = std::getenv("ACCEL_MYSQL_SCAN");
  return v && std::strcmp(v, "cli") == 0;
}

// A connection holding one read view. (Never END it with a COMMIT/ROLLBACK
// statement: the audit plugin reads those as transaction events of the
// accelerators; closing the connection ends it.)
std::unique_ptr<SnapshotLane> OpenSnapshotConnection() {
  auto conn = std::make_unique<SnapshotLane>();
  Connect(*conn);
  Exec(*conn, "SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ");
  Exec(*conn, "START TRANSACTION WITH CONSISTENT SNAPSHOT, READ ONLY");
  return conn;
}
} // namespace

bool SnapshotScansSupported() {
  return !CliForced() && LoadClientApi() != nullptr;
}

// The views are opened back to back while the host admits no writer, so they
// all show the same commits.
SnapshotScanSession::SnapshotScanSession(std::size_t lanes) {
  if (t_session) throw std::logic_error("nested SnapshotScanSession");
  auto session = OpenSnapshotConnection();
  std::vector<std::unique_ptr<SnapshotLane>> extra;
  for (std::size_t lane = 1; lane < lanes; ++lane) {
    extra.push_back(OpenSnapshotConnection());
  }
  t_extra_lanes = std::move(extra);
  t_session = session.release();
}

SnapshotScanSession::~SnapshotScanSession() { Close(); }

void SnapshotScanSession::Close() {
  t_extra_lanes.clear();
  if (t_session) {
    delete t_session;
    t_session = nullptr;
  }
}

namespace {

// Streams `sql` over `conn` (any failure leaves the connection unusable and
// closes it).
void StreamQuery(Conn &conn, const std::string &sql,
                 const std::function<void(const std::vector<std::string> &)> &callback) {
  const ClientApi &api = Api();
  if (!conn.m) {
    throw std::runtime_error("mysql scan: the snapshot session's connection is broken");
  }
  if (api.real_query(conn.m, sql.data(), sql.size()) != 0) {
    const std::string err = api.error(conn.m);
    conn.Close(); // keep no half-broken session
    throw std::runtime_error("mysql scan failed: " + err + ": " + sql);
  }
  MYSQL_RES *res = api.use_result(conn.m); // streaming: rows are not buffered
  if (!res) {
    const std::string err = api.error(conn.m);
    throw std::runtime_error("mysql scan failed: " + err + ": " + sql);
  }
  const unsigned fields = api.num_fields(res);
  std::vector<std::string> cols(fields);
  std::vector<char> nulls(fields, 0);
  RowScope scope(&cols, &nulls);
  try {
    while (MYSQL_ROW row = api.fetch_row(res)) {
      const unsigned long *lengths = api.fetch_lengths(res);
      for (unsigned i = 0; i < fields; ++i) {
        if (row[i]) {
          cols[i].assign(row[i], lengths[i]); // reuses the string's capacity
          nulls[i] = 0;
        } else {
          cols[i].assign("NULL");
          nulls[i] = 1;
        }
      }
      callback(cols);
    }
    if (api.err_no(conn.m) != 0) {
      throw std::runtime_error(std::string("mysql scan failed mid-stream: ") +
                               api.error(conn.m) + ": " + sql);
    }
  } catch (...) {
    // The result is unfinished: the connection is unusable. Closing it frees
    // the result without draining the rest.
    conn.Close();
    throw;
  }
  api.free_result(res);
}

void ScanViaClient(const std::string &sql,
                   const std::function<void(const std::vector<std::string> &)> &callback) {
  if (t_session) {
    StreamQuery(*t_session, sql, callback);
    return;
  }
  Conn own;
  Connect(own);
  StreamQuery(own, sql, callback);
}

} // namespace

std::vector<SnapshotLane *> SnapshotLanes() {
  std::vector<SnapshotLane *> lanes;
  if (t_session) {
    lanes.push_back(t_session);
    for (const auto &lane : t_extra_lanes) lanes.push_back(lane.get());
  }
  return lanes;
}

void ScanOnSnapshotLane(SnapshotLane &lane, const std::string &sql,
                        const std::function<void(const std::vector<std::string> &)> &callback) {
  StreamQuery(lane, sql, callback);
}

#else // !ACCEL_MYSQL_HAVE_CLIENT

bool SnapshotScansSupported() { return false; }
struct SnapshotLane {};
std::vector<SnapshotLane *> SnapshotLanes() { return {}; }
void ScanOnSnapshotLane(SnapshotLane &, const std::string &,
                        const std::function<void(const std::vector<std::string> &)> &) {
  throw std::logic_error("snapshot scans need the MySQL client library (ACCEL_MYSQL_HAVE_CLIENT)");
}
SnapshotScanSession::SnapshotScanSession(std::size_t) {
  throw std::logic_error("snapshot scans need the MySQL client library (ACCEL_MYSQL_HAVE_CLIENT)");
}
SnapshotScanSession::~SnapshotScanSession() {}
void SnapshotScanSession::Close() {}

#endif

namespace {

// Streams `sql` through the mysql CLI (--quick: rows are not buffered by the
// client). The fallback when the plugin is built without the client library.
void ScanViaCli(const std::string &sql,
                const std::function<void(const std::vector<std::string> &)> &callback) {
  const std::string cmd = MysqlCliPrefix() + ShellEscape(sql) + " 2>/dev/null";
  FILE *pipe = popen(cmd.c_str(), "r");
  if (!pipe) {
    throw std::runtime_error("mysql scan popen failed");
  }
  struct Closer {
    FILE *f;
    bool done = false;
    ~Closer() { if (!done) pclose(f); }
  } closer{pipe};
  std::string line;
  char buf[1 << 16];
  std::vector<char> nulls;
  auto deliver = [&] {
    if (line.empty()) return;
    auto cols = SplitTsv(line);
    nulls.assign(cols.size(), 0);
    for (std::size_t i = 0; i < cols.size(); ++i) nulls[i] = cols[i] == "NULL";
    RowScope scope(&cols, &nulls);
    callback(cols);
    line.clear();
  };
  while (fgets(buf, sizeof(buf), pipe)) {
    const std::size_t n = std::strlen(buf);
    const bool eol = n > 0 && buf[n - 1] == '\n';
    line.append(buf, eol ? n - 1 : n);
    if (eol) deliver(); // a line longer than the buffer is assembled first
  }
  deliver();
  closer.done = true;
  if (pclose(pipe) != 0) {
    throw std::runtime_error("mysql scan failed: " + sql);
  }
}

} // namespace

void ScanTypedTuples(const std::string &sql,
                     const std::function<void(const std::vector<std::string> &)> &callback) {
  ++t_table_scans;
#if defined(ACCEL_MYSQL_HAVE_CLIENT)
  if (SnapshotScansSupported()) {
    ScanViaClient(sql, callback);
    return;
  }
#endif
  ScanViaCli(sql, callback);
}

namespace {

// The WHERE condition of partition `p` when the values [lo, hi] of the split
// column are cut into `partitions` equal-width ranges; the first and last
// ranges are open-ended (the first also takes NULLs), so the partitions
// cover every row once whatever the bounds. nullopt: the range holds nothing.
std::optional<std::string> PartitionCondition(const std::string &column, std::size_t partitions,
                                              std::size_t p, int64_t lo, int64_t hi) {
  const unsigned __int128 span = static_cast<unsigned __int128>(static_cast<__int128>(hi) - lo) + 1;
  const unsigned __int128 width = (span + partitions - 1) / partitions;
  const __int128 lower = static_cast<__int128>(lo) + static_cast<__int128>(width * p);
  const __int128 upper = lower + static_cast<__int128>(width);
  if (lower > hi) return std::nullopt;
  const auto text = [](__int128 value) { return std::to_string(static_cast<long long>(value)); };
  const bool first = p == 0, last = upper > hi;
  if (first && last) return std::string("TRUE");
  if (first) return "(" + column + " IS NULL OR " + column + " < " + text(upper) + ")";
  if (last) return column + " >= " + text(lower);
  return column + " >= " + text(lower) + " AND " + column + " < " + text(upper);
}

std::string SelectWhere(const SplitTable &table, const std::string &condition) {
  std::string sql = "SELECT " + table.select + " FROM " + table.table + " WHERE ";
  if (!table.where.empty()) sql += "(" + table.where + ") AND ";
  return sql + "(" + condition + ")";
}

// MIN and MAX of the split column; nullopt for an empty table.
std::optional<std::pair<int64_t, int64_t>> SplitBounds(const SplitTable &table) {
  std::optional<std::pair<int64_t, int64_t>> bounds;
  ScanTypedTuples("SELECT MIN(" + table.split_column + "), MAX(" + table.split_column + ") FROM " +
                      table.table,
                  [&](const std::vector<std::string> &cols) {
                    if (!TypedIsNull(cols, 0)) bounds.emplace(TypedInt(cols, 0), TypedInt(cols, 1));
                  });
  return bounds;
}

} // namespace

void ScanTypedTuplesPartitioned(const core::AuxEngineServices &services, const SplitTable &table,
                                std::size_t partitions, const PartitionedRowSink &sink) {
  if (partitions <= 1) {
    ScanTypedTuples(SelectWhere(table, "TRUE"),
                    [&](const std::vector<std::string> &cols) { sink(0, cols); });
    return;
  }
  // Outside a build's session the partitions share one view of their own.
  std::optional<SnapshotScanSession> own_view;
  if (SnapshotScansSupported() && SnapshotLanes().empty()) own_view.emplace();
  const auto bounds = SplitBounds(table);
  std::vector<std::string> statements(partitions);
  for (std::size_t p = 0; p < partitions; ++p) {
    const auto condition = bounds ? PartitionCondition(table.split_column, partitions, p,
                                                       bounds->first, bounds->second)
                                  : (p == 0 ? std::optional<std::string>("TRUE") : std::nullopt);
    if (condition) statements[p] = SelectWhere(table, *condition);
  }
  auto lanes = SnapshotLanes(); // taken here: the connections belong to this thread's session
  lanes.resize(std::min(lanes.size(), partitions));
  if (lanes.empty()) {
    for (std::size_t p = 0; p < partitions; ++p) {
      if (statements[p].empty()) continue;
      ScanTypedTuples(statements[p], [&](const std::vector<std::string> &cols) { sink(p, cols); });
    }
    return;
  }
  // Lane l runs partitions l, l + lanes, ... on its own connection.
  services.ParallelFor(lanes.size(), [&](std::size_t lane) {
    for (std::size_t p = lane; p < partitions; p += lanes.size()) {
      if (statements[p].empty()) continue;
      ScanOnSnapshotLane(*lanes[lane], statements[p],
                         [&](const std::vector<std::string> &cols) { sink(p, cols); });
    }
  });
}

const std::string &Col(const std::vector<std::string> &cols, int attribute) {
  static const std::string kEmpty;
  if (attribute < 0 || static_cast<std::size_t>(attribute) >= cols.size()) {
    return kEmpty;
  }
  return cols[static_cast<std::size_t>(attribute)];
}

bool TypedIsNull(const std::vector<std::string> &cols, int attribute) {
  if (attribute < 0 || static_cast<std::size_t>(attribute) >= cols.size()) {
    return true;
  }
  if (&cols == t_row && t_row_nulls) {
    return (*t_row_nulls)[static_cast<std::size_t>(attribute)] != 0;
  }
  const auto &text = cols[static_cast<std::size_t>(attribute)];
  return text == "NULL";
}

// NULL reads as 0 / "" in every accessor below (they have no NULL channel);
// ask TypedIsNull first when the difference matters.

int64_t TypedInt(const std::vector<std::string> &cols, int attribute) {
  if (TypedIsNull(cols, attribute)) {
    return 0;
  }
  const auto &text = Col(cols, attribute);
  int64_t value = 0;
  if (!ex::ParseInt64(text.data(), text.size(), value)) {
    throw std::runtime_error("invalid integer value '" + text.substr(0, 64) + "'");
  }
  return value;
}

int32_t TypedDate(const std::vector<std::string> &cols, int attribute) {
  if (TypedIsNull(cols, attribute)) {
    return 0;
  }
  return ParseUnixDays(Col(cols, attribute));
}

std::string TypedText(const std::vector<std::string> &cols, int attribute) {
  if (TypedIsNull(cols, attribute)) {
    return "";
  }
  return Col(cols, attribute);
}

core::ScaledDecimal TypedScaledDecimal(const std::vector<std::string> &cols, int attribute) {
  core::ScaledDecimal value;
  if (TypedIsNull(cols, attribute)) {
    return value;
  }
  const auto &text = Col(cols, attribute);
  if (!ex::ParseDecimal(text.data(), text.size(), value)) {
    throw std::runtime_error("invalid decimal value '" + text.substr(0, 64) + "'");
  }
  return value;
}

int64_t TypedDecimal(const std::vector<std::string> &cols, int attribute, int scale) {
  const core::ScaledDecimal value = TypedScaledDecimal(cols, attribute);
  int64_t raw = 0;
  if (!ex::Rescale(value, scale, raw)) {
    throw std::runtime_error("decimal value '" + Col(cols, attribute).substr(0, 64) +
                             "' is not exactly representable at scale " + std::to_string(scale));
  }
  return raw;
}

int64_t TypedDecimalRaw(const std::vector<std::string> &cols, int attribute) {
  return TypedDecimal(cols, attribute, 2);
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

char TypedChar(const std::vector<std::string> &cols, int attribute) {
  const auto text = TypedText(cols, attribute);
  return text.empty() ? '\0' : text[0];
}

} // namespace detail
} // namespace accelerator_codegen::aux_state::engine::mysql

#endif
