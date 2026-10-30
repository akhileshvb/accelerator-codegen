// Test runner of Verus accelerators (verus_kit.hpp) on Postgres: the counterpart of
// eval/tests/verus_kit/verus_kit_runner.cpp, over libpq against a server with the extension installed.
// Query-agnostic: the query from a file, the tests from a spec directory.
//
//   test_postgres_verus_kit validate     <spec_dir> <query_sql_file>
//   test_postgres_verus_kit differential <accel_id> <spec_dir> <query_sql_file> [every=10]
//   test_postgres_verus_kit snapshots    <accel_id> <spec_dir> <query_sql_file> [writes=200] [readers=4] [seed=1] [rr|rc]
//                                        [commit|abort|rebuild|truncate|drop|kill|workerkill]
//
// <query_sql_file>: the query with $n parameters (workloads/<bench>/q<N>.sql).  Each run works on a copy of the connection's database (CREATE DATABASE ... TEMPLATE), dropped at the
// end: the spec's setup.sql and transactions change data.  The spec is DuckDB SQL (eval/harness/suite.hpp):
// `SELECT * REPLACE (e AS c) FROM t` and `range(n)` are rewritten for Postgres.
// differential: after the build and after every committed transaction, the accelerator
//   (accel_<id>(...)) is compared with the query at the same snapshot.
// snapshots: rr (default): readers in REPEATABLE READ transactions run the accelerator, the query, the
//   accelerator again while a writer commits; all three must agree.  rc: READ COMMITTED readers run
//   both answers in one statement (one snapshot), compared as in differential and rr (ties, LIMIT cuts, bounds).
// The writer of snapshots: commit (the spec's transactions), abort (every third one rolled back, every
//   third one a savepoint rolled back to before its commit), rebuild (every 15th write deletes a table of the
//   query and reloads it), kill (its backend is terminated at random moments, mid-commit among them; it
//   reconnects), vacuum (VACUUM of the query's tables after every write).  Readers must see 0 mismatches
//   and 0 fallbacks.  truncate (as rebuild, with TRUNCATE, which is not MVCC-safe), drop (every 5th commit
//   message is not shipped, accel.fault_drop_commit_every: the worker must rebuild) and workerkill (the
//   shard worker is terminated every few seconds) only require 0 wrong answers: reads may be answered
//   from SQL.  Every run ends by checking that the stopped workers exit.
// VERUS_KIT_BOUNDS_SQL=<file>: the query's bounds query (eval/harness/float_bounds.hpp), as for DuckDB.
// A call the accelerator answered from SQL counts as a fallback (reported, and a failure of the run).
// Connection: libpq's PG* environment, database TPCH_PG_DB (host.sh's) or PGDATABASE.
#include <libpq-fe.h>

#include "eval/harness/suite.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <map>
#include <memory>
#include <random>
#include <regex>
#include <thread>
#include <unistd.h>

namespace accelerator_codegen::suite {
namespace {

struct PgResult {
  std::vector<std::string> names;
  Rows rows;
};

class PgConn {
public:
  explicit PgConn(const std::string &database) {
    const std::string info = "dbname=" + database;
    conn = PQconnectdb(info.c_str());
    Require(PQstatus(conn) == CONNECTION_OK, std::string("connect: ") + PQerrorMessage(conn));
  }
  ~PgConn() { PQfinish(conn); }
  PgConn(const PgConn &) = delete;

  PgResult Exec(const std::string &sql) { return Collect(PQexec(conn, sql.c_str()), sql); }

  // `sql` with $n bound to the values of the SQL expression list `tuple`, typed as Postgres types them.
  PgResult Run(const std::string &sql, const std::string &tuple) {
    if (tuple.empty())
      return Exec(sql);
    PGresult *evaluated = PQexec(conn, ("SELECT " + tuple).c_str());
    Require(PQresultStatus(evaluated) == PGRES_TUPLES_OK, std::string(PQresultErrorMessage(evaluated)) + tuple);
    const int n = PQnfields(evaluated);
    std::vector<Oid> types;
    std::vector<std::string> texts(n);
    std::vector<const char *> values;
    for (int i = 0; i < n; ++i) {
      types.push_back(PQftype(evaluated, i));
      if (!PQgetisnull(evaluated, 0, i))
        texts[i] = PQgetvalue(evaluated, 0, i);
      values.push_back(PQgetisnull(evaluated, 0, i) ? nullptr : texts[i].c_str());
    }
    PQclear(evaluated);
    return Collect(PQexecParams(conn, sql.c_str(), n, types.data(), values.data(), nullptr, nullptr, 0), sql);
  }

  // The number of this session's accelerator reads answered from SQL.
  long Fallbacks() {
    const auto counts = Exec("SELECT accel_get_execute_counts()").rows.at(0).at(0);
    const auto at = counts.find("fallback=");
    return at == std::string::npos ? 0 : std::atol(counts.c_str() + at + 9);
  }

