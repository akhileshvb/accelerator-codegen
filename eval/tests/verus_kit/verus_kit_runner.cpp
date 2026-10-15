// Generic test and benchmark runner for Verus accelerators registered through
// the host kit (verus_kit.hpp), on DuckDB.  Query-agnostic: everything about
// the query comes from its registered glue and a test-spec directory.
//
//   verus_kit_runner differential <db> <accel_id> <spec_dir> [every=10]
//   verus_kit_runner snapshots    <db> <accel_id> <spec_dir> [writes=200] [readers=4] [seed=1]
//   verus_kit_runner bench        <db> <accel_id> <spec_dir> [reps=5] [mixes=0.01,0.05,0.1] [ops=200]
//   verus_kit_runner sql          <db> <query> [max_rows=50]   (read-only exploration)
//   verus_kit_runner record       <db> <accel_id> <spec_dir> <probes_file> <out_dir>
//     (glue contract check: with ACCEL_VERUS_RECORD_DIR=<out_dir>, the C ABI
//     logs what the glue passes; this dumps each probe query (a line
//     "name<TAB>sql") before the build and before / after each transaction,
//     and marks the build and each transaction in <out_dir>/records.jsonl)
//   verus_kit_runner validate     <db> <spec_dir> <query_sql_file>
//     (no accelerator: setup runs, every parameter tuple evaluates and runs
//     the query, every transaction runs and is rolled back)
//
// Spec directory (written by the pipeline's adversarial-test stage):
//   setup.sql    optional; run once before the accelerator is built
//   params.sql   one parameter tuple per line, as SQL literals separated by
//                commas (e.g.  'SAUDI ARABIA'  or  DATE '1995-01-01', 24)
//   txns/*.sql   one write transaction per file (statements separated by ;)
//   bench_txns/*.sql  optional; the writes of `bench` (else txns/)
//
// differential: after the build and after every transaction (captured by the
//   engine), the accelerator (accel_<id>(...)) is compared with the query
//   itself at the same snapshot: all parameter tuples every `every`
//   transactions and at the end, two random ones otherwise.
// snapshots: readers in other connections run, inside one transaction, the
//   accelerator, a pause, the query, a pause, the accelerator, while a writer
//   commits; all three must agree.
// bench: per parameter tuple, median latency of the query vs the
//   accelerator; then mixes of queries and writes (fraction of writes per
//   operation), base (plain writes + query) vs accelerated.
//   The writes come from VERUS_KIT_WRITE_MODE:
//     txns    the spec's bench_txns (default)
//     rf      TPC-H refresh functions in mini-chunks (VERUS_KIT_RF_CHUNK orders,
//             default 1), alternating RF2 (delete the next smallest orders with
//             their lineitems) and RF1 (insert as many orders, cloned with their
//             lineitems from the largest ones, under fresh keys above the maximum)
//     rf_dim  the same shape for the dimension tables, cycling: delete parts with
//             their partsupp rows, insert cloned parts with theirs, then the same
//             for suppliers
//     auto    rf when the glue reads orders or lineitem, else rf_dim
//   In the rf modes the plain pass runs each write and rolls it back, and the
//   accelerated pass replays the same writes and commits them: the accelerator's
//   capture sees only its own connection, so it stays in sync with the database.
//   (The plain side then pays no commit: its write cost is a slight underestimate.)
// Parameter tuples drawn by the pipeline from the query's domain, fresh on every run:
//   VERUS_KIT_PARAMS_FILE=<file>        these tuples replace the spec's (bench, profiles)
//   VERUS_KIT_EXTRA_PARAMS_FILE=<file>  these are checked besides the spec's (differential, snapshots)
// (one tuple per line, as in params.sql)
// The store is the C++ one unless ACCEL_AUX_STORE=verus.  Output: one JSON
// object on stdout.  The database file is modified; pass a copy.
#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "backends/duckdb/api/register.hpp"
#include "substrate/aux/core/aux_execute_stats.hpp"
#include "substrate/aux/core/aux_maintenance_decision_log.hpp"
#include "substrate/verus/kit/verus_kit.hpp"
#include "catalog/query_catalog.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace {
namespace fs = std::filesystem;
namespace engine = accelerator_codegen::aux_state::engine;
namespace core = accelerator_codegen::aux_state::core;
namespace kit = accelerator_codegen::verus_kit;
using engine::duckdb::DuckdbRuntime;
using Clock = std::chrono::steady_clock;

void Require(bool ok, const std::string &msg) {
  if (!ok)
    throw std::runtime_error(msg);
}

std::string ReadFile(const fs::path &p) {
  std::ifstream in(p);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string Json(const std::string &s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\')
      o += '\\', o += static_cast<char>(c);
    else if (c < 0x20)
      o += "\\u00" + std::string(1, "0123456789abcdef"[c >> 4]) + "0123456789abcdef"[c & 15];
    else
      o += static_cast<char>(c);
  }
  return o + "\"";
}

struct Spec {
  std::string setup;
  std::vector<std::string> params; // SQL literal tuples
  std::vector<std::string> txns;
  std::vector<std::string> txn_names; // file names, for messages
  std::vector<std::string> bench_txns;
};

std::vector<std::string> SqlFiles(const fs::path &dir) {
  std::vector<std::string> out;
  if (!fs::is_directory(dir))
    return out;
  std::vector<fs::path> files;
  for (const auto &e : fs::directory_iterator(dir))
    if (e.path().extension() == ".sql")
      files.push_back(e.path());
  std::sort(files.begin(), files.end());
  for (const auto &f : files)
    out.push_back(ReadFile(f));
  return out;
}

