// SCAFFOLDING (test harness)
// The admissible answers of a query whose DOUBLE SUM / AVG the engine rounds in an order of its own.
//
// The generated spec does not fix such an aggregate: it admits any value within the rounding bound
// of the exact one (verus_common::rel::float_within: |v - e| <= gamma_m * a, a the sum of absolute
// values, m the rounded operations on a path, gamma_m = m u / (1 - m u), u = 2^-53).  An
// answer is right iff SOME admissible choice of those values yields exactly it.  So the reference
// result is not the only right answer: a group whose value is at a HAVING threshold may be in the
// answer or not, groups whose values are within the bound of each other may swap places or
// straddle a LIMIT cut.
//
// A query declares its bounds by a bounds query (workloads/<bench>/q<N>.bounds.sql, same parameters):
// one row per candidate group, the query's output columns in order, and after each DOUBLE
// aggregate its interval <col>__lo / <col>__hi (the exact value -+ the bound), then __may (the
// group is in the answer for some admissible choice) and __must (for every one).  The accelerator's
// answer A is admissible iff
//   1. every row of A is a candidate that may be in the answer, its non-DOUBLE cells equal and its
//      DOUBLE cells inside their intervals; no candidate appears twice;
//   2. every candidate that must be in the answer is in A, unless a LIMIT cut it off;
//   3. A is sorted by the query's ORDER BY on its own values (ties in any order);
//   4. with a LIMIT n: |A| <= n, and when |A| = n no candidate outside A must be in the answer and
//      precede A's last row for every value in its interval.
// This is the spec's meaning, not a tolerance: the exactness of everything else is kept.
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace accelerator_codegen::suite::float_bounds {

using Rows = std::vector<std::vector<std::string>>;

// u = 2^-53 and the slack of the exact value's own computation (a compensated sum, off by up to
// 2u a): the interval a bounds query reports is gamma_m a + 4 u a wide on each side.
constexpr const char *kPrelude =
    "CREATE OR REPLACE TEMP MACRO fl_delta(m, a) AS "
    "a * ((m * 1.1102230246251565e-16) / (1 - m * 1.1102230246251565e-16) + 4.4408920985006262e-16)";

struct Probe {
  std::vector<std::string> names;
  Rows rows;
};

namespace detail {

inline std::string Lower(std::string s) {
  for (auto &ch : s)
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}

inline bool Number(const std::string &s, double &out) {
  char *end = nullptr;
  out = std::strtod(s.c_str(), &end);
  return end != s.c_str() && !*end;
}

struct Column {
  std::string name;
  std::size_t at = 0;
  long lo = -1, hi = -1; // probe columns of a DOUBLE aggregate's interval
  bool numeric = false;  // every non-NULL cell of the candidates is a number
};

struct Key {
  std::size_t column;
  bool desc;
};

struct Layout {
  std::vector<Column> columns;
  std::size_t may = 0, must = 0;
  std::vector<Key> keys;
  long limit = -1;
  std::string error;
};

// Position of the last top-level occurrence of `word` (upper case), or npos.
inline std::size_t TopLevel(const std::string &sql, const std::string &word) {
  const std::string up = [&] {
    std::string s = sql;
    for (auto &ch : s)
      ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return s;
  }();
  std::size_t found = std::string::npos;
  int depth = 0;
  bool quote = false;
  for (std::size_t i = 0; i < up.size(); ++i) {
    if (up[i] == '\'')
      quote = !quote;
    if (quote)
      continue;
    depth += up[i] == '(' ? 1 : up[i] == ')' ? -1 : 0;
    if (depth == 0 && up.compare(i, word.size(), word) == 0 && (i == 0 || !std::isalnum(up[i - 1])) &&
        (i + word.size() == up.size() || !std::isalnum(up[i + word.size()])))
      found = i;
  }
  return found;
}

inline Layout MakeLayout(const std::string &query_sql, const Probe &p) {
  Layout l;
  auto find = [&](const std::string &name) -> long {
    for (std::size_t i = 0; i < p.names.size(); ++i)
      if (p.names[i] == name)
        return static_cast<long>(i);
    return -1;
  };
  const long may = find("__may"), must = find("__must");
  if (may < 0 || must < 0) {
    l.error = "the bounds query lacks __may / __must";
    return l;
  }
  l.may = static_cast<std::size_t>(may);
  l.must = static_cast<std::size_t>(must);
  for (std::size_t i = 0; i < p.names.size(); ++i) {
    const auto &n = p.names[i];
    const bool bound = n.size() > 4 && (n.compare(n.size() - 4, 4, "__lo") == 0 || n.compare(n.size() - 4, 4, "__hi") == 0);
    if (bound || n == "__may" || n == "__must")
      continue;
    Column c{n, i, find(n + "__lo"), find(n + "__hi"), true};
    for (const auto &row : p.rows) {
      double d;
      if (row[i] != "NULL" && !Number(row[i], d))
        c.numeric = false;
    }
    l.columns.push_back(c);
  }
  const auto ob = TopLevel(query_sql, "ORDER BY");
  if (ob == std::string::npos)
    return l;
  const auto lim = TopLevel(query_sql, "LIMIT");
  const bool limited = lim != std::string::npos && lim > ob;
  if (limited)
    l.limit = std::atol(query_sql.c_str() + lim + 5);
  std::string items = query_sql.substr(ob + 8, (limited ? lim : query_sql.size()) - ob - 8);
  std::size_t start = 0;
  while (start < items.size()) {
    auto comma = items.find(',', start);
    std::string item = items.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    start = comma == std::string::npos ? items.size() : comma + 1;
    std::size_t b = item.find_first_not_of(" \t\n"), e = item.find_first_of(" \t\n", b);
    if (b == std::string::npos)
      continue;
    std::string name = item.substr(b, e == std::string::npos ? std::string::npos : e - b);
    const std::string rest = e == std::string::npos ? "" : Lower(item.substr(e));
    name = Lower(name.substr(name.find_last_of('.') == std::string::npos ? 0 : name.find_last_of('.') + 1));
    std::size_t col = l.columns.size();
    for (std::size_t i = 0; i < l.columns.size(); ++i)
      if (Lower(l.columns[i].name) == name)
        col = i;
    if (col == l.columns.size() && std::all_of(name.begin(), name.end(), ::isdigit) && !name.empty())
      col = std::stoul(name) - 1;
    if (col >= l.columns.size()) {
      l.error = "ORDER BY item '" + name + "' is not an output column";
      return l;
    }
    l.keys.push_back({col, rest.find("desc") != std::string::npos});
  }
  return l;
}

// NULLs sort last, either direction (DuckDB's default).  <0: a comes first.
inline int Order(const Column &c, const std::string &a, const std::string &b, bool desc) {
  if (a == "NULL" || b == "NULL")
    return a == b ? 0 : a == "NULL" ? 1 : -1;
  int cmp;
  double x, y;
  if (c.numeric && Number(a, x) && Number(b, y))
    cmp = x < y ? -1 : x > y ? 1 : 0;
  else
    cmp = a < b ? -1 : a > b ? 1 : 0;
  return desc ? -cmp : cmp;
}

// The group a row belongs to: its non-DOUBLE cells.  A candidate's cells sit at the probe columns, an
// accelerator row's at the output positions.
inline std::string GroupOf(const Layout &l, const std::vector<std::string> &row, bool candidate) {
  std::string k;
  for (std::size_t c = 0; c < l.columns.size(); ++c)
    if (l.columns[c].lo < 0)
      k += row[candidate ? l.columns[c].at : c] + '\x1f';
  return k;
}

} // namespace detail

// "" if `accel` is an admissible answer, else why not.
inline std::string Judge(const std::string &query_sql, const Probe &p, const Rows &accel) {
  using namespace detail;
  const Layout l = MakeLayout(query_sql, p);
  if (!l.error.empty())
    return l.error;
  std::map<std::string, std::size_t> candidate;
  for (std::size_t i = 0; i < p.rows.size(); ++i)
    if (!candidate.emplace(GroupOf(l, p.rows[i], true), i).second)
      return "the bounds query has two candidates with one key";
  std::vector<char> in_answer(p.rows.size(), 0);
  std::vector<std::size_t> row_of(accel.size());
  for (std::size_t j = 0; j < accel.size(); ++j) {
    if (accel[j].size() != l.columns.size())
      return "row " + std::to_string(j) + " has " + std::to_string(accel[j].size()) + " cells, not " +
             std::to_string(l.columns.size());
    auto it = candidate.find(GroupOf(l, accel[j], false));
    if (it == candidate.end())
      return "row " + std::to_string(j) + " is no candidate group";
    const auto &pr = p.rows[it->second];
    if (in_answer[it->second]++)
      return "row " + std::to_string(j) + " repeats a group";
    if (pr[l.may] != "true")
      return "row " + std::to_string(j) + " is a group that cannot be in the answer";
    for (std::size_t c = 0; c < l.columns.size(); ++c) {
      const auto &col = l.columns[c];
      if (col.lo < 0)
        continue;
      double v, lo, hi;
      if (accel[j][c] == "NULL" || pr[col.lo] == "NULL") {
        if (accel[j][c] != pr[col.lo])
          return "row " + std::to_string(j) + " " + col.name + ": NULL mismatch";
        continue;
      }
      if (!Number(accel[j][c], v) || !Number(pr[col.lo], lo) || !Number(pr[col.hi], hi))
        return "row " + std::to_string(j) + " " + col.name + ": not a number";
      if (!(v >= lo && v <= hi))
        return "row " + std::to_string(j) + " " + col.name + "=" + accel[j][c] + " outside [" + pr[col.lo] + ", " +
               pr[col.hi] + "]";
    }
    row_of[j] = it->second;
  }
  for (std::size_t j = 1; j < accel.size(); ++j)
    for (const auto &k : l.keys) {
      const int c = Order(l.columns[k.column], accel[j - 1][k.column], accel[j][k.column], k.desc);
      if (c < 0)
        break;
      if (c > 0)
        return "rows " + std::to_string(j - 1) + ", " + std::to_string(j) + " are out of order";
    }
  if (l.limit >= 0 && static_cast<long>(accel.size()) > l.limit)
    return "more rows than the LIMIT";
  const bool cut = l.limit >= 0 && static_cast<long>(accel.size()) == l.limit && !accel.empty();
  for (std::size_t i = 0; i < p.rows.size(); ++i) {
    if (in_answer[i] || p.rows[i][l.must] != "true")
      continue;
    if (!cut)
      return "group " + GroupOf(l, p.rows[i], true) + " must be in the answer and is missing";
    // it may be cut off only if some admissible value does not put it before the last row
    const auto &last = accel.back();
    bool before = false; // precedes the last row for every value in its interval
    for (const auto &k : l.keys) {
      const auto &col = l.columns[k.column];
      // the value of the interval that puts the group latest: the lower end descending, the upper ascending
      const std::string &worst = col.lo >= 0 ? p.rows[i][k.desc ? col.lo : col.hi] : p.rows[i][col.at];
      const int c = Order(col, worst, last[k.column], k.desc);
      if (c < 0) {
        before = true;
        break;
      }
      if (c > 0)
        break;
    }
    if (before)
      return "group " + GroupOf(l, p.rows[i], true) + " must be in the answer and precedes the LIMIT cut";
  }
  return "";
}

} // namespace accelerator_codegen::suite::float_bounds