  // This session's fallback causes: name -> count ("reader:", "worker:", runtime and kit reasons).
  std::map<std::string, long> Causes() {
    std::map<std::string, long> out;
    const auto text = Exec("SELECT accel_fallback_causes()").rows.at(0).at(0);
    for (std::size_t i = 0; i < text.size();) {
      const auto end = std::min(text.find(',', i), text.size()), eq = text.rfind('=', end);
      out[text.substr(i, eq - i)] = std::atol(text.c_str() + eq + 1);
      i = end + 1;
    }
    return out;
  }
  void ResetCounts() { Exec("SELECT accel_reset_execute_counts()"); }

private:
  static PgResult Collect(PGresult *r, const std::string &sql) {
    const auto status = PQresultStatus(r);
    if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK) {
      const std::string message = PQresultErrorMessage(r);
      PQclear(r);
      throw std::runtime_error(message + " [" + sql.substr(0, 200) + "]");
    }
    PgResult out;
    for (int j = 0; j < PQnfields(r); ++j)
      out.names.push_back(PQfname(r, j));
    for (int i = 0; i < PQntuples(r); ++i) {
      std::vector<std::string> row;
      for (int j = 0; j < PQnfields(r); ++j)
        row.push_back(PQgetisnull(r, i, j) ? "NULL" : PQgetvalue(r, i, j));
      out.rows.push_back(std::move(row));
    }
    PQclear(r);
    return out;
  }

  PGconn *conn;
};

Rows RunQuery(PgConn &c, const std::string &query_sql, const std::string &tuple) { return c.Run(query_sql, tuple).rows; }

float_bounds::Probe RunProbe(PgConn &c, const std::string &bounds_sql, const std::string &tuple) {
  auto r = c.Run(bounds_sql, tuple);
  for (std::size_t j = 0; j < r.names.size(); ++j) // the judge reads booleans as DuckDB prints them
    if (r.names[j] == "__may" || r.names[j] == "__must")
      for (auto &row : r.rows)
        row[j] = row[j] == "t" ? "true" : row[j] == "f" ? "false" : row[j];
  return {r.names, r.rows};
}

// The bounds queries' helpers, in numeric so that they cannot overflow: float8 arithmetic and SUM raise on
// overflow where DuckDB's round to inf, and groups of 1e308-sized values sum past the double range.  fsum
// is an exact sum of the DOUBLEs' shortest decimal forms (within the bound's slack of their exact sum), and
// the bounds query's SUM is rewritten to it; fl_delta is the same formula as DuckDB's macro, clamped to 0
// where float8 would underflow.
constexpr const char *kBoundsPrelude = R"SQL(
CREATE OR REPLACE FUNCTION fl_delta(m numeric, a numeric) RETURNS numeric
  LANGUAGE sql IMMUTABLE AS $$
    SELECT CASE WHEN r < 2.2250738585072014e-308 THEN 0 ELSE r END
    FROM (SELECT a * ((m * 1.1102230246251565e-16) / (1 - m * 1.1102230246251565e-16) + 4.4408920985006262e-16) AS r) t $$;
CREATE OR REPLACE FUNCTION fsum_step(s numeric, v double precision) RETURNS numeric
  LANGUAGE sql IMMUTABLE AS 'SELECT s + v::text::numeric';
CREATE OR REPLACE FUNCTION fsum_step(s numeric, v numeric) RETURNS numeric
  LANGUAGE sql IMMUTABLE AS 'SELECT s + v';
CREATE OR REPLACE AGGREGATE fsum(double precision) (SFUNC = fsum_step, STYPE = numeric, INITCOND = '0');
CREATE OR REPLACE AGGREGATE fsum(numeric) (SFUNC = fsum_step, STYPE = numeric, INITCOND = '0');
)SQL";

// DuckDB SQL of the spec -> Postgres.
class Dialect {
public:
  explicit Dialect(PgConn &c) : conn(c) {}