Spec LoadSpec(const fs::path &dir) {
  Spec s;
  if (fs::exists(dir / "setup.sql"))
    s.setup = ReadFile(dir / "setup.sql");
  auto load_params = [&](const std::string &text) {
    std::vector<std::string> out;
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
      const auto b = line.find_first_not_of(" \t\r");
      if (b == std::string::npos || line.compare(b, 2, "--") == 0)
        continue;
      out.push_back(line.substr(b));
    }
    return out;
  };
  s.params = load_params(fs::exists(dir / "params.sql") ? ReadFile(dir / "params.sql") : "");
  // Random parameter tuples drawn by the pipeline from the query's domain (a fresh draw per run):
  //   VERUS_KIT_PARAMS_FILE        replaces the spec's tuples (bench, profiles)
  //   VERUS_KIT_EXTRA_PARAMS_FILE  adds to them (differential, snapshots)
  if (const char *f = std::getenv("VERUS_KIT_PARAMS_FILE"); f && *f) {
    auto drawn = load_params(ReadFile(f));
    if (!drawn.empty())
      s.params = std::move(drawn);
  }
  if (const char *f = std::getenv("VERUS_KIT_EXTRA_PARAMS_FILE"); f && *f)
    for (auto &t : load_params(ReadFile(f)))
      if (std::find(s.params.begin(), s.params.end(), t) == s.params.end())
        s.params.push_back(std::move(t));
  if (s.params.empty())
    s.params.push_back(""); // a query without parameters
  s.txns = SqlFiles(dir / "txns");
  if (fs::is_directory(dir / "txns")) {
    for (const auto &e : fs::directory_iterator(dir / "txns"))
      if (e.path().extension() == ".sql")
        s.txn_names.push_back(e.path().filename().string());
    std::sort(s.txn_names.begin(), s.txn_names.end());
  }
  s.txn_names.resize(s.txns.size());
  s.bench_txns = SqlFiles(dir / "bench_txns");
  if (s.bench_txns.empty())
    s.bench_txns = s.txns;
  return s;
}

std::unique_ptr<duckdb::MaterializedQueryResult> Q(duckdb::Connection &c, const std::string &sql) {
  auto r = c.Query(sql);
  Require(r && !r->HasError(), (r ? r->GetError() : "query failed") + ": " + sql);
  return r;
}

using Rows = std::vector<std::vector<std::string>>;

std::string Cell(const duckdb::Value &v) { return v.IsNull() ? "NULL" : v.ToString(); }

Rows Collect(duckdb::QueryResult &r) {
  auto &m = dynamic_cast<duckdb::MaterializedQueryResult &>(r);
  Rows rows;
  for (duckdb::idx_t i = 0; i < m.RowCount(); ++i) {
    std::vector<std::string> row;
    for (duckdb::idx_t c = 0; c < m.ColumnCount(); ++c)
      row.push_back(Cell(m.GetValue(c, i)));
    rows.push_back(std::move(row));
  }
  return rows;
}

// The query with its parameters (the tuple's literals, evaluated by DuckDB).
Rows RunQuery(duckdb::Connection &c, const std::string &query_sql, const std::string &tuple) {
  duckdb::vector<duckdb::Value> values;
  if (!tuple.empty()) {
    auto t = Q(c, "SELECT " + tuple);
    for (duckdb::idx_t i = 0; i < t->ColumnCount(); ++i)
      values.push_back(t->GetValue(i, 0));
  }
  auto stmt = c.Prepare(query_sql);
  Require(stmt && !stmt->HasError(), stmt ? stmt->GetError() : "prepare failed");
  auto r = stmt->Execute(values, false);
  Require(r && !r->HasError(), r ? r->GetError() : "query failed");
  return Collect(*r);
}

Rows RunAccel(duckdb::Connection &c, const std::string &id, const std::string &tuple) {
  auto r = Q(c, "SELECT * FROM accel_" + id + "(" + tuple + ")");
  return Collect(*r);
}

// Timed runs (bench): the same statements, the result materialized but not rendered as
// text -- per-cell Value::ToString is this harness's comparison format, not query work, and
// on a large answer it dominates both sides alike.
std::size_t TimeQuery(duckdb::Connection &c, const std::string &query_sql, const std::string &tuple) {
  duckdb::vector<duckdb::Value> values;
  if (!tuple.empty()) {
    auto t = Q(c, "SELECT " + tuple);
    for (duckdb::idx_t i = 0; i < t->ColumnCount(); ++i)
      values.push_back(t->GetValue(i, 0));
  }
  auto stmt = c.Prepare(query_sql);
  Require(stmt && !stmt->HasError(), stmt ? stmt->GetError() : "prepare failed");
  auto r = stmt->Execute(values, false);
  Require(r && !r->HasError(), r ? r->GetError() : "query failed");
  return dynamic_cast<duckdb::MaterializedQueryResult &>(*r).RowCount();
}

std::size_t TimeAccel(duckdb::Connection &c, const std::string &id, const std::string &tuple) {
  auto r = Q(c, "SELECT * FROM accel_" + id + "(" + tuple + ")");
  return dynamic_cast<duckdb::MaterializedQueryResult &>(*r).RowCount();
}

bool SameCell(const std::string &a, const std::string &b) {
  if (a == b)
    return true;
  char *ea = nullptr, *eb = nullptr;
  const double x = std::strtod(a.c_str(), &ea), y = std::strtod(b.c_str(), &eb);
  if (ea == a.c_str() || eb == b.c_str() || *ea || *eb)
    return false;
  return std::fabs(x - y) <= 1e-9 * std::max({1.0, std::fabs(x), std::fabs(y)});
}

bool SameRows(const Rows &a, const Rows &b) {
  if (a.size() != b.size())
    return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].size() != b[i].size())
      return false;
    for (std::size_t j = 0; j < a[i].size(); ++j)
      if (!SameCell(a[i][j], b[i][j]))
        return false;
  }
  return true;
}

// 0 same, 1 same rows in another order, 2 different.
int Compare(const Rows &accel, const Rows &query) {
  if (SameRows(accel, query))
    return 0;
  Rows x = accel, y = query;
  std::sort(x.begin(), x.end());
  std::sort(y.begin(), y.end());
  return SameRows(x, y) ? 1 : 2;
}

// The query's final ORDER BY (output columns) and LIMIT: SQL leaves the order of
// rows with equal keys open, and with a LIMIT, which of them make the cut.  An
// accelerator's answer is right if it is sorted by the keys, has the query's key
// sequence, and its rows are among the query's (unlimited) rows.
struct Ordering {
  std::vector<std::pair<std::size_t, bool>> keys; // column, descending
  bool limited = false;
  std::string unlimited_sql;                      // the query without its LIMIT
};

