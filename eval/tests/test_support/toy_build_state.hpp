// The toy module of the parallel-build tests: state built from a scan of
// (Int key, Date day, Decimal(2) quantity, Text flag, Text name), whatever
// the engine. Merging in any partition order gives the same state, except
// `inexact_sum`, a floating-point sum that is reproducible per partition count.
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <string_view>

namespace accelerator_codegen::aux_state::test {

struct ToyState {
  int64_t rows = 0, key_sum = 0, qty_raw = 0, date_max = 0;
  double exact_sum = 0; // qty_raw / 4: every partial sum is exactly representable
  double inexact_sum = 0;
  uint64_t name_hash = 0, work_hash = 0; // work_hash: the cost knob of Add
  std::map<char, int64_t> flags;

  bool operator==(const ToyState &o) const {
    return rows == o.rows && key_sum == o.key_sum && qty_raw == o.qty_raw && date_max == o.date_max &&
           exact_sum == o.exact_sum && name_hash == o.name_hash && work_hash == o.work_hash && flags == o.flags;
  }

  // `work` extra hash rounds per row stand in for an expensive state insert.
  template <class Row> void Add(const Row &row, int work = 0) {
    rows++;
    key_sum += row.Int(0);
    date_max = std::max<int64_t>(date_max, row.Date(1));
    qty_raw += row.Decimal(2);
    exact_sum += static_cast<double>(row.Decimal(2)) * 0.25;
    inexact_sum += static_cast<double>(row.Decimal(2)) * 0.1;
    flags[row.Char(3)]++;
    name_hash ^= std::hash<std::string_view>()(row.TextView(4));
    uint64_t mix = static_cast<uint64_t>(row.Int(0));
    for (int i = 0; i < work; ++i) {
      mix = (mix ^ (mix >> 29)) * 0xbf58476d1ce4e5b9ULL + i;
    }
    work_hash ^= work ? mix : 0;
  }

  void Merge(ToyState &&from) {
    rows += from.rows;
    key_sum += from.key_sum;
    qty_raw += from.qty_raw;
    date_max = std::max(date_max, from.date_max);
    exact_sum += from.exact_sum;
    inexact_sum += from.inexact_sum;
    name_hash ^= from.name_hash;
    work_hash ^= from.work_hash;
    for (const auto &[flag, count] : from.flags) {
      flags[flag] += count;
    }
  }
};

} // namespace accelerator_codegen::aux_state::test