  std::string operator()(std::string sql) {
    sql = std::regex_replace(sql, std::regex("//"), "/");
    sql = std::regex_replace(sql, std::regex(R"(::DOUBLE\b)", std::regex::icase), "::DOUBLE PRECISION");
    sql = std::regex_replace(sql, std::regex(R"(\bhash\()", std::regex::icase), "hashtext(");
    sql = std::regex_replace(sql, std::regex(R"(\bmedian\(([^()]*)\))", std::regex::icase),
                             "percentile_cont(0.5) WITHIN GROUP (ORDER BY $1)");
    sql = Printf(sql);
    // DuckDB's physical row number: a number that grows with the tuple's position
    sql = std::regex_replace(sql, std::regex(R"(\browid\b)", std::regex::icase),
                             "((ctid::text::point)[0]::bigint * 65536 + (ctid::text::point)[1]::bigint)");
    sql = Ranges(Lists(sql));
    static const std::regex star(R"(SELECT\s+\*\s+REPLACE\s*\()", std::regex::icase);
    for (std::smatch m; std::regex_search(sql, m, star);) {
      const auto open = m.position(0) + m.length(0) - 1;
      const auto close = Matching(sql, open);
      const std::regex from(R"(^\s*FROM\s+(\w+))", std::regex::icase);
      std::smatch f;
      const std::string tail = sql.substr(close + 1);
      Require(std::regex_search(tail, f, from), "SELECT * REPLACE without FROM <table>");
      std::map<std::string, std::string> replaced;
      for (const auto &item : Split(sql.substr(open + 1, close - open - 1))) {
        const auto as = item.rfind(" AS ");
        Require(as != std::string::npos, "REPLACE item without AS: " + item);
        replaced[Trim(item.substr(as + 4))] = item.substr(0, as);
      }
      std::string columns;
      for (const auto &column : Columns(f[1])) {
        const auto it = replaced.find(column);
        columns += (columns.empty() ? "" : ", ") + (it == replaced.end() ? column : "(" + it->second + ") AS " + column);
      }
      sql = sql.substr(0, m.position(0)) + "SELECT " + columns + sql.substr(close + 1);
    }
    return sql;
  }

private:
  // printf('%0Nd', e) -> lpad((e)::text, N, '0')
  static std::string Printf(std::string sql) {
    static const std::regex printf(R"(printf\s*\()", std::regex::icase);
    for (std::smatch m; std::regex_search(sql, m, printf);) {
      const std::size_t open = m.position(0) + m.length(0) - 1, close = Matching(sql, open);
      const auto args = Split(sql.substr(open + 1, close - open - 1));
      std::smatch width;
      const std::string format = Trim(args[0]);
      Require(args.size() == 2 && std::regex_match(format, width, std::regex(R"('%0(\d+)d')")),
              "printf form not supported: " + sql.substr(open, close - open + 1));
      sql.replace(m.position(0), close + 1 - m.position(0), "lpad((" + args[1] + ")::text, " + width[1].str() + ", '0')");
    }
    return sql;
  }
  // range(n), range(a, b), range(a, b, step): generate_series over [a, b).
  static std::string Ranges(std::string sql) {
    static const std::regex range(R"(\brange\(([^()]*)\))", std::regex::icase);
    for (std::smatch m; std::regex_search(sql, m, range);) {
      auto args = Split(m[1]);
      for (auto &a : args)
        a = Trim(a);
      const std::string from = args.size() == 1 ? "0" : args[0], to = args.size() == 1 ? args[0] : args[1];
      sql.replace(m.position(0), m.length(0),
                  "generate_series(" + from + ", (" + to + ") - 1" + (args.size() > 2 ? ", " + args[2] : "") + ")");
    }
    return sql;
  }
  // A list literal [a, b][i] -> (ARRAY[a, b])[i]; a `[` after an identifier or `]` is an index.
  static std::string Lists(const std::string &sql) {
    std::string out;
    bool quote = false;
    for (std::size_t i = 0; i < sql.size(); ++i) {
      const char c = sql[i];
      if (c == '\'')
        quote = !quote;
      std::size_t prev = out.find_last_not_of(" \t\r\n");
      const bool index = prev != std::string::npos && (std::isalnum(static_cast<unsigned char>(out[prev])) ||
                                                       out[prev] == '_' || out[prev] == ']' || out[prev] == ')' || out[prev] == '"');
      if (c != '[' || quote || index) {
        out += c;
        continue;
      }
      const auto close = sql.find(']', i);
      Require(close != std::string::npos, "unclosed [");
      out += "(ARRAY[" + Lists(sql.substr(i + 1, close - i - 1)) + "])";
      i = close;
    }
    return out;
  }
  static std::size_t Matching(const std::string &s, std::size_t open) {
    int depth = 0;
    bool quote = false;
    for (std::size_t i = open; i < s.size(); ++i) {
      if (s[i] == '\'')
        quote = !quote;
      if (quote)
        continue;
      depth += s[i] == '(' ? 1 : s[i] == ')' ? -1 : 0;
      if (depth == 0)
        return i;
    }
    throw std::runtime_error("unbalanced parentheses");
  }
  static std::vector<std::string> Split(const std::string &s) {
    std::vector<std::string> out;
    int depth = 0;
    bool quote = false;
    std::string item;
    for (char c : s) {
      if (c == '\'')
        quote = !quote;
      if (!quote)
        depth += c == '(' ? 1 : c == ')' ? -1 : 0;
      if (c == ',' && depth == 0 && !quote) {
        out.push_back(item);
        item.clear();
      } else {
        item += c;
      }
    }
    out.push_back(item);
    return out;
  }
  static std::string Trim(const std::string &s) {
    const auto b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? "" : s.substr(b, e - b + 1);
  }
  const std::vector<std::string> &Columns(const std::string &table) {
    auto &cached = columns[table];
    if (cached.empty())
      for (const auto &row : conn.Exec("SELECT column_name FROM information_schema.columns WHERE table_name = '" + table +
                                       "' AND table_schema = 'public' ORDER BY ordinal_position").rows)
        cached.push_back(row[0]);
    Require(!cached.empty(), "no table " + table);
    return cached;
  }
  PgConn &conn;
  std::map<std::string, std::vector<std::string>> columns;
};

