// SCAFFOLDING
// Floating-point ORDER BY ties in differential comparisons.
//
// SQL leaves the summation order of SUM/AVG over DOUBLE unspecified, and
// DuckDB's SUM(DOUBLE) is a plain running sum in scan / hash / thread-combine
// order, so it can differ by a few ulps from an accelerator's sum of the same
// values (e.g. a double-double, order-independent one). Two groups whose
// exact sums are equal can then come out in either order: SEC-EDGAR Q6's
// basic / diluted EPS rows tie exactly and are ordered by tag, but DuckDB's
// running sum of one of them is 1 ulp off. No accelerator can reproduce
// DuckDB's summation order in general (it depends on the physical row order,
// and an incrementally maintained sum has no such order at all).
//
// So when the leading ORDER BY key of the outermost query is a DOUBLE/FLOAT
// result column, a positional mismatch is re-checked tie-aware: a run of
// consecutive expected rows whose key is within the comparator's tolerance of
// the run's first key is a tie cluster; inside it the actual keys must match
// positionally and the rows must agree as a multiset. A cluster may only be
// reordered when its keys are not bit-identical on at least one side (real
// rounding noise): exact ties are ordered by the tie-breaking keys, and that
// order is still checked positionally. A cluster that reaches
// the end of a result cut by a literal LIMIT may have been cut on its rounded
// keys, so only its keys are checked. Same rule as the Postgres Q6
// differential (EqualTolerant).
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