int CellOrder(const std::string &a, const std::string &b) {
  if (SameCell(a, b))
    return 0;
  char *ea = nullptr, *eb = nullptr;
  const double x = std::strtod(a.c_str(), &ea), y = std::strtod(b.c_str(), &eb);
  if (ea != a.c_str() && eb != b.c_str() && !*ea && !*eb)
    return x < y ? -1 : 1;
  return a < b ? -1 : 1; // text, ISO dates
}

// position of the last top-level occurrence of `word` (case-insensitive), or npos
std::size_t TopLevel(const std::string &sql, const std::string &word) {
  std::size_t found = std::string::npos;
  int depth = 0;
  bool quote = false;
  for (std::size_t i = 0; i < sql.size(); ++i) {
    const char ch = sql[i];
    if (ch == '\'')
      quote = !quote;
    if (quote)
      continue;
    depth += ch == '(' ? 1 : ch == ')' ? -1 : 0;
    if (depth == 0 && i + word.size() <= sql.size() && (i == 0 || !std::isalnum(sql[i - 1])) &&
        std::equal(word.begin(), word.end(), sql.begin() + i,
                   [](char x, char y) { return std::toupper(x) == std::toupper(y); }) &&
        (i + word.size() == sql.size() || !std::isalnum(sql[i + word.size()])))
      found = i;
  }
  return found;
}

Ordering MakeOrdering(duckdb::Connection &c, const std::string &sql, const std::vector<std::string> &tuples) {
  Ordering o;
  const auto ob = TopLevel(sql, "ORDER BY");
  if (ob == std::string::npos)
    return o;
  const auto lim = TopLevel(sql, "LIMIT");
  o.limited = lim != std::string::npos && lim > ob;
  o.unlimited_sql = o.limited ? sql.substr(0, lim) : sql;
  auto stmt = c.Prepare(sql);
  if (!stmt || stmt->HasError())
    return o;
  auto names = stmt->GetNames();
  // A parameter whose type only its value decides (e.g. `SUM(x) * $2`) leaves the statement
  // unbound until it runs: its output names are placeholders ("unknown") until then, so take
  // them from a run with the first tuple DuckDB can evaluate.
  if (!stmt->GetStatementProperties().bound_all_parameters)
    for (const auto &tuple : tuples) {
      try {
        duckdb::vector<duckdb::Value> values;
        if (!tuple.empty()) {
          auto t = Q(c, "SELECT " + tuple);
          for (duckdb::idx_t i = 0; i < t->ColumnCount(); ++i)
            values.push_back(t->GetValue(i, 0));
        }
        auto r = stmt->Execute(values, false);
        if (!r || r->HasError())
          continue;
        names = r->names;
        break;
      } catch (const std::exception &) {
      }
    }
  std::stringstream items(sql.substr(ob + 8, (o.limited ? lim : sql.size()) - ob - 8));
  for (std::string item; std::getline(items, item, ',');) {
    std::stringstream words(item);
    std::string name, dir;
    words >> name >> dir;
    for (auto &ch : dir)
      ch = static_cast<char>(std::toupper(ch));
    std::size_t col = names.size();
    for (std::size_t i = 0; i < names.size(); ++i) {
      std::string a = names[i], b = name;
      for (auto &ch : a) ch = static_cast<char>(std::tolower(ch));
      for (auto &ch : b) ch = static_cast<char>(std::tolower(ch));
      if (a == b) col = i;
    }
    if (col == names.size() && !name.empty() && std::all_of(name.begin(), name.end(), ::isdigit))
      col = std::stoul(name) - 1; // ORDER BY 2
    if (col >= names.size()) {    // an expression: keep the exact comparison
      o.keys.clear();
      return o;
    }
    o.keys.emplace_back(col, dir == "DESC");
  }
  return o;
}

bool SameKeys(const Ordering &o, const std::vector<std::string> &a, const std::vector<std::string> &b) {
  for (const auto &[col, desc] : o.keys)
    if (CellOrder(a[col], b[col]) != 0)
      return false;
  return true;
}

bool SortedBy(const Ordering &o, const Rows &rows) {
  for (std::size_t i = 1; i < rows.size(); ++i)
    for (const auto &[col, desc] : o.keys) {
      const int c = CellOrder(rows[i - 1][col], rows[i][col]) * (desc ? -1 : 1);
      if (c < 0)
        break;
      if (c > 0)
        return false;
    }
  return true;
}

// Compare, allowing the order (and LIMIT cut) that ties leave open.  `full`
// yields the query's rows without its LIMIT (only asked for when needed).
int CompareOrdered(const Ordering &o, const Rows &accel, const Rows &query, const std::function<Rows()> &full) {
  const int c = Compare(accel, query);
  if (c == 0 || o.keys.empty())
    return c;
  if (c == 1)
    return SortedBy(o, accel) ? 0 : 1;
  if (!o.limited || accel.size() != query.size() || !SortedBy(o, accel))
    return 2;
  for (std::size_t i = 0; i < accel.size(); ++i)
    if (accel[i].size() != query[i].size() || !SameKeys(o, accel[i], query[i]))
      return 2;
  Rows all = full(), mine = accel; // accel's rows must be among the query's
  std::sort(all.begin(), all.end());
  std::sort(mine.begin(), mine.end());
  std::size_t j = 0;
  for (const auto &row : mine) {
    while (j < all.size() && all[j] < row && !SameRows({all[j]}, {row}))
      ++j;
    if (j == all.size() || !SameRows({all[j]}, {row}))
      return 2;
    ++j;
  }
  return 0;
}

std::string RowsJson(const Rows &rows, std::size_t limit = 5) {
  std::string o = "[";
  for (std::size_t i = 0; i < rows.size() && i < limit; ++i) {
    o += i ? ",[" : "[";
    for (std::size_t j = 0; j < rows[i].size(); ++j)
      o += (j ? "," : "") + Json(rows[i][j]);
    o += "]";
  }
  if (rows.size() > limit)
    o += ",\"...\"";
  return o + "]";
}