// A copy of the connection's database for one run, dropped at the end.
struct ScratchDatabase {
  ScratchDatabase() {
    const char *base = std::getenv("TPCH_PG_DB") ? std::getenv("TPCH_PG_DB") : std::getenv("PGDATABASE");
    Require(base != nullptr, "set TPCH_PG_DB (or PGDATABASE)");
    name = std::string(base) + "_run" + std::to_string(getpid());
    PgConn admin("postgres");
    admin.Exec("DROP DATABASE IF EXISTS " + name);
    admin.Exec("CREATE DATABASE " + name + " TEMPLATE " + base);
  }
  ~ScratchDatabase() {
    try {
      PgConn run(name);
      run.Exec("SELECT accel_stop_shard_workers()"); // the workers hold the database
    } catch (const std::exception &) {
    }
    try {
      PgConn admin("postgres");
      admin.Exec("DROP DATABASE IF EXISTS " + name + " WITH (FORCE)");
    } catch (const std::exception &e) {
      std::cerr << "drop " << name << ": " << e.what() << "\n";
    }
  }
  std::string name;
};

struct Setup {
  std::string query_sql;
  Ordering ordering;
};

void Apply(PgConn &c, Dialect &dialect, const std::string &sql, bool rollback) {
  c.Exec("BEGIN; " + dialect(sql) + "\n; " + (rollback ? "ROLLBACK" : "COMMIT"));
}

Setup Prepare(PgConn &c, Dialect &dialect, const std::string &query, const Spec &spec) {
  Setup s{query, {}};
  c.Exec(kBoundsPrelude);
  if (!spec.setup.empty())
    c.Exec(dialect(spec.setup));
  s.ordering = MakeOrdering(query, [&] {
    for (const auto &tuple : spec.params)
      try {
        return c.Run(query, tuple).names;
      } catch (const std::exception &) {
      }
    return std::vector<std::string>{};
  });
  if (const char *bounds = std::getenv("VERUS_KIT_BOUNDS_SQL"); bounds && *bounds) {
    s.ordering.bounds_sql = std::regex_replace(ReadFile(bounds), std::regex(R"(\bSUM\()", std::regex::icase), "fsum(");
    s.ordering.query_sql = query;
  }
  return s;
}

// The accelerator's rows; a call answered from SQL is noted as a fallback.
Rows RunAccel(Tally &tally, std::mutex &tally_mutex, PgConn &c, const std::string &id, const std::string &where,
              const std::string &tuple) {
  const long before = c.Fallbacks();
  auto rows = c.Exec("SELECT * FROM accel_" + id + "(" + tuple + ")").rows;
  if (c.Fallbacks() > before) {
    std::lock_guard<std::mutex> lock(tally_mutex);
    tally.NoteFallback(where, tuple, "fallback", "answered from SQL");
  }
  return rows;
}

// The shard worker's counter `name` of "accel_shard_stats()".
long ShardStat(PgConn &c, const std::string &name) {
  const auto text = c.Exec("SELECT accel_shard_stats()").rows.at(0).at(0);
  const auto at = text.find(name + "=");
  return at == std::string::npos ? -1 : std::atol(text.c_str() + at + name.size() + 1);
}