namespace accelerator_codegen::float_order_ties {

struct FloatOrderKey {
  bool present = false;
  std::size_t column = 0;
  long long limit = -1; // the outermost query's literal LIMIT, else -1
};

inline bool IsWordChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

inline std::string UpperAscii(std::string text) {
  for (auto &c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return text;
}

// Positions of keyword `word` at parenthesis depth 0, outside quotes and
// comments, in the upper-cased `sql`.
inline std::vector<std::size_t> TopLevelKeyword(const std::string &upper, const std::string &word) {
  std::vector<std::size_t> out;
  int depth = 0;
  for (std::size_t i = 0; i < upper.size(); ++i) {
    const char c = upper[i];
    if (c == '\'' || c == '"') {
      const auto close = upper.find(c, i + 1);
      if (close == std::string::npos) break;
      i = close;
    } else if (c == '-' && i + 1 < upper.size() && upper[i + 1] == '-') {
      const auto eol = upper.find('\n', i);
      if (eol == std::string::npos) break;
      i = eol;
    } else if (c == '(') {
      ++depth;
    } else if (c == ')') {
      --depth;
    } else if (depth == 0 && upper.compare(i, word.size(), word) == 0 &&
               (i == 0 || !IsWordChar(upper[i - 1])) &&
               (i + word.size() >= upper.size() || !IsWordChar(upper[i + word.size()]))) {
      out.push_back(i);
    }
  }
  return out;
}

// The result column named by the outermost ORDER BY's first item, when that
// item is a bare (possibly qualified) column name and the column is
// floating-point.
inline FloatOrderKey FindFloatOrderKey(const std::string &sql, const std::vector<std::string> &names,
                                       const std::vector<bool> &is_float) {
  FloatOrderKey key;
  const auto upper = UpperAscii(sql);
  const auto order = TopLevelKeyword(upper, "ORDER");
  if (order.empty()) return key;
  const auto skip_space = [&](std::size_t at) {
    while (at < upper.size() && std::isspace(static_cast<unsigned char>(upper[at]))) ++at;
    return at;
  };
  std::size_t at = skip_space(order.back() + 5);
  if (upper.compare(at, 2, "BY") != 0) return key;
  at = skip_space(at + 2);
  std::size_t end = at;
  while (end < upper.size() && (IsWordChar(upper[end]) || upper[end] == '.')) ++end;
  std::string item = upper.substr(at, end - at);
  const std::size_t next = skip_space(end);
  if (next < upper.size() && upper[next] == '(') return key; // an expression
  if (const auto dot = item.rfind('.'); dot != std::string::npos) item = item.substr(dot + 1);
  if (item.empty()) return key;
  for (std::size_t column = 0; column < names.size() && column < is_float.size(); ++column) {
    if (is_float[column] && UpperAscii(names[column]) == item) {
      key.present = true;
      key.column = column;
      break;
    }
  }
  for (const auto limit : TopLevelKeyword(upper, "LIMIT")) {
    if (limit > order.back()) key.limit = std::strtoll(upper.c_str() + limit + 5, nullptr, 10);
  }
  return key;
}

inline bool DoublesClose(double lhs, double rhs, double rel_tol) {
  if (lhs == rhs) return true;
  if (std::isnan(lhs) || std::isnan(rhs)) return std::isnan(lhs) && std::isnan(rhs);
  const double scale = std::max({1.0, std::fabs(lhs), std::fabs(rhs)});
  return std::fabs(lhs - rhs) <= scale * rel_tol;
}

inline std::string ExactDouble(std::optional<double> value) {
  if (!value) return "NULL";
  std::ostringstream text;
  text << std::setprecision(17) << *value << " (" << std::hexfloat << *value << ")";
  return text.str();
}

// Empty when the actual rows equal the expected ones up to reordering inside
// tie clusters of `key`, else why not.
//   expected_key / actual_key: row -> std::optional<double> (nullopt = NULL)
//   expected_row / actual_row: row -> std::vector<std::string> cells
//   cells_equal(column, expected_cell, actual_cell) -> bool
// `is_float` marks the floating-point columns: rows of a cluster are paired by
// their other columns. Accepted reorders are described on `log`.
template <class ExpectedKey, class ActualKey, class ExpectedRow, class ActualRow, class CellsEqual>
std::string TieAwareMismatch(std::size_t rows, const FloatOrderKey &key, double rel_tol,
                             const std::vector<bool> &is_float, ExpectedKey expected_key,
                             ActualKey actual_key, ExpectedRow expected_row, ActualRow actual_row,
                             CellsEqual cells_equal, std::ostream *log) {
  struct Row {
    std::string pair_key;
    std::vector<std::string> cells;
  };
  const auto make = [&](std::vector<std::string> cells) {
    Row row;
    for (std::size_t c = 0; c < cells.size(); ++c) {
      if (c >= is_float.size() || !is_float[c]) row.pair_key += cells[c] + '\x1f';
    }
    row.cells = std::move(cells);
    return row;
  };
  const auto same = [&](const std::optional<double> &a, const std::optional<double> &b) {
    return a.has_value() == b.has_value() && (!a || DoublesClose(*a, *b, rel_tol));
  };
  const auto join = [](const std::vector<std::string> &cells) {
    std::string out;
    for (std::size_t c = 0; c < cells.size(); ++c) out += (c ? " | " : "") + cells[c];
    return out;
  };
  for (std::size_t first = 0; first < rows;) {
    const auto anchor = expected_key(first);
    std::size_t end = first + 1;
    while (end < rows && same(expected_key(end), anchor)) ++end;
    std::vector<Row> expected;
    std::vector<Row> actual;
    for (std::size_t row = first; row < end; ++row) {
      if (!same(expected_key(row), actual_key(row))) {
        return "order key at row " + std::to_string(row) + " expected=" +
               ExactDouble(expected_key(row)) + " actual=" + ExactDouble(actual_key(row));
      }
      expected.push_back(make(expected_row(row)));
      actual.push_back(make(actual_row(row)));
    }
    bool reordered = false;
    for (std::size_t i = 0; i < expected.size() && !reordered; ++i) {
      reordered = expected[i].pair_key != actual[i].pair_key;
    }
    const bool cut_by_limit =
        end == rows && key.limit >= 0 && static_cast<long long>(rows) == key.limit;
    // Only rounding noise licenses a reorder: when each side's keys in the
    // cluster are bit-identical, both orders come from the tie-breaking keys
    // and must agree exactly. A LIMIT-cut cluster may also have been cut on
    // keys that differ between the sides only.
    bool noisy = false;
    for (std::size_t row = first; row < end && !noisy; ++row) {
      noisy = expected_key(row) != expected_key(first) || actual_key(row) != actual_key(first) ||
              (cut_by_limit && expected_key(row) != actual_key(row));
    }
    if (reordered && !noisy) {
      return "rows reordered at rows " + std::to_string(first) + ".." + std::to_string(end - 1) +
             " although their order keys are exactly equal on each side (" +
             ExactDouble(expected_key(first)) + " / " + ExactDouble(actual_key(first)) + ")";
    }
    if (!cut_by_limit) {
      const auto by_key = [](const Row &a, const Row &b) { return a.pair_key < b.pair_key; };
      std::stable_sort(expected.begin(), expected.end(), by_key);
      std::stable_sort(actual.begin(), actual.end(), by_key);
      for (std::size_t i = 0; i < expected.size(); ++i) {
        bool ok = expected[i].cells.size() == actual[i].cells.size();
        for (std::size_t c = 0; ok && c < expected[i].cells.size(); ++c) {
          ok = cells_equal(c, expected[i].cells[c], actual[i].cells[c]);
        }
        if (!ok) {
          return "rows differ in the order-key tie cluster at rows " + std::to_string(first) +
                 ".." + std::to_string(end - 1) + ": expected [" + join(expected[i].cells) +
                 "] actual [" + join(actual[i].cells) + "]";
        }
      }
    }
    if (reordered && log != nullptr) {
      *log << "differential: rows " << first << ".." << end - 1
           << (cut_by_limit ? " differ inside a LIMIT-cut" : " reordered inside a")
           << " floating-point order-key tie cluster";
      for (std::size_t row = first; row < end; ++row) {
        *log << "\n  row " << row << " expected [" << join(expected_row(row))
             << "] key=" << ExactDouble(expected_key(row))
             << "\n           actual [" << join(actual_row(row)) << "] key=" << ExactDouble(actual_key(row));
      }
      *log << '\n';
    }
    first = end;
  }
  return {};
}

} // namespace accelerator_codegen::float_order_ties