struct Tally {
  int checks = 0, mismatches = 0, order_mismatches = 0;
  int reference_errors = 0; // DuckDB itself could not evaluate the query: nothing to compare
  std::vector<std::string> examples;
  // One comparison: the reference first (if DuckDB cannot evaluate it, e.g. a decimal
  // overflow on extreme test values, the check is skipped and counted); an accelerator
  // call that fails where the reference succeeds is a wrong answer.
  void CheckCall(const std::string &where, const std::string &tuple, const std::function<Rows()> &accel,
                 const std::function<Rows()> &query, const Ordering &o, const std::function<Rows()> &full) {
    Rows q;
    try {
      q = query();
    } catch (const std::exception &) {
      ++reference_errors;
      return;
    }
    Rows a;
    try {
      a = accel();
    } catch (const std::exception &e) {
      ++checks;
      ++mismatches;
      if (examples.size() < 8)
        examples.push_back("{\"at\":" + Json(where) + ",\"params\":" + Json(tuple) +
                           ",\"kind\":\"accel_error\",\"error\":" + Json(e.what()) + "}");
      return;
    }
    Check(where, tuple, a, q, o, full);
  }
  void Check(const std::string &where, const std::string &tuple, const Rows &a, const Rows &q,
             const Ordering &o = {}, const std::function<Rows()> &full = {}) {
    ++checks;
    const int c = CompareOrdered(o, a, q, full);
    if (c == 0)
      return;
    (c == 1 ? order_mismatches : mismatches)++;
    if (examples.size() < 8)
      examples.push_back("{\"at\":" + Json(where) + ",\"params\":" + Json(tuple) + ",\"kind\":" +
                         Json(c == 1 ? "order" : "rows") + ",\"accel_rows\":" + std::to_string(a.size()) +
                         ",\"query_rows\":" + std::to_string(q.size()) + ",\"accel\":" + RowsJson(a) +
                         ",\"query\":" + RowsJson(q) + "}");
  }
  std::string Fields() const {
    std::string ex = "[";
    for (std::size_t i = 0; i < examples.size(); ++i)
      ex += (i ? "," : "") + examples[i];
    return "\"checks\":" + std::to_string(checks) + ",\"reference_errors\":" + std::to_string(reference_errors) +
           ",\"mismatches\":" + std::to_string(mismatches) +
           ",\"order_mismatches\":" + std::to_string(order_mismatches) + ",\"examples\":" + ex + "]";
  }
};

struct Setup {
  const kit::Glue *glue;
  const engine::SharedQueryCatalogEntry *entry;
  Ordering ordering;
};

// The declared schema's constraints (ACCEL_VERUS_SCHEMA_SQL: e.g. ALTER TABLE ...
// SET NOT NULL), applied before anything else, so a write that leaves the
// schema's world fails instead of producing data the spec says cannot exist.
std::string SchemaSql() {
  const char *path = std::getenv("ACCEL_VERUS_SCHEMA_SQL");
  return path && *path && fs::exists(path) ? ReadFile(path) : std::string();
}

// with_setup=false (bench): measure the real data, without the tests' setup
// rows (edge cases for correctness, not a workload).
Setup Prepare(duckdb::Connection &setup, const std::string &id, const Spec &spec, bool with_setup = true) {
  Setup s{kit::Find(id), engine::VerusKitCatalogEntry(id)};
  Require(s.glue && s.entry, "no Verus accelerator registered as " + id);
  if (const auto schema = SchemaSql(); !schema.empty())
    Q(setup, schema);
  if (with_setup && !spec.setup.empty())
    Q(setup, spec.setup);
  accelerator_codegen::RegisterAcceleratorFunctions(setup);
  s.ordering = MakeOrdering(setup, s.glue->query_sql, spec.params);
  return s;
}

std::string StoreFields(DuckdbRuntime &rt) {
  const auto counts = core::ReadAuxExecuteCounts();
  return "\"store\":" + Json(rt.UsesVerusStore() ? "verus" : "cpp") + ",\"versions\":" +
         std::to_string(rt.StateVersionCount()) + ",\"stale_installs\":" + std::to_string(rt.StaleInstallCount()) +
         ",\"executes\":{\"hit\":" + std::to_string(counts.hit) + ",\"fallback\":" + std::to_string(counts.fallback) +
         "},\"reader_fallbacks\":" + std::to_string(rt.reader_fallbacks.load());
}