// The first build of the state runs in the shard worker, for every member of its shard: the test starts
// once all are built.  A read before that falls back or waits, and commits made meanwhile are applied
// together afterwards (no reader can be served between them).
void Warm(PgConn &c, const std::string &id, const std::string &tuple) {
  for (int tries = 0; tries < 1800; ++tries) {
    const long before = c.Fallbacks();
    c.Exec("SELECT * FROM accel_" + id + "(" + tuple + ")");
    const long members = ShardStat(c, "members");
    if (c.Fallbacks() == before && members > 0 && ShardStat(c, "synced") == members)
      return;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  throw std::runtime_error("the accelerator " + id + " did not build in 180 s");
}

// The answers of one statement, hence of one snapshot even at READ COMMITTED: the accelerator's rows, the
// query's, and, when the ordering asks for them, the query's rows without its LIMIT and the bounds query's.
struct Together {
  Rows accel, query, full;
  float_bounds::Probe probe;
};

// `sql` with its $n replaced by the tuple's expressions.
std::string Inlined(std::string sql, const std::string &tuple) {
  const auto items = SplitTuple(tuple);
  for (std::size_t i = items.size(); i > 0; --i)
    sql = std::regex_replace(sql, std::regex("\\$" + std::to_string(i) + "(?![0-9])"), "(" + items[i - 1] + ")");
  while (!sql.empty() && (sql.back() == ';' || std::isspace(static_cast<unsigned char>(sql.back()))))
    sql.pop_back();
  return sql;
}

// A text array literal "{a,"b c",...}" as its elements.
std::vector<std::string> ArrayCells(const std::string &text) {
  std::vector<std::string> cells;
  for (std::size_t i = 1; i < text.size() && text[i] != '}';) {
    std::string cell;
    if (text[i] == '"') {
      for (++i; text[i] != '"'; ++i)
        cell += text[i] == '\\' ? text[++i] : text[i];
      ++i;
    } else {
      for (; text[i] != ',' && text[i] != '}'; ++i)
        cell += text[i];
    }
    cells.push_back(cell);
    ++i; // , or }
  }
  return cells;
}

Together RunTogether(PgConn &c, const std::string &id, const std::string &query, const std::string &tuple, const Ordering &ordering) {
  const char *cells = "ARRAY(SELECT value FROM json_each_text(row_to_json(t)))::text";
  const char *names = "ARRAY(SELECT key FROM json_each_text(row_to_json(t)))::text";
  const auto branch = [&](const char *tag, const std::string &from, bool with_names) {
    return std::string(" UNION ALL SELECT '") + tag + "', " + cells + ", " + (with_names ? names : "''") + " FROM (" + from + ") t";
  };
  std::string sql = std::string("SELECT 'a', ") + cells + ", '' FROM accel_" + id + "(" + tuple + ") t" +
                    branch("q", Inlined(query, tuple), false);
  if (ordering.limited)
    sql += branch("u", Inlined(ordering.unlimited_sql, tuple), false);
  if (!ordering.bounds_sql.empty()) { // the names come from a row of NULLs: the bounds query may have no rows
    const std::string bounds = Inlined(ordering.bounds_sql, tuple);
    sql += branch("p", bounds, false) + branch("n", "SELECT b.* FROM (SELECT 1) one LEFT JOIN (" + bounds + ") b ON false", true);
  }
  Together out;
  for (const auto &r : c.Exec(sql).rows) {
    const auto row = ArrayCells(r[1]);
    switch (r[0][0]) {
    case 'a': out.accel.push_back(row); break;
    case 'q': out.query.push_back(row); break;
    case 'u': out.full.push_back(row); break;
    case 'n': out.probe.names = ArrayCells(r[2]); break;
    default: out.probe.rows.push_back(row);
    }
  }
  return out;
}

 // The sessions' fallback causes, summed.  Reads of the transaction that wrote the captured tables
// ("reader:own_writes") are the one allowed cause: counted apart from the rest.
struct CauseTotals {
  void Add(PgConn &c) {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto &[cause, n] : c.Causes())
      totals[cause] += n;
  }
  long Other() const {
    long n = 0;
    for (const auto &[cause, count] : totals)
      n += cause == "reader:own_writes" || cause.rfind("event:", 0) == 0 ? 0 : count;
    return n;
  }
  std::string Json() const {
    std::string out;
    for (const auto &[cause, n] : totals)
      out += (out.empty() ? "" : ",") + std::string("\"") + cause + "\":" + std::to_string(n);
    return "\"causes\":{" + out + "}";
  }
  std::mutex mutex;
  std::map<std::string, long> totals;
};

std::string Tail(PgConn &c, const Tally &tally, int fallbacks, const CauseTotals &causes) {
  return "\"fallbacks\":" + std::to_string(fallbacks) + "," + causes.Json() + ",\"shard\":" + Json(c.Exec("SELECT accel_shard_stats()").rows[0][0]) +
         "," + tally.Fields();
}

int Differential(const std::string &id, const Spec &spec, const std::string &query, int every) {
  ScratchDatabase scratch;
  PgConn writer(scratch.name), reader(scratch.name);
  Dialect dialect(writer);
  const auto s = Prepare(writer, dialect, query, spec);
  Warm(reader, id, spec.params.at(0));
  reader.ResetCounts();
  CauseTotals causes;
  Tally tally;
  std::mutex mutex;
  const auto run = RunDifferentialLoop(
      tally, spec, s.query_sql, s.ordering, reader, every, 7,
      [&](const std::string &where, const std::string &t) { return RunAccel(tally, mutex, reader, id, where, t); },
      [&](const std::string &sql) { Apply(writer, dialect, sql, false); },
      [&](const std::string &sql) { Apply(writer, dialect, sql, true); });
  causes.Add(reader);
  const int fallbacks = [&] {
    int n = 0;
    for (const auto &[kind, count] : tally.fallback_kinds)
      n += count;
    return n;
  }();
  std::cout << "{\"mode\":\"differential\",\"txns\":" << spec.txns.size() << ",\"write_errors\":" << run.write_errors
            << ",\"write_error_examples\":" << run.write_error_examples << "]," << Tail(reader, tally, fallbacks, causes) << "}"
            << std::endl;
  const bool compared = tally.checks > 0 && tally.reference_errors <= tally.checks;
  return compared && run.write_errors == 0 && tally.mismatches + tally.order_mismatches == 0 && fallbacks == 0 ? 0 : 1;
}

// The tables of the query: names after FROM / JOIN that the database has.
std::vector<std::string> QueryTables(PgConn &c, const std::string &query) {
  std::vector<std::string> out;
  static const std::regex from(R"(\b(?:FROM|JOIN)\s+([a-z_][a-z_0-9]*))", std::regex::icase);
  for (std::sregex_iterator it(query.begin(), query.end(), from), end; it != end; ++it) {
    const std::string name = (*it)[1];
    if (std::find(out.begin(), out.end(), name) == out.end() &&
        !c.Exec("SELECT 1 FROM information_schema.tables WHERE table_schema = 'public' AND table_name = '" + name + "'").rows.empty())
      out.push_back(name);
  }
  return out;
}

