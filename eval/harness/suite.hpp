// SCAFFOLDING
// The declarative test-suite format of an accelerator and the checks that run it, shared by the
// Verus kit runner and the engine harnesses.  A suite directory (accelerators/<bench>/q<N>/tests/workload/):
//   setup.sql    optional; run once before the accelerator is built
//   params.sql   one parameter tuple per line, as SQL literals separated by commas
//                (e.g.  'SAUDI ARABIA'  or  DATE '1995-01-01', 24)
//   txns/*.sql   one write transaction per file (statements separated by ;), applied in order, committed
//   bench_txns/*.sql  optional; the writes of a benchmark (else txns/)
// Fresh random tuples from the parameter domain come in per run through a file (LoadSpec).
#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"
#include "eval/harness/float_bounds.hpp"
#include "substrate/aux/core/aux_maintenance_decision_log.hpp"

namespace accelerator_codegen::suite {
namespace fs = std::filesystem;
namespace core = accelerator_codegen::aux_state::core;

inline void Require(bool ok, const std::string &msg) {
  if (!ok)
    throw std::runtime_error(msg);
}

inline std::string ReadFile(const fs::path &p) {
  std::ifstream in(p);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

inline std::string Json(const std::string &s) {
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

inline std::vector<std::string> SqlFiles(const fs::path &dir) {
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

inline Spec LoadSpec(const fs::path &dir, const char *replace_env, const char *extra_env) {
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
  // Random parameter tuples drawn from the query's domain (a fresh draw per run): the file named by
  // `replace_env` replaces the spec's tuples (bench, profiles), the one named by `extra_env` adds to them.
  if (const char *f = std::getenv(replace_env); f && *f) {
    auto drawn = load_params(ReadFile(f));
    if (!drawn.empty())
      s.params = std::move(drawn);
  }
  if (const char *f = std::getenv(extra_env); f && *f)
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

inline std::unique_ptr<duckdb::MaterializedQueryResult> Q(duckdb::Connection &c, const std::string &sql) {
  auto r = c.Query(sql);
  Require(r && !r->HasError(), (r ? r->GetError() : "query failed") + ": " + sql);
  return r;
}

using Rows = std::vector<std::vector<std::string>>;

inline std::string Cell(const duckdb::Value &v) { return v.IsNull() ? "NULL" : v.ToString(); }

inline Rows Collect(duckdb::QueryResult &r) {
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
inline Rows RunQuery(duckdb::Connection &c, const std::string &query_sql, const std::string &tuple) {
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

// The bounds query's candidates (float_bounds.hpp), at the connection's snapshot.
inline float_bounds::Probe RunProbe(duckdb::Connection &c, const std::string &bounds_sql, const std::string &tuple) {
  Q(c, float_bounds::kPrelude);
  duckdb::vector<duckdb::Value> values;
  if (!tuple.empty()) {
    auto t = Q(c, "SELECT " + tuple);
    for (duckdb::idx_t i = 0; i < t->ColumnCount(); ++i)
      values.push_back(t->GetValue(i, 0));
  }
  auto stmt = c.Prepare(bounds_sql);
  Require(stmt && !stmt->HasError(), stmt ? stmt->GetError() : "prepare failed");
  auto r = stmt->Execute(values, false);
  Require(r && !r->HasError(), r ? r->GetError() : "bounds query failed");
  return {r->names, Collect(*r)};
}

inline Rows RunAccel(duckdb::Connection &c, const std::string &function, const std::string &tuple) {
  auto r = Q(c, "SELECT * FROM " + function + "(" + tuple + ")");
  return Collect(*r);
}

// Numeric cells agree within a relative 1e-9: the engine and the kernel sum DOUBLEs in different
// orders, so their results differ in the last bits.  This slack is for testing only; what is trusted
// is the generated spec's error bound (verus_common::rel::float_within), not this tolerance.
inline bool SameCell(const std::string &a, const std::string &b) {
  if (a == b)
    return true;
  char *ea = nullptr, *eb = nullptr;
  const double x = std::strtod(a.c_str(), &ea), y = std::strtod(b.c_str(), &eb);
  if (ea == a.c_str() || eb == b.c_str() || *ea || *eb)
    return false;
  if (std::isnan(x) || std::isnan(y) || std::isinf(x) || std::isinf(y)) // NaN equals NaN; infinities must match
    return (std::isnan(x) && std::isnan(y)) || x == y;
  return std::fabs(x - y) <= 1e-9 * std::max({1.0, std::fabs(x), std::fabs(y)});
}

inline bool SameRows(const Rows &a, const Rows &b) {
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
inline int Compare(const Rows &accel, const Rows &query) {
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
  bool bag = false;                               // no ORDER BY: any order is right
  std::vector<std::pair<std::size_t, bool>> keys; // column, descending
  bool limited = false;
  std::string unlimited_sql;                      // the query without its LIMIT
  // Rounded DOUBLE aggregates (float_bounds.hpp): the bounds query, and the query it bounds.  Set,
  // an answer is judged admissible instead of compared with the reference.
  std::string bounds_sql, query_sql;
};

inline int CellOrder(const std::string &a, const std::string &b) {
  if (SameCell(a, b))
    return 0;
  char *ea = nullptr, *eb = nullptr;
  const double x = std::strtod(a.c_str(), &ea), y = std::strtod(b.c_str(), &eb);
  if (ea != a.c_str() && eb != b.c_str() && !*ea && !*eb)
    return x < y ? -1 : 1;
  return a < b ? -1 : 1; // text, ISO dates
}

// position of the last top-level occurrence of `word` (case-insensitive), or npos
inline std::size_t TopLevel(const std::string &sql, const std::string &word) {
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

inline Ordering MakeOrdering(duckdb::Connection &c, const std::string &sql, const std::vector<std::string> &tuples) {
  Ordering o;
  const auto ob = TopLevel(sql, "ORDER BY");
  if (ob == std::string::npos) {
    o.bag = true;
    return o;
  }
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

inline bool SameKeys(const Ordering &o, const std::vector<std::string> &a, const std::vector<std::string> &b) {
  for (const auto &[col, desc] : o.keys)
    if (CellOrder(a[col], b[col]) != 0)
      return false;
  return true;
}

inline bool SortedBy(const Ordering &o, const Rows &rows) {
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
inline int CompareOrdered(const Ordering &o, const Rows &accel, const Rows &query, const std::function<Rows()> &full,
                          const std::function<float_bounds::Probe()> &probe = {}, std::string *why = nullptr) {
  if (!o.bounds_sql.empty() && probe) {
    const std::string reason = float_bounds::Judge(o.query_sql, probe(), accel);
    if (why)
      *why = reason;
    return reason.empty() ? 0 : 2;
  }
  const int c = Compare(accel, query);
  if (o.bag && c == 1)
    return 0;
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

inline std::string RowsJson(const Rows &rows, std::size_t limit = 5) {
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
  std::vector<std::string> fallback_examples, reference_error_examples;
  std::map<std::string, int> fallback_kinds; // kernel_none | param_outside_type | host_conversion
  // One comparison: the reference first (if DuckDB cannot evaluate it, e.g. a decimal
  // overflow on extreme test values, the check is skipped and counted); an accelerator
  // call that fails where the reference succeeds is a wrong answer.
  void CheckCall(const std::string &where, const std::string &tuple, const std::function<Rows()> &accel,
                 const std::function<Rows()> &query, const Ordering &o, const std::function<Rows()> &full,
                 const std::function<float_bounds::Probe()> &probe = {}) {
    Rows q;
    try {
      q = query();
    } catch (const std::exception &e) {
      ++reference_errors;
      if (reference_error_examples.size() < 8)
        reference_error_examples.push_back("{\"at\":" + Json(where) + ",\"params\":" + Json(tuple) +
                                           ",\"error\":" + Json(std::string(e.what()).substr(0, 300)) + "}");
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
    Check(where, tuple, a, q, o, full, probe);
  }
  void Check(const std::string &where, const std::string &tuple, const Rows &a, const Rows &q,
             const Ordering &o = {}, const std::function<Rows()> &full = {},
             const std::function<float_bounds::Probe()> &probe = {}) {
    ++checks;
    std::string why;
    const int c = CompareOrdered(o, a, q, full, probe, &why);
    if (c == 0)
      return;
    (c == 1 ? order_mismatches : mismatches)++;
    if (examples.size() < 8)
      examples.push_back("{\"at\":" + Json(where) + ",\"params\":" + Json(tuple) + ",\"kind\":" +
                         Json(c == 1 ? "order" : "rows") + (why.empty() ? "" : ",\"inadmissible\":" + Json(why)) +
                         ",\"accel_rows\":" + std::to_string(a.size()) +
                         ",\"query_rows\":" + std::to_string(q.size()) + ",\"accel\":" + RowsJson(a) +
                         ",\"query\":" + RowsJson(q) + "}");
  }
  // An accelerator call that answered with the SQL fallback, and why (one example per call).
  void NoteFallback(const std::string &where, const std::string &tuple, const std::string &kind,
                    const std::string &reason) {
    ++fallback_kinds[kind];
    if (fallback_examples.size() < 64)
      fallback_examples.push_back("{\"at\":" + Json(where) + ",\"params\":" + Json(tuple) +
                                  ",\"kind\":" + Json(kind) + ",\"reason\":" + Json(reason) + "}");
  }
  static std::string Array(const std::vector<std::string> &items) {
    std::string o = "[";
    for (std::size_t i = 0; i < items.size(); ++i)
      o += (i ? "," : "") + items[i];
    return o + "]";
  }
  std::string Fields() const {
    std::string ex = "[";
    for (std::size_t i = 0; i < examples.size(); ++i)
      ex += (i ? "," : "") + examples[i];
    std::string kinds = "{";
    for (const auto &[k, n] : fallback_kinds)
      kinds += (kinds.size() > 1 ? "," : "") + Json(k) + ":" + std::to_string(n);
    return "\"fallback_kinds\":" + kinds + "},\"fallback_examples\":" + Array(fallback_examples) + ",\"reference_error_examples\":" +
           Array(reference_error_examples) + "," + "\"checks\":" + std::to_string(checks) + ",\"reference_errors\":" + std::to_string(reference_errors) +
           ",\"mismatches\":" + std::to_string(mismatches) +
           ",\"order_mismatches\":" + std::to_string(order_mismatches) + ",\"examples\":" + ex + "]";
  }
};

// Keep only the transactions named in the comma-separated `names` (the quick tier's pick).
inline void KeepTxns(Spec &spec, const std::string &names) {
  std::vector<std::string> wanted;
  std::stringstream in(names);
  for (std::string name; std::getline(in, name, ',');)
    wanted.push_back(name);
  Spec kept = spec;
  kept.txns.clear();
  kept.txn_names.clear();
  for (std::size_t i = 0; i < spec.txns.size(); ++i)
    if (std::find(wanted.begin(), wanted.end(), spec.txn_names[i]) != wanted.end()) {
      kept.txns.push_back(spec.txns[i]);
      kept.txn_names.push_back(spec.txn_names[i]);
    }
  spec = std::move(kept);
}

// The declared schema's constraints (e.g. ALTER TABLE ... SET NOT NULL) in the file named by `env`,
// applied before anything else, so a write that leaves the schema's world fails instead of producing
// data the spec says cannot exist.
inline std::string SchemaSql(const char *env) {
  const char *path = std::getenv(env);
  return path && *path && fs::exists(path) ? ReadFile(path) : std::string();
}

// The top-level comma-separated literals of a tuple (quotes and parentheses respected).
inline std::vector<std::string> SplitTuple(const std::string &tuple) {
  std::vector<std::string> out;
  int depth = 0;
  bool quote = false;
  std::string cur;
  for (std::size_t i = 0; i < tuple.size(); ++i) {
    const char ch = tuple[i];
    if (ch == '\'')
      quote = !quote;
    if (!quote) {
      depth += ch == '(' ? 1 : ch == ')' ? -1 : 0;
      if (ch == ',' && depth == 0) {
        out.push_back(cur);
        cur.clear();
        continue;
      }
    }
    cur += ch;
  }
  if (!cur.empty())
    out.push_back(cur);
  for (auto &item : out)
    item.erase(0, item.find_first_not_of(" \t"));
  return out;
}

// The first `n` literals of a tuple: the arguments of an accelerator that derives the query's
// trailing parameters (an interval's end) from the earlier ones.
inline std::string Project(const std::string &tuple, std::size_t n) {
  const auto items = SplitTuple(tuple);
  std::string out;
  for (std::size_t i = 0; i < n && i < items.size(); ++i)
    out += (i ? ", " : "") + items[i];
  return out;
}

// What one write cost: the commit (statements, capture, maintenance run in it) and the first accelerated read
// after it, which waits for any maintenance the commit left pending; maint_ms is the accelerator's own share of
// both (-1: the engine does not report it), without the engine's cost of executing and committing the SQL.
struct WriteCost {
  std::string txn;
  double write_ms = 0, read_ms = 0, maint_ms = -1;
  std::string action; // the maintenance action the module took (ApplyDelta, RebuildPartition, RebuildAll, NoOp); empty: not observable
};

// Seconds of each phase of a harness run, in order (the report's `phases`), printed as they end.
inline std::vector<std::pair<std::string, double>> &PhaseSeconds() {
  static std::vector<std::pair<std::string, double>> phases;
  return phases;
}

struct Phase {
  explicit Phase(std::string name_p) : name(std::move(name_p)) {}
  ~Phase() {
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    PhaseSeconds().emplace_back(name, seconds);
    std::cerr << "phase " << name << ": " << seconds << " s\n";
  }
  std::string name;
  std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
};

inline std::string PhaseFields() {
  std::string out = "\"phases\":{";
  for (const auto &[name, seconds] : PhaseSeconds())
    out += (out.size() > 10 ? "," : "") + Json(name) + ":" + std::to_string(seconds);
  return out + "}";
}

// Milliseconds of accelerator maintenance of each adversarial-phase probe (eval/harness/differential.cpp), for the
// suite report.
inline std::vector<double> &ProbeCostsMs() {
  static std::vector<double> costs;
  return costs;
}

struct DifferentialRun {
  int write_errors = 0;
  std::string write_error_examples = "[";
  std::map<std::string, int> actions; // staged maintenance action -> count
  std::vector<WriteCost> costs;

  // `"write_costs":[...],"probe_costs_ms":[...]`: the cost model of quick_check projects these to the full dataset.
  std::string CostFields() const {
    std::string out = "\"write_costs\":[";
    for (std::size_t i = 0; i < costs.size(); ++i)
      out += (i ? "," : "") + std::string("{\"txn\":") + Json(costs[i].txn) + ",\"write_ms\":" +
             std::to_string(costs[i].write_ms) + ",\"read_ms\":" + std::to_string(costs[i].read_ms) +
             ",\"maint_ms\":" + std::to_string(costs[i].maint_ms) +
             (costs[i].action.empty() ? "" : ",\"action\":" + Json(costs[i].action)) + "}";
    out += "],\"probe_costs_ms\":[";
    for (std::size_t i = 0; i < ProbeCostsMs().size(); ++i)
      out += (i ? "," : "") + std::to_string(ProbeCostsMs()[i]);
    return out + "]";
  }
};

// A transaction whose first line is `-- rollback` is applied and then aborted: nothing may change.
inline bool IsRollbackTxn(const std::string &sql) {
  const auto b = sql.find_first_not_of(" \t\r\n");
  return b != std::string::npos && sql.size() >= b + 11 && std::equal(sql.begin() + b, sql.begin() + b + 11, "-- rollback",
                                                                    [](char x, char y) { return std::tolower(x) == y; });
}

// The differential every accelerator is held to: after the build and after every transaction (applied by
// `apply`, committed), the accelerator's answer equals the query's at the same snapshot, for every tuple
// every `every` transactions and at the end, two random ones otherwise.
inline DifferentialRun RunDifferentialLoop(
    Tally &tally, const Spec &spec, const std::string &query_sql, const Ordering &ordering, duckdb::Connection &qc,
    int every, uint64_t seed, const std::function<Rows(const std::string &, const std::string &)> &accel,
    const std::function<void(const std::string &)> &apply,
    const std::function<void(const std::string &)> &abort = {},
    const std::function<double()> &maintenance_us = {},   // cumulative accelerator maintenance (apply + build) time
    const std::function<std::string()> &last_action = {}) { // the maintenance action the last write took
  DifferentialRun run;
  std::mt19937_64 rng(seed);
  using Clock = std::chrono::steady_clock;
  const auto since_ms = [](Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); };
  constexpr auto kNone = static_cast<std::size_t>(-1);
  std::size_t unread = kNone; // the write whose first read is still to be timed
  double maintenance_before = 0;
  auto timed_accel = [&](const std::string &where, const std::string &t) {
    const auto started = Clock::now();
    auto rows = accel(where, t);
    if (unread != kNone) {
      auto &cost = run.costs[std::exchange(unread, kNone)];
      cost.read_ms = since_ms(started);
      if (maintenance_us)
        cost.maint_ms = (maintenance_us() - maintenance_before) / 1000.0;
    }
    return rows;
  };
  auto check = [&](const std::string &where, const std::string &t) {
    tally.CheckCall(where, t, [&] { return timed_accel(where, t); }, [&] { return RunQuery(qc, query_sql, t); }, ordering,
                    [&] { return RunQuery(qc, ordering.unlimited_sql, t); },
                    [&] { return RunProbe(qc, ordering.bounds_sql, t); });
  };
  auto check_all = [&](const std::string &where) {
    for (const auto &t : spec.params)
      check(where, t);
  };
  check_all("build");
  for (std::size_t i = 0; i < spec.txns.size(); ++i) {
    core::ClearMaintenanceDecisionLog();
    unread = kNone;
    const auto write_started = Clock::now();
    if (maintenance_us)
      maintenance_before = maintenance_us();
    try {
      if (IsRollbackTxn(spec.txns[i])) {
        Require(static_cast<bool>(abort), "the suite has a rollback transaction and the harness cannot abort one");
        abort(spec.txns[i]);
      } else {
        apply(spec.txns[i]);
      }
    } catch (const std::exception &e) {
      // the suite is validated on plain SQL: a write the accelerated path cannot apply is a failure
      if (++run.write_errors <= 3)
        run.write_error_examples += std::string(run.write_errors > 1 ? "," : "") + "{\"txn\":" +
                                    Json(spec.txn_names[i]) + ",\"error\":" + Json(std::string(e.what()).substr(0, 600)) + "}";
      continue;
    }
    run.costs.push_back({spec.txn_names[i], since_ms(write_started), 0});
    unread = run.costs.size() - 1;
    const auto d = core::LastMaintenanceDecision();
    run.actions[d ? std::to_string(static_cast<int>(d->staged_action)) : "none"]++;
    if (last_action)
      run.costs.back().action = last_action();
    const std::string where = "txn " + std::to_string(i) + " (" + spec.txn_names[i] + ")";
    if (every > 0 && (i + 1) % static_cast<std::size_t>(every) == 0) {
      check_all(where);
    } else {
      for (int k = 0; k < 2; ++k)
        check(where, spec.params[rng() % spec.params.size()]);
    }
  }
  check_all("end");
  return run;
}

// The spec runs on the database: no accelerator needed (the suite is written before the accelerator).
// Setup runs, every tuple evaluates, and every transaction applies, rolls back to the state before it,
// and applies again in sequence (committed).  Per transaction: how many tuples' answers it changes
// (a transaction that changes none can catch no stale maintenance).
inline int Validate(const std::string &path, const Spec &spec, const std::string &query_sql,
                    const std::string &schema_sql, std::ostream &out = std::cout) {
  duckdb::DuckDB db(path);
  duckdb::Connection c(db);
  std::vector<std::string> errors;
  auto run = [&](const std::string &what, const std::string &sql) {
    auto r = c.Query(sql);
    if (!r || r->HasError())
      errors.push_back(what + ": " + (r ? r->GetError() : std::string("failed")));
  };
  if (!schema_sql.empty())
    run("schema constraints", schema_sql);
  if (!spec.setup.empty())
    run("setup.sql", spec.setup);
  const auto ordering = MakeOrdering(c, query_sql, spec.params);
  // the same answer, up to the order and LIMIT cut that ties leave open
  // the bounds query (VERUS_KIT_BOUNDS_SQL) must admit the reference's own answer
  const char *bounds_file = std::getenv("VERUS_KIT_BOUNDS_SQL");
  const std::string bounds_sql = bounds_file && *bounds_file ? ReadFile(bounds_file) : std::string();
  // (with rounded DOUBLE aggregates the reference itself varies from run to run: an admissible answer counts as the same)
  auto same = [&](std::size_t k, const Rows &a, const Rows &b) {
    if (CompareOrdered(ordering, a, b, [&] { return RunQuery(c, ordering.unlimited_sql, spec.params[k]); }) == 0)
      return true;
    return !bounds_sql.empty() && float_bounds::Judge(query_sql, RunProbe(c, bounds_sql, spec.params[k]), a).empty();
  };
  auto answers = [&](std::vector<Rows> &out) {
    out.clear();
    for (const auto &t : spec.params) {
      try {
        out.push_back(RunQuery(c, query_sql, t));
      } catch (const std::exception &) {
        out.emplace_back(); // reported once, below
      }
    }
  };
  int nonempty = 0;
  std::string tuple_rows = "[";
  std::vector<Rows> before;
  for (const auto &t : spec.params) {
    try {
      const auto rows = RunQuery(c, query_sql, t);
      if (!bounds_sql.empty())
        if (const auto why = float_bounds::Judge(query_sql, RunProbe(c, bounds_sql, t), rows); !why.empty())
          errors.push_back("params " + t + ": the reference answer is not admissible by the bounds query: " + why);
      nonempty += !rows.empty();
      tuple_rows += std::string(before.empty() ? "" : ",") + std::to_string(rows.size());
      before.push_back(rows);
    } catch (const std::exception &e) {
      errors.push_back("params " + t + ": " + e.what());
      tuple_rows += std::string(before.empty() ? "" : ",") + "-1";
      before.emplace_back();
    }
  }
  int txn_errors = 0;
  std::string effects = "[";
  for (std::size_t i = 0; i < spec.txns.size(); ++i) {
    // rolled back: applied inside a transaction and undone
    c.BeginTransaction();
    auto r = c.Query(spec.txns[i]);
    const bool applies = r && !r->HasError();
    c.Rollback();
    std::string problem = applies ? "" : (r ? r->GetError() : std::string("failed"));
    std::vector<Rows> now;
    if (applies) {
      answers(now);
      for (std::size_t k = 0; k < now.size() && problem.empty(); ++k)
        if (!same(k, now[k], before[k]))
          problem = "the answer to " + spec.params[k] + " differs after the rollback: " + RowsJson(now[k], 2) +
                    " vs " + RowsJson(before[k], 2);
    }
    // committed, in sequence: a transaction is checked on the state the ones before it leave (a subquery
    // that finds a row on the original data may find none later)
    int changed = 0;
    if (problem.empty() && !IsRollbackTxn(spec.txns[i])) {
      c.BeginTransaction();
      auto applied = c.Query(spec.txns[i]);
      if (!applied || applied->HasError()) {
        problem = applied ? applied->GetError() : std::string("failed");
        c.Rollback();
      } else {
        c.Commit();
        answers(now);
        for (std::size_t k = 0; k < now.size(); ++k)
          changed += !same(k, now[k], before[k]);
        before = now;
      }
    }
    if (!problem.empty()) {
      ++txn_errors;
      if (errors.size() < 20)
        errors.push_back("txn " + std::to_string(i) + " (" + spec.txn_names[i] + "): " + problem);
    }
    effects += std::string(i ? "," : "") + "{\"txn\":" + Json(spec.txn_names[i]) + ",\"changes_answers_of\":" +
               std::to_string(changed) + "}";
  }
  std::string ej = "[";
  for (std::size_t i = 0; i < errors.size(); ++i)
    ej += (i ? "," : "") + Json(errors[i]);
  out << "{\"mode\":\"validate\",\"params\":" << spec.params.size() << ",\"params_nonempty\":" << nonempty << ",\"tuple_rows\":" << tuple_rows << "]"
            << ",\"txns\":" << spec.txns.size() << ",\"bench_txns\":" << spec.bench_txns.size()
            << ",\"txn_errors\":" << txn_errors << ",\"txn_effects\":" << effects << "],\"errors\":" << ej << "]}"
            << std::endl;
  return errors.empty() ? 0 : 1;
}

} // namespace accelerator_codegen::suite