int Differential(const std::string &path, const std::string &id, const Spec &spec, int every) {
  duckdb::DuckDB db(path);
  duckdb::Connection wc(db), qc(db);
  const auto s = Prepare(qc, id, spec);
  DuckdbRuntime rt(wc, *s.entry, 10.0);
  core::ResetAuxExecuteCounts();
  const auto build_start = Clock::now();
  rt.Build();
  const double build_ms = std::chrono::duration<double, std::milli>(Clock::now() - build_start).count();
  Tally tally;
  std::mt19937_64 rng(7);
  auto check_all = [&](const std::string &where) {
    for (const auto &t : spec.params)
      tally.CheckCall(where, t, [&] { return RunAccel(qc, id, t); }, [&] { return RunQuery(qc, s.glue->query_sql, t); },
                        s.ordering, [&] { return RunQuery(qc, s.ordering.unlimited_sql, t); });
  };
  check_all("build");
  std::map<std::string, int> actions;
  int write_errors = 0;
  std::string write_error_examples = "[";
  for (std::size_t i = 0; i < spec.txns.size(); ++i) {
    core::ClearMaintenanceDecisionLog();
    try {
      rt.ApplyWrite(spec.txns[i]);
    } catch (const std::exception &e) {
      // the tests run on plain DuckDB (validated): a write the runtime cannot apply is a failure
      if (++write_errors <= 3)
        write_error_examples += std::string(write_errors > 1 ? "," : "") + "{\"txn\":" + Json(spec.txn_names[i]) +
                                ",\"error\":" + Json(std::string(e.what()).substr(0, 600)) + "}";
      continue;
    }
    const auto d = core::LastMaintenanceDecision();
    actions[d ? std::to_string(static_cast<int>(d->staged_action)) : "none"]++;
    const std::string where = "txn " + std::to_string(i);
    if (every > 0 && (i + 1) % static_cast<std::size_t>(every) == 0) {
      check_all(where);
    } else {
      for (int k = 0; k < 2; ++k) {
        const auto &t = spec.params[rng() % spec.params.size()];
        tally.CheckCall(where, t, [&] { return RunAccel(qc, id, t); }, [&] { return RunQuery(qc, s.glue->query_sql, t); },
                        s.ordering, [&] { return RunQuery(qc, s.ordering.unlimited_sql, t); });
      }
    }
  }
  check_all("end");
  std::string acts = "{";
  for (const auto &[k, v] : actions)
    acts += (acts.size() > 1 ? "," : "") + Json(k) + ":" + std::to_string(v);
  std::cout << "{\"mode\":\"differential\",\"txns\":" << spec.txns.size() << ",\"write_errors\":" << write_errors
            << ",\"write_error_examples\":" << write_error_examples << "]" << ",\"build_ms\":" << build_ms << "," << tally.Fields() << ",\"staged_actions\":" << acts
            << "},\"staged_action_names\":{\"1\":\"ApplyDelta\",\"3\":\"RebuildAll\"}," << StoreFields(rt) << "}"
            << std::endl;
  // skipped comparisons (reference errors) must stay a minority: a run that compared
  // little or nothing proves little
  const bool compared = tally.checks > 0 && tally.reference_errors <= tally.checks;
  return compared && write_errors == 0 && tally.mismatches + tally.order_mismatches == 0 && rt.StaleInstallCount() == 0 ? 0 : 1;
}

int Snapshots(const std::string &path, const std::string &id, const Spec &spec, int writes, int readers,
              uint64_t seed) {
  Require(!spec.txns.empty(), "snapshots needs txns/");
  duckdb::DuckDB db(path);
  duckdb::Connection wc(db), setup(db);
  const auto s = Prepare(setup, id, spec);
  DuckdbRuntime rt(wc, *s.entry, 10.0);
  rt.Build();
  std::atomic<bool> done{false};
  std::atomic<int> checks{0}, mismatches{0}, failures{0}, reference_errors{0};
  std::mutex ex_mutex;
  std::vector<std::string> examples;
  std::vector<std::thread> threads;
  for (int t = 0; t < readers; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937_64 rng(seed * 1000003 + static_cast<uint64_t>(t));
      try {
        duckdb::Connection rc(db);
        while (!done.load()) {
          const auto &p = spec.params[rng() % spec.params.size()];
          Q(rc, "BEGIN TRANSACTION");
          const auto a = RunAccel(rc, id, p);
          std::this_thread::sleep_for(std::chrono::microseconds(rng() % 3000));
          Rows b;
          try {
            b = RunQuery(rc, s.glue->query_sql, p);
          } catch (const std::exception &) {
            // DuckDB itself cannot evaluate the reference here (e.g. a decimal overflow on extreme
            // test values): nothing to compare; skipped and counted, as in differential
            Q(rc, "ROLLBACK");
            ++reference_errors;
            continue;
          }
          std::this_thread::sleep_for(std::chrono::microseconds(rng() % 3000));
          const auto c = RunAccel(rc, id, p);
          const auto full = [&] { return RunQuery(rc, s.ordering.unlimited_sql, p); }; // same snapshot
          const bool wrong = CompareOrdered(s.ordering, a, b, full) != 0 || CompareOrdered(s.ordering, c, b, full) != 0;
          Q(rc, "COMMIT");
          ++checks;
          if (wrong) {
            ++mismatches;
            std::lock_guard<std::mutex> lock(ex_mutex);
            if (examples.size() < 5)
              examples.push_back("{\"reader\":" + std::to_string(t) + ",\"params\":" + Json(p) + ",\"accel\":" +
                                 RowsJson(a) + ",\"query\":" + RowsJson(b) + ",\"accel_again\":" + RowsJson(c) + "}");
          }
        }
      } catch (const std::exception &e) {
        if (++failures <= 3)
          std::cerr << "reader " << t << ": " << e.what() << "\n";
      }
    });
  }
  std::mt19937_64 rng(seed);
  int write_errors = 0;
  std::string write_error_example;
  for (int i = 0; i < writes; ++i) {
    try {
      rt.ApplyWrite(spec.txns[rng() % spec.txns.size()]);
    } catch (const std::exception &e) {
      // random order: a transaction can legitimately fail here (its rows gone)
      if (++write_errors == 1)
        write_error_example = std::string(e.what()).substr(0, 600);
    }
    std::this_thread::sleep_for(std::chrono::microseconds(rng() % 2000));
  }
  done = true;
  for (auto &th : threads)
    th.join();
  std::string ex = "[";
  for (std::size_t i = 0; i < examples.size(); ++i)
    ex += (i ? "," : "") + examples[i];
  std::cout << "{\"mode\":\"snapshots\",\"writes\":" << writes << ",\"write_errors\":" << write_errors
            << ",\"write_error_example\":" << Json(write_error_example) << ",\"readers\":" << readers << ",\"checks\":" << checks << ",\"reference_errors\":" << reference_errors
            << ",\"mismatches\":" << mismatches << ",\"reader_failures\":" << failures << ",\"examples\":" << ex << "]," << StoreFields(rt) << "}"
            << std::endl;
  // some writes must commit, or the readers saw no concurrent maintenance
  return mismatches == 0 && failures == 0 && write_errors < writes && checks > 0 && reference_errors <= checks &&
                 rt.StaleInstallCount() == 0
             ? 0
             : 1;
}

double Median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v.empty() ? 0.0 : v[v.size() / 2];
}