// One write of the writer, by mode (see the file header).
void WriteOnce(PgConn &w, Dialect &dialect, const std::string &mode, int i, const std::vector<std::string> &txns,
               const std::vector<std::string> &tables, std::mt19937_64 &rng) {
  const auto &a = txns[rng() % txns.size()], &b = txns[rng() % txns.size()];
  if (mode == "abort" && i % 3 == 1)
    return Apply(w, dialect, a, true);
  if (mode == "abort" && i % 3 == 2) {
    w.Exec("BEGIN; SAVEPOINT s; " + dialect(a) + "\n; ROLLBACK TO s; SAVEPOINT t; " + dialect(b) + "\n; RELEASE t; COMMIT");
    return;
  }
  if ((mode == "rebuild" || mode == "truncate") && i % 15 == 14 && !tables.empty()) {
    // rebuild empties the table with DELETE (MVCC: older snapshots keep their rows) and reloads it.  TRUNCATE is not
    // MVCC-safe, so truncate only checks that no answer is wrong: its lock queues behind readers' locks and the
    // worker's build scan queues behind it, while those readers wait for the worker; the lock timeout cancels the
    // TRUNCATE and the reload is tried again.
    const auto &t = tables[(i / 15) % tables.size()];
    const std::string empty = mode == "truncate" ? "TRUNCATE " : "DELETE FROM ";
    w.Exec("SET lock_timeout = '150ms'");
    for (int attempt = 0; attempt < 40; ++attempt) {
      try {
        w.Exec("BEGIN; CREATE TEMP TABLE reload ON COMMIT DROP AS SELECT * FROM " + t + "; " + empty + t + "; INSERT INTO " + t +
               " SELECT * FROM reload; COMMIT");
        break;
      } catch (const std::exception &e) {
        w.Exec("ROLLBACK");
        if (std::string(e.what()).find("lock timeout") == std::string::npos)
          throw;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    }
    w.Exec("RESET lock_timeout");
    return;
  }
  Apply(w, dialect, a, false);
  if (mode == "vacuum") // what the worker has not looked up yet must survive: its snapshots hold the horizon back
    for (const auto &t : tables)
      w.Exec("VACUUM " + t);
}

// Milliseconds of the operations a mode times, summarized as p50 / p99 / max.
struct Latencies {
  void Add(std::chrono::steady_clock::time_point since) {
    std::lock_guard<std::mutex> lock(mutex);
    ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since).count());
  }
  std::string Json() {
    std::lock_guard<std::mutex> lock(mutex);
    std::sort(ms.begin(), ms.end());
    const auto at = [&](double q) { return ms.empty() ? 0.0 : ms[static_cast<std::size_t>(q * (ms.size() - 1))]; };
    char text[160];
    std::snprintf(text, sizeof text, "{\"n\":%zu,\"p50\":%.2f,\"p99\":%.2f,\"max\":%.2f}", ms.size(), at(0.5), at(0.99), at(1));
    return text;
  }
  std::mutex mutex;
  std::vector<double> ms;
};

