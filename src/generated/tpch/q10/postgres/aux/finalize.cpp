#include "tpch/q10/postgres/aux/module.hpp"

#include <algorithm>
#include <unordered_map>
#include <utility>
#include <vector>

namespace accelerator_codegen::aux_state::query {
namespace {

const auto kQ10Better = [](const std::pair<int64_t, int64_t> &lhs,
                           const std::pair<int64_t, int64_t> &rhs) {
  if (lhs.second != rhs.second)
    return lhs.second > rhs.second;
  return lhs.first < rhs.first;
};

} // namespace

std::vector<std::pair<int64_t, int64_t>> TpchQ10SelectTopK(
    const core::DenseIdArray<TpchQ10CustomerRank> &ranks,
    const std::vector<const std::unordered_map<int64_t, int64_t> *>
        &month_revenues,
    std::size_t limit) {
  std::unordered_map<int64_t, int64_t> totals;
  for (const auto *month_map : month_revenues) {
    if (!month_map)
      continue;
    for (const auto &entry : *month_map)
      totals[entry.first] += entry.second;
  }

  std::vector<std::pair<int64_t, int64_t>> ranked;
  ranked.reserve(totals.size());
  for (const auto &entry : totals) {
    if (entry.second == 0)
      continue;
    if (ranks.Get(entry.first).present == 0)
      continue;
    ranked.emplace_back(entry.first, entry.second);
  }

  if (limit == 0) {
    ranked.clear();
  } else if (ranked.size() > limit) {
    std::partial_sort(ranked.begin(),
                      ranked.begin() + static_cast<std::ptrdiff_t>(limit),
                      ranked.end(), kQ10Better);
    ranked.resize(limit);
  } else {
    std::sort(ranked.begin(), ranked.end(), kQ10Better);
  }
  return ranked;
}

void TpchQ10FinalizeAndEmit(
    const core::DenseIdArray<TpchQ10CustomerRank> &ranks,
    const std::unordered_map<int32_t, std::string> &nations,
    const std::vector<const std::unordered_map<int64_t, int64_t> *>
        &month_revenues,
    const std::unordered_map<int64_t, TpchQ10Customer> &customer_strings,
    int32_t /*start_days*/, core::AuxRowSink &sink, std::size_t limit) {
  const auto ranked = TpchQ10SelectTopK(ranks, month_revenues, limit);

  sink.Begin({{{"c_custkey", core::AuxType::Int64, false},
               {"c_name", core::AuxType::String, false},
               {"revenue", core::AuxType::ScaledDecimal, false},
               {"c_acctbal", core::AuxType::ScaledDecimal, false},
               {"n_name", core::AuxType::String, false},
               {"c_address", core::AuxType::String, false},
               {"c_phone", core::AuxType::String, false},
               {"c_comment", core::AuxType::String, false}}});
  for (const auto &[custkey, revenue] : ranked) {
    const auto rank = ranks.Get(custkey);
    const auto strings = customer_strings.find(custkey);
    const auto nation = nations.find(rank.nationkey);
    const std::string empty;
    const auto &name = strings == customer_strings.end() ? empty : strings->second.name;
    const auto &address =
        strings == customer_strings.end() ? empty : strings->second.address;
    const auto &phone = strings == customer_strings.end() ? empty : strings->second.phone;
    const auto &comment =
        strings == customer_strings.end() ? empty : strings->second.comment;
    sink.WriteRow(
        {{core::AuxValue::Of(core::AuxType::Int64, custkey),
          core::AuxValue::Of(core::AuxType::String, name),
          core::AuxValue::Of(core::AuxType::ScaledDecimal,
                             core::ScaledDecimal{revenue, 4}),
          core::AuxValue::Of(core::AuxType::ScaledDecimal,
                             core::ScaledDecimal{rank.acctbal_raw, 2}),
          core::AuxValue::Of(core::AuxType::String,
                             nation == nations.end() ? std::string()
                                                     : nation->second),
          core::AuxValue::Of(core::AuxType::String, address),
          core::AuxValue::Of(core::AuxType::String, phone),
          core::AuxValue::Of(core::AuxType::String, comment)}});
  }
  sink.End();
}

void TpchQ10FinalizeAndEmit(const TpchQ10State &state, int32_t start_days,
                            core::AuxRowSink &sink) {
  const auto end_days = TpchQ10AddMonths(start_days, 3);
  std::vector<const std::unordered_map<int64_t, int64_t> *> month_revenues;
  for (int32_t cursor = start_days; cursor < end_days;) {
    const auto month = TpchQ10MonthKeyFromDays(cursor);
    const auto it = state.month_revenue.find(month);
    if (it != state.month_revenue.end())
      month_revenues.push_back(&it->second);
    const auto next = TpchQ10AddMonths(TpchQ10MonthStartDays(month), 1);
    if (next <= cursor)
      break;
    cursor = next;
  }
  TpchQ10FinalizeAndEmit(state.customer_ranks, state.nations, month_revenues, {},
                         start_days, sink, kTpchQ10ResultLimit);
}

} // namespace accelerator_codegen::aux_state::query