// VERUS_KIT_ACCEL_ONLY=1: only the accelerator's side runs (for profiling it).
// Profiling hooks: VERUS_KIT_PERF_CTL / _ACK are perf's control FIFOs
// (perf record -D -1 --control fifo:ctl,ack): profiling starts after the build,
// so a profile shows the steady state (reads, write maintenance), not the
// one-time build.
void PerfEnable() {
  const char *ctl = std::getenv("VERUS_KIT_PERF_CTL");
  if (!ctl || !*ctl)
    return;
  if (FILE *f = std::fopen(ctl, "w")) {
    std::fputs("enable\n", f);
    std::fclose(f);
  }
  if (const char *ack = std::getenv("VERUS_KIT_PERF_ACK"); ack && *ack) {
    if (FILE *f = std::fopen(ack, "r")) {
      char buf[16];
      (void)std::fgets(buf, sizeof buf, f);
      std::fclose(f);
    }
  }
}

std::vector<int64_t> Keys(duckdb::Connection &c, const std::string &sql) {
  auto r = c.Query(sql);
  Require(r && !r->HasError(), r ? r->GetError() : sql);
  std::vector<int64_t> keys;
  for (duckdb::idx_t i = 0; i < r->RowCount(); ++i)
    keys.push_back(r->GetValue(0, i).GetValue<int64_t>());
  return keys;
}

// Deterministic refresh-style writes (see VERUS_KIT_WRITE_MODE above); a copy
// replays the same sequence.
struct RfWrites {
  struct Entity {
    std::string table, key, partsupp_key; // partsupp_key empty: orders (with lineitem)
    std::vector<int64_t> victims, templates;
    int64_t next_key = 0;
    std::size_t victim_at = 0, template_at = 0;
  };
  std::vector<Entity> entities; // rf: orders; rf_dim: part, supplier
  int chunk = 1;
  long seq = 0;

  static Entity Load(duckdb::Connection &c, const std::string &table, const std::string &key,
                     const std::string &partsupp_key, std::size_t victims) {
    Entity e{table, key, partsupp_key, {}, {}, 0, 0, 0};
    e.victims = Keys(c, "SELECT " + key + " FROM " + table + " ORDER BY " + key + " LIMIT " + std::to_string(victims));
    e.templates = Keys(c, "SELECT " + key + " FROM " + table + " ORDER BY " + key + " DESC LIMIT 1000");
    e.next_key = Keys(c, "SELECT max(" + key + ") FROM " + table)[0] + 1;
    Require(!e.templates.empty(), "rf: empty table " + table);
    return e;
  }

  std::string Next() {
    const long op = seq++;
    auto &e = entities[(op / 2) % entities.size()];
    std::ostringstream sql;
    if (op % 2 == 0) { // delete the next victims
      Require(e.victim_at + chunk <= e.victims.size(), "rf: out of " + e.table + " keys to delete");
      std::string in = "(";
      for (int j = 0; j < chunk; ++j)
        in += (j ? "," : "") + std::to_string(e.victims[e.victim_at++]);
      in += ")";
      if (e.partsupp_key.empty())
        sql << "DELETE FROM lineitem WHERE l_orderkey IN " << in << "; ";
      else
        sql << "DELETE FROM partsupp WHERE " << e.partsupp_key << " IN " << in << "; ";
      sql << "DELETE FROM " << e.table << " WHERE " << e.key << " IN " << in;
    } else { // insert clones under fresh keys
      for (int j = 0; j < chunk; ++j) {
        const auto from = std::to_string(e.templates[e.template_at++ % e.templates.size()]);
        const auto key = std::to_string(e.next_key++);
        sql << (j ? "; " : "") << "INSERT INTO " << e.table << " SELECT * REPLACE (" << key << " AS " << e.key
            << ") FROM " << e.table << " WHERE " << e.key << " = " << from << "; ";
        if (e.partsupp_key.empty())
          sql << "INSERT INTO lineitem SELECT * REPLACE (" << key << " AS l_orderkey) FROM lineitem WHERE l_orderkey = "
              << from;
        else
          sql << "INSERT INTO partsupp SELECT * REPLACE (" << key << " AS " << e.partsupp_key
              << ") FROM partsupp WHERE " << e.partsupp_key << " = " << from;
      }
    }
    return sql.str();
  }
};

std::string WriteMode(const kit::Glue &glue) {
  const char *env = std::getenv("VERUS_KIT_WRITE_MODE");
  std::string mode = env && *env ? env : "txns";
  if (mode == "auto") {
    mode = "rf_dim";
    for (const auto &t : glue.tables)
      if (std::string(t.name) == "orders" || std::string(t.name) == "lineitem")
        mode = "rf";
  }
  Require(mode == "txns" || mode == "rf" || mode == "rf_dim",
          "VERUS_KIT_WRITE_MODE must be txns, rf, rf_dim or auto");
  return mode;
}