int Snapshots(const std::string &id, const Spec &spec, const std::string &query, int writes, int readers, uint64_t seed,
              bool read_committed, const std::string &mode) {
  Require(!spec.txns.empty(), "snapshots needs txns/");
  ScratchDatabase scratch;
  PgConn writer(scratch.name);
  Dialect dialect(writer);
  const auto s = Prepare(writer, dialect, query, spec);
  Warm(writer, id, spec.params.at(0));
  writer.ResetCounts();
  const auto tables = QueryTables(writer, query);
  auto session = std::make_unique<PgConn>(scratch.name); // the writes' session (kill replaces it)
  if (mode == "drop")
    session->Exec("SET accel.fault_drop_commit_every = 5");
  std::atomic<int> session_pid{std::atoi(session->Exec("SELECT pg_backend_pid()").rows[0][0].c_str())};
  CauseTotals causes;
  Tally tally;
  std::mutex mutex;
  std::atomic<bool> done{false};
  std::atomic<int> failures{0};
  Latencies write_latency, read_latency;
  std::vector<std::thread> threads;
  for (int t = 0; t < readers; ++t)
    threads.emplace_back([&, t] {
      std::mt19937_64 rng(seed * 1000003 + static_cast<uint64_t>(t));
      try {
        PgConn rc(scratch.name);
        const std::string pid = rc.Exec("SELECT pg_backend_pid()").rows[0][0];
        while (!done.load()) {
          const auto &p = spec.params[rng() % spec.params.size()];
          if (read_committed) { // one statement holds both answers
            const long before = rc.Fallbacks();
            const auto started = std::chrono::steady_clock::now();
            const auto together = RunTogether(rc, id, query, p, s.ordering);
            read_latency.Add(started);
            std::lock_guard<std::mutex> lock(mutex);
            if (rc.Fallbacks() > before)
              tally.NoteFallback("reader " + std::to_string(t), p, "fallback", "answered from SQL");
            tally.Check("reader " + std::to_string(t) + " rc", p, together.accel, together.query, s.ordering,
                        [&] { return together.full; }, [&] { return together.probe; });
            continue;
          }
          rc.Exec("BEGIN ISOLATION LEVEL REPEATABLE READ");
          const long fb0 = rc.Fallbacks();
          const auto started = std::chrono::steady_clock::now();
          const auto a = RunAccel(tally, mutex, rc, id, "reader " + std::to_string(t), p);
          read_latency.Add(started);
          std::this_thread::sleep_for(std::chrono::microseconds(rng() % 3000));
          Rows b;
          try {
            b = RunQuery(rc, s.query_sql, p);
          } catch (const std::exception &) {
            rc.Exec("ROLLBACK");
            std::lock_guard<std::mutex> lock(mutex);
            ++tally.reference_errors;
            continue;
          }
          std::this_thread::sleep_for(std::chrono::microseconds(rng() % 3000));
          const long fb1 = rc.Fallbacks();
          const auto c = RunAccel(tally, mutex, rc, id, "reader " + std::to_string(t), p);
          const long fb2 = rc.Fallbacks();
          const auto full = [&] { return RunQuery(rc, s.ordering.unlimited_sql, p); };
          const auto probe = [&] { return RunProbe(rc, s.ordering.bounds_sql, p); };
          std::lock_guard<std::mutex> lock(mutex);
          const std::string served = std::string(" [first ") + (fb1 > fb0 ? "sql" : "accel") + ", again " + (fb2 > fb1 ? "sql" : "accel") + "]";
          tally.Check("reader " + std::to_string(t) + " pid " + pid + " first" + served, p, a, b, s.ordering, full, probe);
          tally.Check("reader " + std::to_string(t) + " pid " + pid + " again" + served, p, c, b, s.ordering, full, probe);
          rc.Exec("COMMIT");
        }
        causes.Add(rc);
      } catch (const std::exception &e) {
        if (++failures <= 3)
          std::cerr << "reader " << t << ": " << e.what() << "\n";
      }
    });
  std::thread killer;
  if (mode == "kill" || mode == "workerkill")
    killer = std::thread([&] {
      std::mt19937_64 krng(seed ^ 0x5bd1e995);
      PgConn admin(scratch.name);
      while (!done.load()) {
        if (mode == "kill") {
          std::this_thread::sleep_for(std::chrono::microseconds(500 + krng() % 40000));
          admin.Exec("SELECT pg_terminate_backend(" + std::to_string(session_pid.load()) + ")");
        } else { // the shard worker, wherever it is in a pass; it restarts on demand
          std::this_thread::sleep_for(std::chrono::milliseconds(2000 + krng() % 4000));
          admin.Exec("SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE backend_type = 'accel shard worker' AND datname = '" + scratch.name + "'");
        }
      }
    });
  std::mt19937_64 rng(seed);
  int write_errors = 0, kills = 0;
  std::string write_error_example;
  for (int i = 0; i < writes; ++i) {
    try {
      const auto started = std::chrono::steady_clock::now();
      WriteOnce(*session, dialect, mode, i, spec.txns, tables, rng);
      write_latency.Add(started);
    } catch (const std::exception &e) { // random order: a transaction can legitimately fail (its rows gone)
      const std::string message = e.what();
      const bool terminated = message.find("terminating connection") != std::string::npos ||
                              message.find("server closed the connection") != std::string::npos ||
                              message.find("no connection to the server") != std::string::npos;
      if (terminated) {
        ++kills;
        session = std::make_unique<PgConn>(scratch.name);
        session_pid = std::atoi(session->Exec("SELECT pg_backend_pid()").rows[0][0].c_str());
      } else {
        try {
          session->Exec("ROLLBACK");
        } catch (const std::exception &) {
        }
        if (++write_errors == 1)
          write_error_example = message.substr(0, 600);
      }
    }
    std::this_thread::sleep_for(std::chrono::microseconds(rng() % 2000));
  }
  done = true;
  if (killer.joinable())
    killer.join();
  for (auto &th : threads)
    th.join();
  int fallbacks = 0;
  for (const auto &[kind, count] : tally.fallback_kinds)
    fallbacks += count;
  // Liveness: asked to stop, the shard workers exit (one stuck on a commit left pending holds the database).
  writer.Exec("SELECT accel_stop_shard_workers()");
  const auto workers = [&] {
    return std::atol(writer.Exec("SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'accel shard worker' AND datname = '" +
                                 scratch.name + "'").rows[0][0].c_str());
  };
  for (int tries = 0; tries < 300 && workers() > 0; ++tries)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const long lingering = workers();
  std::cout << "{\"mode\":\"snapshots\",\"writes\":" << writes << ",\"write_errors\":" << write_errors
            << ",\"write_error_example\":" << Json(write_error_example) << ",\"readers\":" << readers
            << ",\"writer\":" << Json(mode) << ",\"kills\":" << kills << ",\"lingering_workers\":" << lingering << ",\"reader_failures\":" << failures
            << ",\"write_ms\":" << write_latency.Json() << ",\"read_ms\":" << read_latency.Json() << ","
            << Tail(writer, tally, fallbacks, causes) << "}" << std::endl;
  const bool answers_from_sql_allowed = mode == "truncate" || mode == "drop" || mode == "workerkill";
  return tally.mismatches + tally.order_mismatches == 0 && failures == 0 && write_errors < writes && tally.checks > 0 &&
                 (fallbacks == 0 || answers_from_sql_allowed) && lingering == 0
             ? 0
             : 1;
}

