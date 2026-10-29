// Test runner of Verus accelerators (verus_kit.hpp) on Postgres: the counterpart of
// eval/tests/verus_kit/verus_kit_runner.cpp, over libpq against a server with the extension installed.
// Query-agnostic: the query from a file, the tests from a spec directory.
//
//   test_postgres_verus_kit validate     <spec_dir> <query_sql_file>
//   test_postgres_verus_kit differential <accel_id> <spec_dir> <query_sql_file> [every=10]
//   test_postgres_verus_kit snapshots    <accel_id> <spec_dir> <query_sql_file> [writes=200] [readers=4] [seed=1]
//
// <query_sql_file>: the query with $n parameters (workloads/<bench>/q<N>.sql).  Each run works on a copy of the connection's database (CREATE DATABASE ... TEMPLATE), dropped at the
// end: the spec's setup.sql and transactions change data.  The spec is DuckDB SQL (eval/harness/suite.hpp):
// `SELECT * REPLACE (e AS c) FROM t` and `range(n)` are rewritten for Postgres.
// differential: after the build and after every committed transaction, the accelerator
//   (accel_<id>(...)) is compared with the query at the same snapshot.
// snapshots: readers in REPEATABLE READ transactions run the accelerator, the query, the accelerator
//   again while a writer commits; all three must agree.
// VERUS_KIT_BOUNDS_SQL=<file>: the query's bounds query (eval/harness/float_bounds.hpp), as for DuckDB.
// A call the accelerator answered from SQL counts as a fallback (reported, and a failure of the run).
// Connection: libpq's PG* environment, database TPCH_PG_DB (host.sh's) or PGDATABASE.
#include <libpq-fe.h>

#include "eval/harness/suite.hpp"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <map>
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
  return {r.names, r.rows};
}

// The bounds queries' helpers (DuckDB's fsum, fl_delta macro): an exact sum of the DOUBLEs' shortest
// decimal forms, which is within the bound's slack of their exact sum.
constexpr const char *kBoundsPrelude = R"SQL(
CREATE OR REPLACE FUNCTION fl_delta(m double precision, a double precision) RETURNS double precision
  LANGUAGE sql IMMUTABLE AS 'SELECT a * ((m * 1.1102230246251565e-16) / (1 - m * 1.1102230246251565e-16) + 4.4408920985006262e-16)';
CREATE OR REPLACE FUNCTION fsum_step(s numeric, v double precision) RETURNS numeric
  LANGUAGE sql IMMUTABLE AS 'SELECT s + v::text::numeric';
CREATE OR REPLACE FUNCTION fsum_final(s numeric) RETURNS double precision
  LANGUAGE sql IMMUTABLE AS 'SELECT s::double precision';
CREATE OR REPLACE AGGREGATE fsum(double precision) (SFUNC = fsum_step, STYPE = numeric, INITCOND = '0', FINALFUNC = fsum_final);
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
      PgConn admin("postgres");
      admin.Exec("SELECT accel_stop_shard_workers()"); // the workers hold the database
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
    s.ordering.bounds_sql = ReadFile(bounds);
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

