#pragma once

// Shared libpq helpers of the SEC-EDGAR Q2 PostgreSQL tests: connection from
// the PG* environment (default: the q2 sandbox instance), parameterised
// queries, and the accelerator-vs-plain-SQL comparison.

#include <libpq-fe.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace q2pg {

inline std::string Env(const char *name, const char *fallback) {
  const char *v = std::getenv(name);
  return v && *v ? v : fallback;
}

inline std::string ConnInfo(const std::string &db) {
  return "host=" + Env("PGHOST", "127.0.0.1") + " port=" + Env("PGPORT", "55402") +
         " user=" + Env("PGUSER", "postgres") + " password=" + Env("PGPASSWORD", "postgres") +
         " dbname=" + db + " options='-c client_min_messages=warning'";
}

inline void Require(bool ok, const std::string &what) {
  if (!ok) {
    throw std::runtime_error(what);
  }
}

// A cell: text form plus NULL flag.
struct Cell {
  std::string text;
  bool is_null = false;
};
using Row = std::vector<Cell>;
using Rows = std::vector<Row>;

class Conn {
public:
  explicit Conn(const std::string &db) : conn(PQconnectdb(ConnInfo(db).c_str())) {
    Require(PQstatus(conn) == CONNECTION_OK, std::string("connect: ") + PQerrorMessage(conn));
  }
  ~Conn() { PQfinish(conn); }
  Conn(const Conn &) = delete;
  Conn &operator=(const Conn &) = delete;

  // Runs `sql` with text parameters (nullptr = NULL).  Throws on error.
  Rows Query(const std::string &sql, const std::vector<const char *> &params = {}) {
    PGresult *res = PQexecParams(conn, sql.c_str(), static_cast<int>(params.size()), nullptr,
                                 params.empty() ? nullptr : params.data(), nullptr, nullptr, 0);
    const auto st = PQresultStatus(res);
    if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK) {
      std::string msg = PQresultErrorMessage(res);
      PQclear(res);
      throw std::runtime_error(msg + " [" + sql.substr(0, 200) + "]");
    }
    Rows rows;
    const int nr = PQntuples(res), nc = PQnfields(res);
    for (int r = 0; r < nr; ++r) {
      Row row;
      for (int c = 0; c < nc; ++c) {
        Cell cell;
        cell.is_null = PQgetisnull(res, r, c) != 0;
        if (!cell.is_null) {
          cell.text.assign(PQgetvalue(res, r, c), static_cast<size_t>(PQgetlength(res, r, c)));
        }
        row.push_back(std::move(cell));
      }
      rows.push_back(std::move(row));
    }
    PQclear(res);
    return rows;
  }
  // Statement that may fail: returns the error text ("" on success).
  std::string TryExec(const std::string &sql) {
    PGresult *res = PQexec(conn, sql.c_str());
    const auto st = PQresultStatus(res);
    std::string err;
    if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK) {
      err = PQresultErrorMessage(res);
    }
    PQclear(res);
    return err;
  }
  void Exec(const std::string &sql) {
    const std::string err = TryExec(sql);
    Require(err.empty(), err + " [" + sql.substr(0, 200) + "]");
  }
  std::string Scalar(const std::string &sql) {
    const auto rows = Query(sql);
    return rows.empty() || rows[0].empty() ? std::string() : rows[0][0].text;
  }

private:
  PGconn *conn;
};

inline double ParseDouble(const std::string &s) { return std::strtod(s.c_str(), nullptr); }

inline bool SameDouble(const std::string &a, const std::string &b) {
  const double x = ParseDouble(a), y = ParseDouble(b);
  if (std::isnan(x) || std::isnan(y)) {
    return std::isnan(x) && std::isnan(y);
  }
  return x == y; // -0 == 0, as in the SQL join condition
}

// The query of benchmarks/sec_edgar/queries/q2.sql with ($1 fy, $2 uom).
inline const char *PlainSql() {
  return "SELECT s.name, n.tag, n.value FROM num n JOIN sub s ON n.adsh = s.adsh "
         "JOIN (SELECT adsh, tag, MAX(value) AS max_value FROM num "
         "WHERE uom = $2::text AND value IS NOT NULL GROUP BY adsh, tag) m "
         "ON n.adsh = m.adsh AND n.tag = m.tag AND n.value = m.max_value "
         "WHERE n.uom = $2::text AND s.fy = $1::bigint AND n.value IS NOT NULL "
         "ORDER BY n.value DESC, s.name, n.tag LIMIT 100";
}

inline const char *AccelSql() { return "SELECT name, tag, value FROM accel_sec_edgar_q2_aux($1::bigint, $2::text)"; }

// Compares accel and plain answers; returns "" on a match, else a description.
// `plain_conn` may differ from `accel_conn` (e.g. to keep accel off a snapshot).
inline std::string Compare(Conn &accel_conn, Conn &plain_conn, const char *fy, const char *uom) {
  const std::vector<const char *> params{fy, uom};
  const Rows want = plain_conn.Query(PlainSql(), params);
  const Rows got = accel_conn.Query(AccelSql(), params);
  const std::string ctx = std::string("fy=") + (fy ? fy : "NULL") + " uom=" +
                          (uom ? uom : "NULL");
  if (want.size() != got.size()) {
    return ctx + ": row count accel=" + std::to_string(got.size()) +
           " sql=" + std::to_string(want.size());
  }
  for (size_t i = 0; i < want.size(); ++i) {
    if (want[i][0].is_null != got[i][0].is_null || want[i][0].text != got[i][0].text) {
      return ctx + ": name row " + std::to_string(i) + " accel=" + got[i][0].text +
             " sql=" + want[i][0].text;
    }
    if (want[i][1].text != got[i][1].text) {
      return ctx + ": tag row " + std::to_string(i) + " accel=" + got[i][1].text +
             " sql=" + want[i][1].text;
    }
    if (!SameDouble(want[i][2].text, got[i][2].text)) {
      return ctx + ": value row " + std::to_string(i) + " accel=" + got[i][2].text +
             " sql=" + want[i][2].text;
    }
  }
  return "";
}

} // namespace q2pg