// A transaction reading after its own writes to the captured tables is the one read the worker cannot
// serve: the accelerator answers from SQL (cause reader:own_writes), as the query itself would.
int OwnWrites(const std::string &id, const Spec &spec, const std::string &query) {
  Require(!spec.txns.empty(), "ownwrites needs txns/");
  ScratchDatabase scratch;
  PgConn c(scratch.name);
  Dialect dialect(c);
  const auto s = Prepare(c, dialect, query, spec);
  Warm(c, id, spec.params.at(0));
  c.ResetCounts();
  int unequal = 0, reads = 0;
  for (std::size_t i = 0; i < spec.txns.size() && i < 5; ++i) {
    c.Exec("BEGIN; " + dialect(spec.txns[i]));
    for (const auto &p : spec.params) {
      Rows a, b;
      try {
        b = RunQuery(c, s.query_sql, p);
      } catch (const std::exception &) {
        c.Exec("ROLLBACK; BEGIN; " + dialect(spec.txns[i]));
        continue;
      }
      const long before = c.Fallbacks();
      a = c.Run("SELECT * FROM accel_" + id + "(" + p + ")", "").rows;
      ++reads;
      if (c.Fallbacks() > before) // from SQL in this transaction: the query's own rows
        unequal += a != b;
    }
    c.Exec("ROLLBACK");
  }
  CauseTotals causes;
  causes.Add(c);
  const long own = causes.totals["reader:own_writes"], fallbacks = c.Fallbacks();
  std::cout << "{\"mode\":\"ownwrites\",\"reads\":" << reads << ",\"unequal\":" << unequal << ",\"fallbacks\":" << fallbacks
            << "," << causes.Json() << "}" << std::endl;
  return reads > 0 && unequal == 0 && own > 0 && own == fallbacks && causes.Other() == 0 ? 0 : 1;
}

// No accelerator: the spec runs on the database; every tuple evaluates, every transaction applies and rolls back.
int Validate(const Spec &spec, const std::string &query_sql) {
  ScratchDatabase scratch;
  PgConn c(scratch.name);
  Dialect dialect(c);
  std::vector<std::string> errors;
  try {
    if (!spec.setup.empty())
      c.Exec(dialect(spec.setup));
  } catch (const std::exception &e) {
    errors.push_back(std::string("setup: ") + e.what());
  }
  for (const auto &tuple : spec.params)
    try {
      c.Run(query_sql, tuple);
    } catch (const std::exception &e) {
      errors.push_back("tuple " + tuple + ": " + e.what());
    }
  for (std::size_t i = 0; i < spec.txns.size(); ++i)
    try {
      Apply(c, dialect, spec.txns[i], false);
    } catch (const std::exception &e) {
      c.Exec("ROLLBACK");
      errors.push_back(spec.txn_names[i] + ": " + e.what());
    }
  std::cout << "{\"mode\":\"validate\",\"errors\":[";
  for (std::size_t i = 0; i < errors.size(); ++i)
    std::cout << (i ? "," : "") << Json(errors[i].substr(0, 400));
  std::cout << "]}" << std::endl;
  return errors.empty() ? 0 : 1;
}

} // namespace
} // namespace accelerator_codegen::suite

int main(int argc, char **argv) {
  using namespace accelerator_codegen::suite;
  try {
    if (argc < 3)
      throw std::runtime_error("usage: test_postgres_verus_kit validate|differential|snapshots ... (see the file header)");
    const std::string mode = argv[1];
    if (mode == "validate") {
      Require(argc >= 4, "validate <spec_dir> <query_sql_file>");
      return Validate(LoadSpec(argv[2], "VERUS_KIT_PARAMS_FILE", "VERUS_KIT_EXTRA_PARAMS_FILE"), ReadFile(argv[3]));
    }
    Require(argc >= 5, mode + " <accel_id> <spec_dir> <query_sql_file> ...");
    const auto spec = LoadSpec(argv[3], "VERUS_KIT_PARAMS_FILE", "VERUS_KIT_EXTRA_PARAMS_FILE");
    const auto query = ReadFile(argv[4]);
    const auto arg = [&](int i, long fallback) { return argc > i ? std::atol(argv[i]) : fallback; };
    if (mode == "differential")
      return Differential(argv[2], spec, query, static_cast<int>(arg(5, 10)));
    if (mode == "ownwrites")
      return OwnWrites(argv[2], spec, query);
    if (mode == "snapshots")
      return Snapshots(argv[2], spec, query, static_cast<int>(arg(5, 200)), static_cast<int>(arg(6, 4)),
                       static_cast<uint64_t>(arg(7, 1)), argc > 8 && std::string(argv[8]) == "rc",
                       argc > 9 ? argv[9] : "commit");
    throw std::runtime_error("unknown mode " + mode);
  } catch (const std::exception &e) {
    std::cerr << "error: " << e.what() << "\n";
    return 2;
  }
}