int Bench(const std::string &path, const std::string &id, const Spec &spec, int reps, std::vector<double> mixes,
          int ops) {
  // VERUS_KIT_BENCH_PHASE=reads|writes: only that part (for per-axis profiles)
  const char *phase_env = std::getenv("VERUS_KIT_BENCH_PHASE");
  const std::string phase = phase_env ? phase_env : "";
  if (phase == "reads")
    mixes.clear();
  const bool accel_only = std::getenv("VERUS_KIT_ACCEL_ONLY") != nullptr;
  duckdb::DuckDB db(path);
  duckdb::Connection wc(db), qc(db), plain(db);
  const auto s = Prepare(qc, id, spec, /*with_setup=*/false);
  DuckdbRuntime rt(wc, *s.entry, 10.0);
  const auto build_start = Clock::now();
  rt.Build();
  const double build_ms = std::chrono::duration<double, std::milli>(Clock::now() - build_start).count();
  // the parameter tuples the plain query can evaluate on this database (the tests' extreme tuples can
  // overflow it at scale): those are what is timed
  std::vector<std::string> usable;
  for (const auto &t : spec.params) {
    try {
      RunQuery(qc, s.glue->query_sql, t);
      usable.push_back(t);
    } catch (const std::exception &) {
    }
  }
  Require(!usable.empty(), "bench: the plain query fails for every parameter tuple on this database");
  const std::string write_mode = WriteMode(*s.glue);
  const char *chunk_env = std::getenv("VERUS_KIT_RF_CHUNK");
  const int rf_chunk = chunk_env && *chunk_env ? std::max(1, std::atoi(chunk_env)) : 1;
  std::optional<RfWrites> rf;
  if (write_mode != "txns") {
    rf.emplace();
    rf->chunk = rf_chunk;
    const std::size_t victims = static_cast<std::size_t>(ops) * std::max<std::size_t>(mixes.size(), 1) * rf_chunk + 16;
    if (write_mode == "rf")
      rf->entities.push_back(RfWrites::Load(qc, "orders", "o_orderkey", "", victims));
    else {
      rf->entities.push_back(RfWrites::Load(qc, "part", "p_partkey", "ps_partkey", victims));
      rf->entities.push_back(RfWrites::Load(qc, "supplier", "s_suppkey", "ps_suppkey", victims));
    }
  }
  const bool have_writes = rf.has_value() || !spec.bench_txns.empty();
  PerfEnable();
  auto us = [](Clock::time_point a) { return std::chrono::duration<double, std::micro>(Clock::now() - a).count(); };
  // read-only: per tuple, medians of the query and of the accelerator
  std::string per = "[";
  double log_sum = 0;
  int n = 0;
  std::size_t tuples = phase == "writes" ? 0 : std::min<std::size_t>(usable.size(), 12);
  for (std::size_t i = 0; i < tuples; ++i) {
    const auto &t = usable[i];
    std::vector<double> q, a;
    TimeAccel(qc, id, t); // warm
    for (int r = 0; r < reps; ++r) {
      auto t0 = Clock::now();
      if (!accel_only)
        TimeQuery(qc, s.glue->query_sql, t);
      q.push_back(us(t0));
      t0 = Clock::now();
      TimeAccel(qc, id, t);
      a.push_back(us(t0));
    }
    const double mq = Median(q), ma = Median(a);
    log_sum += std::log(mq / std::max(ma, 1.0));
    ++n;
    per += (i ? "," : "") + std::string("{\"params\":") + Json(t) + ",\"query_us\":" + std::to_string(mq) +
           ",\"accel_us\":" + std::to_string(ma) + "}";
  }
  per += "]";
  const double read_speedup = n ? std::exp(log_sum / n) : 0.0;
  // mixes: ops operations, each a write with probability f, else a query
  std::string mix_json = "[";
  double maint_us_sum = 0, plain_write_us_sum = 0;
  int maint_n = 0, plain_write_n = 0;
  for (std::size_t m = 0; m < mixes.size(); ++m) {
    const double f = mixes[m];
    double base_us = 0, accel_us = 0;
    std::optional<RfWrites> replay = rf; // the plain pass replays the writes the accelerated pass commits
    for (int pass = accel_only ? 1 : 0; pass < 2; ++pass) {
      std::mt19937_64 rng(1234 + m);
      const auto t0 = Clock::now();
      for (int i = 0; i < ops; ++i) {
        const bool write = have_writes && std::uniform_real_distribution<double>(0, 1)(rng) < f;
        if (write) {
          const std::string sql = rf ? (pass == 0 ? replay : rf)->Next()
                                     : spec.bench_txns[rng() % spec.bench_txns.size()];
          if (pass == 0) {
            const auto w0 = Clock::now();
            auto r = plain.Query("BEGIN; " + sql + (rf ? "; ROLLBACK;" : "; COMMIT;"));
            if (r && r->HasError())
              plain.Query("ROLLBACK");
            plain_write_us_sum += us(w0);
            ++plain_write_n;
          } else {
            const auto w0 = Clock::now();
            try {
              rt.ApplyWrite(sql);
            } catch (const std::exception &) {
            }
            maint_us_sum += us(w0);
            ++maint_n;
          }
        } else {
          const auto &t = usable[rng() % usable.size()];
          try { // a write can push a value past the plain query's range: skip that read in both passes
            if (pass == 0)
              TimeQuery(qc, s.glue->query_sql, t);
            else
              TimeAccel(qc, id, t);
          } catch (const std::exception &) {
          }
        }
      }
      (pass == 0 ? base_us : accel_us) = us(t0);
    }
    mix_json += (m ? "," : "") + std::string("{\"write_fraction\":") + std::to_string(f) +
                ",\"base_us\":" + std::to_string(base_us) + ",\"accel_us\":" + std::to_string(accel_us) +
                ",\"speedup\":" + std::to_string(base_us / std::max(accel_us, 1.0)) + "}";
  }
  mix_json += "]";
  std::cout << "{\"mode\":\"bench\",\"build_ms\":" << build_ms << ",\"read_speedup\":" << read_speedup
            << ",\"per_params\":" << per << ",\"mixes\":" << mix_json << ",\"write_us_mean\":"
            << (maint_n ? maint_us_sum / maint_n : 0.0) << ",\"plain_write_us_mean\":"
            << (plain_write_n ? plain_write_us_sum / plain_write_n : 0.0) << ",\"write_mode\":" << Json(write_mode)
            << ",\"rf_chunk\":" << rf_chunk << "," << StoreFields(rt) << "}" << std::endl;
  return 0;
}

} // namespace