// The first build of the state runs in the shard worker: reads before it is done fall back.
void Warm(PgConn &c, const std::string &id, const std::string &tuple) {
  for (int tries = 0; tries < 600; ++tries) {
    const long before = c.Fallbacks();
    c.Exec("SELECT * FROM accel_" + id + "(" + tuple + ")");
    if (c.Fallbacks() == before)
      return;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  throw std::runtime_error("the accelerator " + id + " did not build in 60 s");
}

std::string Tail(PgConn &c, const Tally &tally, int fallbacks) {
  return "\"fallbacks\":" + std::to_string(fallbacks) + ",\"shard\":" + Json(c.Exec("SELECT accel_shard_stats()").rows[0][0]) +
         "," + tally.Fields();
}

int Differential(const std::string &id, const Spec &spec, const std::string &query, int every) {
  ScratchDatabase scratch;
  PgConn writer(scratch.name), reader(scratch.name);
  Dialect dialect(writer);
  const auto s = Prepare(writer, dialect, query, spec);
  Warm(reader, id, spec.params.at(0));
  Tally tally;
  std::mutex mutex;
  const auto run = RunDifferentialLoop(
      tally, spec, s.query_sql, s.ordering, reader, every, 7,
      [&](const std::string &where, const std::string &t) { return RunAccel(tally, mutex, reader, id, where, t); },
      [&](const std::string &sql) { Apply(writer, dialect, sql, false); },
      [&](const std::string &sql) { Apply(writer, dialect, sql, true); });
  const int fallbacks = [&] {
    int n = 0;
    for (const auto &[kind, count] : tally.fallback_kinds)
      n += count;
    return n;
  }();
  std::cout << "{\"mode\":\"differential\",\"txns\":" << spec.txns.size() << ",\"write_errors\":" << run.write_errors
            << ",\"write_error_examples\":" << run.write_error_examples << "]," << Tail(reader, tally, fallbacks) << "}"
            << std::endl;
  const bool compared = tally.checks > 0 && tally.reference_errors <= tally.checks;
  return compared && run.write_errors == 0 && tally.mismatches + tally.order_mismatches == 0 && fallbacks == 0 ? 0 : 1;
}

int Snapshots(const std::string &id, const Spec &spec, const std::string &query, int writes, int readers, uint64_t seed) {
  Require(!spec.txns.empty(), "snapshots needs txns/");
  ScratchDatabase scratch;
  PgConn writer(scratch.name);
  Dialect dialect(writer);
  const auto s = Prepare(writer, dialect, query, spec);
  Warm(writer, id, spec.params.at(0));
  Tally tally;
  std::mutex mutex;
  std::atomic<bool> done{false};
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < readers; ++t)
    threads.emplace_back([&, t] {
      std::mt19937_64 rng(seed * 1000003 + static_cast<uint64_t>(t));
      try {
        PgConn rc(scratch.name);
        while (!done.load()) {
          const auto &p = spec.params[rng() % spec.params.size()];
          rc.Exec("BEGIN ISOLATION LEVEL REPEATABLE READ");
          const auto a = RunAccel(tally, mutex, rc, id, "reader " + std::to_string(t), p);
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
          const auto c = RunAccel(tally, mutex, rc, id, "reader " + std::to_string(t), p);
          const auto full = [&] { return RunQuery(rc, s.ordering.unlimited_sql, p); };
          const auto probe = [&] { return RunProbe(rc, s.ordering.bounds_sql, p); };
          std::lock_guard<std::mutex> lock(mutex);
          tally.Check("reader " + std::to_string(t) + " first", p, a, b, s.ordering, full, probe);
          tally.Check("reader " + std::to_string(t) + " again", p, c, b, s.ordering, full, probe);
          rc.Exec("COMMIT");
        }
      } catch (const std::exception &e) {
        if (++failures <= 3)
          std::cerr << "reader " << t << ": " << e.what() << "\n";
      }
    });
  std::mt19937_64 rng(seed);
  int write_errors = 0;
  std::string write_error_example;
  for (int i = 0; i < writes; ++i) {
    try {
      Apply(writer, dialect, spec.txns[rng() % spec.txns.size()], false);
    } catch (const std::exception &e) { // random order: a transaction can legitimately fail (its rows gone)
      writer.Exec("ROLLBACK");
      if (++write_errors == 1)
        write_error_example = std::string(e.what()).substr(0, 600);
    }
    std::this_thread::sleep_for(std::chrono::microseconds(rng() % 2000));
  }
  done = true;
  for (auto &th : threads)
    th.join();
  int fallbacks = 0;
  for (const auto &[kind, count] : tally.fallback_kinds)
    fallbacks += count;
  std::cout << "{\"mode\":\"snapshots\",\"writes\":" << writes << ",\"write_errors\":" << write_errors
            << ",\"write_error_example\":" << Json(write_error_example) << ",\"readers\":" << readers
            << ",\"reader_failures\":" << failures << "," << Tail(writer, tally, fallbacks) << "}" << std::endl;
  return tally.mismatches + tally.order_mismatches == 0 && failures == 0 && write_errors < writes && tally.checks > 0 &&
                 fallbacks == 0
             ? 0
             : 1;
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
    if (mode == "snapshots")
      return Snapshots(argv[2], spec, query, static_cast<int>(arg(5, 200)), static_cast<int>(arg(6, 4)),
                       static_cast<uint64_t>(arg(7, 1)));
    throw std::runtime_error("unknown mode " + mode);
  } catch (const std::exception &e) {
    std::cerr << "error: " << e.what() << "\n";
    return 2;
  }
}
