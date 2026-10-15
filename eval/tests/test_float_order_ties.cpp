// Unit test of the tie-aware ordered comparison (eval/harness/float_order_ties.hpp).
#include "eval/harness/float_order_ties.hpp"

#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace ties = accelerator_codegen::float_order_ties;

void Require(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}

struct Rows {
  std::vector<std::vector<std::string>> cells; // column 1 is the float key
};

std::string Check(const Rows &expected, const Rows &actual, long long limit = -1) {
  ties::FloatOrderKey key;
  key.present = true;
  key.column = 1;
  key.limit = limit;
  const std::vector<bool> is_float = {false, true};
  const auto key_of = [](const Rows &rows) {
    return [&rows](std::size_t row) -> std::optional<double> { return std::stod(rows.cells[row][1]); };
  };
  const auto cells_of = [](const Rows &rows) { return [&rows](std::size_t row) { return rows.cells[row]; }; };
  return ties::TieAwareMismatch(
      expected.cells.size(), key, 1e-9, is_float, key_of(expected), key_of(actual), cells_of(expected),
      cells_of(actual),
      [&](std::size_t c, const std::string &e, const std::string &a) {
        return is_float[c] ? ties::DoublesClose(std::stod(e), std::stod(a), 1e-9) : e == a;
      },
      nullptr);
}

} // namespace

int main() {
  try {
    // ORDER BY detection: outermost ORDER BY, bare float column, LIMIT.
    const std::vector<std::string> names = {"name", "tag", "total_value", "cnt"};
    const std::vector<bool> is_float = {false, false, true, false};
    auto key = ties::FindFloatOrderKey(
        "SELECT name, tag, SUM(v) AS total_value, COUNT(*) AS cnt FROM t GROUP BY 1, 2 "
        "ORDER BY total_value DESC, name LIMIT 200",
        names, is_float);
    Require(key.present && key.column == 2 && key.limit == 200, "detect total_value / LIMIT 200");
    key = ties::FindFloatOrderKey("SELECT * FROM (SELECT x FROM t ORDER BY total_value) ORDER BY name", names,
                                  is_float);
    Require(!key.present, "outermost ORDER BY is on a non-float column");
    key = ties::FindFloatOrderKey("SELECT name FROM t ORDER BY SUM(v) DESC", names, is_float);
    Require(!key.present, "expression key is not a column");
    key = ties::FindFloatOrderKey("select n.tag from t order by t.total_value", names, is_float);
    Require(key.present && key.column == 2 && key.limit == -1, "qualified, lower case, no LIMIT");

    // SEC-EDGAR Q6 EPS rows: exact sums tie, DuckDB's running sum is 1 ulp
    // off, so the two rows may come in either order.
    const Rows expected{{{"a", "9"}, {"Diluted", "5.4800000000000004"}, {"Basic", "5.4799999999999995"}, {"z", "1"}}};
    const Rows swapped{{{"a", "9"}, {"Basic", "5.4800000000000004"}, {"Diluted", "5.4800000000000004"}, {"z", "1"}}};
    Require(Check(expected, swapped).empty(), "reorder inside a tie cluster is accepted");
    // Exact ties on both sides: the tie-breakers order them, no reorder.
    const Rows exact{{{"a", "9"}, {"Basic", "5.48"}, {"Diluted", "5.48"}, {"z", "1"}}};
    const Rows exact_swapped{{{"a", "9"}, {"Diluted", "5.48"}, {"Basic", "5.48"}, {"z", "1"}}};
    Require(!Check(exact, exact_swapped).empty(), "reorder of exact ties is rejected");
    // A different row inside the cluster is still a mismatch.
    const Rows wrong_member{{{"a", "9"}, {"Basic", "5.48"}, {"Other", "5.48"}, {"z", "1"}}};
    Require(!Check(expected, wrong_member).empty(), "different member is rejected");
    // Rows with keys that are not close may not be reordered.
    const Rows far_expected{{{"a", "9"}, {"b", "5"}}};
    const Rows far_swapped{{{"b", "5"}, {"a", "9"}}};
    Require(!Check(far_expected, far_swapped).empty(), "reorder across distinct keys is rejected");
    // A trailing cluster of a LIMIT-cut result may hold different members.
    const Rows cut_expected{{{"a", "9"}, {"b", "1.0000000000000002"}}};
    const Rows cut_actual{{{"a", "9"}, {"c", "1"}}};
    Require(Check(cut_expected, cut_actual, 2).empty(), "LIMIT-cut trailing cluster");
    Require(!Check(cut_expected, cut_actual, 5).empty(), "not cut: fewer rows than LIMIT");
    Require(!Check(cut_expected, cut_actual).empty(), "not cut: no LIMIT");
    const Rows cut_exact{{{"a", "9"}, {"c", "1.0000000000000002"}}};
    Require(!Check(cut_expected, cut_exact, 2).empty(), "LIMIT-cut but exactly equal keys");
    std::cout << "test_float_order_ties passed\n";
    return 0;
  } catch (const std::exception &ex) {
    std::cerr << "test_float_order_ties failed: " << ex.what() << '\n';
    return 1;
  }
}
