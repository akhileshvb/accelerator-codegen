// Q21's answer order, shared by its engines' modules: ORDER BY numwait DESC,
// s_name (bytewise) LIMIT 100, from per-name or per-supplier numwait totals.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace accelerator_codegen::aux_state::query {

inline constexpr std::size_t kTpchQ21Limit = 100;

// True when (numwait, name) comes first in the answer.
inline bool TpchQ21RanksBefore(int64_t lhs_numwait, std::string_view lhs_name,
                               int64_t rhs_numwait, std::string_view rhs_name) {
  if (lhs_numwait != rhs_numwait)
    return lhs_numwait > rhs_numwait;
  return lhs_name < rhs_name;
}

// The first kTpchQ21Limit groups in answer order, from (name, numwait) groups
// with distinct names (`Name` points at a string): a bounded heap whose top is
// the worst group kept. `Row` is {std::string name; int64_t numwait;}.
template <class Row, class Name>
std::vector<Row> TpchQ21TopGroups(const std::vector<std::pair<Name, int64_t>> &groups) {
  using Group = std::pair<Name, int64_t>;
  const auto before = [](const Group &lhs, const Group &rhs) {
    return TpchQ21RanksBefore(lhs.second, std::string_view(*lhs.first), rhs.second,
                              std::string_view(*rhs.first));
  };
  std::vector<Group> heap;
  heap.reserve(std::min(groups.size(), kTpchQ21Limit));
  for (const auto &group : groups) {
    if (heap.size() < kTpchQ21Limit) {
      heap.push_back(group);
      std::push_heap(heap.begin(), heap.end(), before);
    } else if (before(group, heap.front())) {
      std::pop_heap(heap.begin(), heap.end(), before);
      heap.back() = group;
      std::push_heap(heap.begin(), heap.end(), before);
    }
  }
  std::sort_heap(heap.begin(), heap.end(), before);
  std::vector<Row> rows;
  rows.reserve(heap.size());
  for (const auto &group : heap)
    rows.push_back({std::string(*group.first), group.second});
  return rows;
}

// Q21 groups by s_name, not s_suppkey: suppliers that share a name form one
// output group whose numwait is the sum of their counts.
template <class Row>
std::vector<Row> TpchQ21MergeByNameAndTop(
    const std::vector<std::pair<std::string_view, int64_t>> &rows) {
  std::unordered_map<std::string_view, int64_t> by_name;
  by_name.reserve(rows.size());
  for (const auto &[name, numwait] : rows)
    by_name[name] += numwait;
  std::vector<std::pair<const std::string_view *, int64_t>> groups;
  groups.reserve(by_name.size());
  for (const auto &[name, numwait] : by_name)
    if (numwait > 0)
      groups.emplace_back(&name, numwait);
  return TpchQ21TopGroups<Row>(groups);
}

} // namespace accelerator_codegen::aux_state::query
