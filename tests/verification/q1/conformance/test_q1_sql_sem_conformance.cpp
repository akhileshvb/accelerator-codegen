// Conformance: MathSem (SQL integer embedding) vs FinalizeQ1MergedGroups.
// Implements filter + group-by Apply fold matching Q1Sql.dfy, then real Finalize.
#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>

#include "tpch/q1/postgres/aux/finalize.hpp"

using accelerator_codegen::ApplyQ1RowContribution;
using accelerator_codegen::FinalizeQ1MergedGroups;
using accelerator_codegen::MergeQ1DayGroupStats;
using accelerator_codegen::PackQ1GroupKey;
using accelerator_codegen::Q1AccT;
using accelerator_codegen::Q1DayGroupStats;
using accelerator_codegen::Q1GroupKey;
using accelerator_codegen::Q1MergedGroupStats;
using accelerator_codegen::Q1OutputRow;

namespace {

struct Row {
  int32_t shipdate;
  int64_t qty, price, disc, tax;
  char rf, ls;
};

// Twin of Q1Sql.SqlFilter
std::vector<Row> SqlFilter(const std::vector<Row> &t, int32_t cutoff) {
  std::vector<Row> out;
  for (const auto &r : t)
    if (r.shipdate <= cutoff)
      out.push_back(r);
  return out;
}

// Twin of Q1Sql.SqlFoldByGroup via Apply
std::map<Q1GroupKey, Q1MergedGroupStats>
SqlFoldByGroup(const std::vector<Row> &t) {
  std::map<Q1GroupKey, Q1MergedGroupStats> merged;
  for (const auto &r : t) {
    const Q1GroupKey k = PackQ1GroupKey(r.rf, r.ls);
    Q1DayGroupStats day{};
    ApplyQ1RowContribution(day, r.qty, r.price, r.disc, r.tax, 1);
    MergeQ1DayGroupStats(merged[k], day);
  }
  return merged;
}

bool DoubleBitsEqual(double a, double b) {
  uint64_t ua = 0, ub = 0;
  std::memcpy(&ua, &a, sizeof(ua));
  std::memcpy(&ub, &b, sizeof(ub));
  return ua == ub;
}

// MathSem: filter → fold → Finalize (catalog Q1 shape)
std::vector<Q1OutputRow> MathSem(const std::vector<Row> &t, int32_t cutoff) {
  auto filtered = SqlFilter(t, cutoff);
  auto merged = SqlFoldByGroup(filtered);
  std::unordered_map<Q1GroupKey, Q1MergedGroupStats> um(merged.begin(),
                                                         merged.end());
  return FinalizeQ1MergedGroups(um);
}

void CheckMathSemMatchesFinalizeDirect() {
  // Same groups via direct merge without filter (all rows in cutoff)
  std::vector<Row> t = {
      {100, 1000, 10000, 5, 4, 'N', 'O'},
      {100, 2000, 20000, 6, 8, 'N', 'O'},
      {50, 500, 5000, 0, 0, 'A', 'F'},
      {200, 1500, 33300, 3, 7, 'R', 'O'}, // filtered out if cutoff=100
  };
  const int32_t cutoff = 100;
  auto rows = MathSem(t, cutoff);

  // Manual expected: only first three rows; keys N/O and A/F
  assert(rows.size() == 2);
  assert(rows[0].group_key < rows[1].group_key); // sorted

  Q1MergedGroupStats no{};
  {
    Q1DayGroupStats d1{};
    ApplyQ1RowContribution(d1, 1000, 10000, 5, 4, 1);
    MergeQ1DayGroupStats(no, d1);
    Q1DayGroupStats d2{};
    ApplyQ1RowContribution(d2, 2000, 20000, 6, 8, 1);
    MergeQ1DayGroupStats(no, d2);
  }
  Q1MergedGroupStats af{};
  {
    Q1DayGroupStats d{};
    ApplyQ1RowContribution(d, 500, 5000, 0, 0, 1);
    MergeQ1DayGroupStats(af, d);
  }

  std::unordered_map<Q1GroupKey, Q1MergedGroupStats> direct;
  direct[PackQ1GroupKey('A', 'F')] = af;
  direct[PackQ1GroupKey('N', 'O')] = no;
  auto expected = FinalizeQ1MergedGroups(direct);
  assert(expected.size() == rows.size());
  for (size_t i = 0; i < rows.size(); ++i) {
    assert(rows[i].group_key == expected[i].group_key);
    assert(rows[i].count_order == expected[i].count_order);
    assert(DoubleBitsEqual(rows[i].avg_qty, expected[i].avg_qty));
    assert(DoubleBitsEqual(rows[i].avg_price, expected[i].avg_price));
    assert(DoubleBitsEqual(rows[i].avg_disc, expected[i].avg_disc));
  }
}

void CheckEmptyFilter() {
  std::vector<Row> t = {{500, 1000, 10000, 5, 4, 'N', 'O'}};
  auto rows = MathSem(t, /*cutoff=*/100);
  assert(rows.empty());
}

void CheckSqlAggFormulas() {
  // SqlAgg equals one Apply from empty (Q1Sql.SqlAggEqualsApply singleton)
  Row r{1, 1000, 10000, 5, 4, 'N', 'O'};
  Q1DayGroupStats cpp{};
  ApplyQ1RowContribution(cpp, r.qty, r.price, r.disc, r.tax, 1);
  assert(cpp.count == 1);
  assert(cpp.sum_qty == 1000);
  assert(cpp.sum_base_price == 10000);
  assert(cpp.sum_disc_price ==
         static_cast<Q1AccT>(10000) * static_cast<Q1AccT>(95));
  assert(cpp.sum_charge == cpp.sum_disc_price * static_cast<Q1AccT>(104));
  assert(cpp.sum_discount == 5);
}

} // namespace

int main() {
  CheckSqlAggFormulas();
  CheckEmptyFilter();
  CheckMathSemMatchesFinalizeDirect();
  return 0;
}
