// The admissible-answer judgement of eval/harness/float_bounds.hpp on synthetic bounds queries.
#include <iostream>

#include "eval/harness/float_bounds.hpp"

namespace fb = accelerator_codegen::suite::float_bounds;
using Rows = fb::Rows;

namespace {
int failures = 0;
void Expect(bool admissible, const fb::Probe &p, const std::string &sql, const Rows &a, const char *what) {
  const std::string why = fb::Judge(sql, p, a);
  if (why.empty() != admissible) {
    std::cerr << "FAIL " << what << ": " << (why.empty() ? "accepted" : "rejected: " + why) << "\n";
    ++failures;
  }
}
} // namespace

int main() {
  const std::string sql = "SELECT g, SUM(v) AS total, COUNT(*) AS cnt FROM t GROUP BY g ORDER BY total DESC, g LIMIT 2";
  const std::vector<std::string> names = {"g", "total", "total__lo", "total__hi", "cnt", "__may", "__must"};
  // g, total, lo, hi, cnt, may, must
  fb::Probe p{names,
              {{"a", "10", "9", "11", "3", "true", "true"},
               {"b", "10.5", "9.5", "11.5", "2", "true", "true"},
               {"c", "10.2", "10.1", "10.3", "4", "true", "true"},
               {"d", "5", "4", "6", "1", "true", "false"}}};
  // a, b, c overlap: any two of them whose values are sorted, c / b / a each may be on top
  Expect(true, p, sql, {{"b", "11", "2"}, {"a", "10.5", "3"}}, "b above a, c cut off: c may drop to 10.1 <= 10.5");
  Expect(true, p, sql, {{"b", "11.4", "2"}, {"c", "10.3", "4"}}, "a may cut: its lo 9 <= 10.3");
  Expect(false, p, sql, {{"b", "11.4", "2"}, {"c", "10.0", "4"}}, "c below its interval");
  Expect(false, p, sql, {{"b", "10.5", "2"}, {"a", "11", "3"}}, "out of order on its own values");
  Expect(false, p, sql, {{"b", "11", "2"}, {"a", "11", "3"}}, "tie on the value, then g must ascend");
  Expect(true, p, sql, {{"a", "11", "3"}, {"b", "11", "2"}}, "tie on the value, g ascending");
  Expect(false, p, sql, {{"b", "10.5", "9"}, {"a", "10", "3"}}, "cnt is exact");
  Expect(false, p, sql, {{"b", "10.5", "2"}, {"b", "10.5", "2"}}, "a group twice");
  Expect(false, p, sql, {{"zz", "10", "2"}}, "no such group");
  Expect(false, p, sql, {{"d", "5", "1"}, {"a", "10", "3"}}, "d is not above a");
  Expect(false, p, sql, {{"b", "10.5", "2"}, {"a", "10", "3"}, {"c", "10", "4"}}, "more rows than the LIMIT");
  // a LIMIT cut with a candidate that must be in and cannot fall to the cut value
  fb::Probe q{names, {{"a", "10", "9", "11", "3", "true", "true"}, {"b", "20", "19", "21", "2", "true", "true"}}};
  Expect(false, q, sql, {{"a", "10", "3"}}, "b must be in the answer (not cut: fewer than LIMIT rows)");
  const std::string one = "SELECT g, SUM(v) AS total FROM t GROUP BY g ORDER BY total DESC LIMIT 1";
  fb::Probe r{{"g", "total", "total__lo", "total__hi", "__may", "__must"},
              {{"a", "10", "9", "11", "true", "true"}, {"b", "12", "11", "13", "true", "true"}, {"c", "30", "29", "31", "true", "true"}}};
  Expect(false, r, one, {{"a", "10"}}, "b and c must be above a");
  Expect(true, r, one, {{"c", "30"}}, "c on top");
  Expect(true, r, one, {{"c", "29.5"}}, "c on top, any value in its interval");
  // HAVING at the threshold: a candidate that may but need not be in the answer
  fb::Probe h{{"g", "total", "total__lo", "total__hi", "__may", "__must"},
              {{"a", "10", "9", "11", "true", "false"}, {"b", "12", "11", "13", "true", "true"}}};
  const std::string all = "SELECT g, SUM(v) AS total FROM t GROUP BY g";
  Expect(true, h, all, {{"b", "12"}}, "a absent, may not be in");
  Expect(true, h, all, {{"a", "10.5"}, {"b", "12.5"}}, "a present");
  Expect(false, h, all, {{"a", "10.5"}}, "b must be in");
  std::cerr << (failures ? "FAILED\n" : "float_bounds: ok\n");
  return failures ? 1 : 0;
}