int Record(const std::string &path, const std::string &id, const Spec &spec, const std::string &probes_path,
           const std::string &out_dir) {
  std::vector<std::pair<std::string, std::string>> probes;
  {
    std::istringstream in(ReadFile(probes_path));
    for (std::string line; std::getline(in, line);) {
      const auto tab = line.find('\t');
      if (tab != std::string::npos)
        probes.emplace_back(line.substr(0, tab), line.substr(tab + 1));
    }
  }
  duckdb::DuckDB db(path);
  duckdb::Connection wc(db), qc(db);
  const auto s = Prepare(qc, id, spec);
  auto mark = [&](const std::string &json) {
    std::ofstream(fs::path(out_dir) / "records.jsonl", std::ios::app) << json << "\n";
  };
  auto dump = [&](const std::string &label) {
    for (const auto &[name, sql] : probes) {
      auto r = Q(qc, sql);
      const auto rows = Collect(*r);
      std::string cols = "[";
      for (duckdb::idx_t i = 0; i < r->ColumnCount(); ++i)
        cols += (i ? "," : "") + Json(r->ColumnName(i));
      std::ofstream(fs::path(out_dir) / ("probe_" + label + "_" + name + ".json"))
          << "{\"columns\":" << cols << "],\"rows\":" << RowsJson(rows, rows.size()) << "}";
    }
  };
  DuckdbRuntime rt(wc, *s.entry, 10.0);
  dump("build");
  mark("{\"marker\":\"build\"}");
  rt.Build();
  int write_errors = 0;
  for (std::size_t i = 0; i < spec.txns.size(); ++i) {
    dump("before_" + std::to_string(i));
    mark("{\"marker\":\"txn\",\"i\":" + std::to_string(i) + "}");
    try {
      rt.ApplyWrite(spec.txns[i]);
    } catch (const std::exception &e) {
      ++write_errors;
      mark("{\"marker\":\"txn_error\",\"i\":" + std::to_string(i) + ",\"error\":" + Json(e.what()) + "}");
    }
    dump("after_" + std::to_string(i));
  }
  std::cout << "{\"mode\":\"record\",\"txns\":" << spec.txns.size() << ",\"write_errors\":" << write_errors << "}"
            << std::endl;
  return 0;
}

// A read-only query (rolled back), for exploring the data.
int Sql(const std::string &path, const std::string &sql, std::size_t max_rows) {
  duckdb::DBConfig config;
  config.options.access_mode = duckdb::AccessMode::READ_ONLY;
  duckdb::DuckDB db(path, &config);
  duckdb::Connection c(db);
  auto r = Q(c, sql);
  const auto rows = Collect(*r);
  std::string cols = "[";
  for (duckdb::idx_t i = 0; i < r->ColumnCount(); ++i)
    cols += (i ? "," : "") + Json(r->ColumnName(i));
  std::cout << "{\"columns\":" << cols << "],\"row_count\":" << rows.size() << ",\"rows\":" << RowsJson(rows, max_rows)
            << "}" << std::endl;
  return 0;
}

// The spec runs on the database: no accelerator needed (tests are written
// before the kernel).
int Validate(const std::string &path, const Spec &spec, const std::string &query_sql) {
  duckdb::DuckDB db(path);
  duckdb::Connection c(db);
  std::vector<std::string> errors;
  if (const auto schema = SchemaSql(); !schema.empty()) {
    auto r = c.Query(schema);
    if (!r || r->HasError())
      errors.push_back("schema constraints: " + (r ? r->GetError() : std::string("failed")));
  }
  if (!spec.setup.empty()) {
    auto r = c.Query(spec.setup);
    if (!r || r->HasError())
      errors.push_back("setup.sql: " + (r ? r->GetError() : std::string("failed")));
  }
  int nonempty = 0;
  for (const auto &t : spec.params) {
    try {
      nonempty += !RunQuery(c, query_sql, t).empty();
    } catch (const std::exception &e) {
      errors.push_back("params " + t + ": " + e.what());
    }
  }
  // In sequence, committed, as the tests run: a transaction is checked on the
  // state the ones before it leave (a subquery that finds a row on the
  // original data may find none later).
  int txn_errors = 0;
  for (std::size_t i = 0; i < spec.txns.size(); ++i) {
    c.BeginTransaction();
    auto r = c.Query(spec.txns[i]);
    if (!r || r->HasError()) {
      ++txn_errors;
      if (errors.size() < 20)
        errors.push_back("txn " + std::to_string(i) + " (" + spec.txn_names[i] + "): " +
                         (r ? r->GetError() : std::string("failed")));
      c.Rollback();
    } else {
      c.Commit();
    }
  }
  std::string ej = "[";
  for (std::size_t i = 0; i < errors.size(); ++i)
    ej += (i ? "," : "") + Json(errors[i]);
  std::cout << "{\"mode\":\"validate\",\"params\":" << spec.params.size() << ",\"params_nonempty\":" << nonempty
            << ",\"txns\":" << spec.txns.size() << ",\"bench_txns\":" << spec.bench_txns.size()
            << ",\"txn_errors\":" << txn_errors << ",\"errors\":" << ej << "]}" << std::endl;
  return errors.empty() ? 0 : 1;
}

int main(int argc, char **argv) {
  try {
    if (argc >= 7 && std::string(argv[1]) == "record")
      return Record(argv[2], argv[3], LoadSpec(argv[4]), argv[5], argv[6]);
    if (argc >= 5 && std::string(argv[1]) == "validate")
      return Validate(argv[2], LoadSpec(argv[3]), ReadFile(argv[4]));
    if (argc >= 4 && std::string(argv[1]) == "sql")
      return Sql(argv[2], argv[3], argc > 4 ? std::stoul(argv[4]) : 50);
    Require(argc >= 5, "usage: verus_kit_runner differential|snapshots|bench <db> <accel_id> <spec_dir> [...]");
    const std::string mode = argv[1], db = argv[2], id = argv[3];
    const Spec spec = LoadSpec(argv[4]);
    auto arg = [&](int i, const char *def) { return std::string(argc > i ? argv[i] : def); };
    if (mode == "differential")
      return Differential(db, id, spec, std::stoi(arg(5, "10")));
    if (mode == "snapshots")
      return Snapshots(db, id, spec, std::stoi(arg(5, "200")), std::stoi(arg(6, "4")), std::stoull(arg(7, "1")));
    if (mode == "bench") {
      std::vector<double> mixes;
      std::stringstream ss(arg(6, "0.01,0.05,0.1"));
      for (std::string x; std::getline(ss, x, ',');)
        if (!x.empty())
          mixes.push_back(std::stod(x));
      return Bench(db, id, spec, std::stoi(arg(5, "5")), mixes, std::stoi(arg(7, "200")));
    }
    Require(false, "unknown mode " + mode);
  } catch (const std::exception &e) {
    std::cout << "{\"error\":" << Json(e.what()) << "}" << std::endl;
    return 2;
  }
  return 2;
}
