#include "tpch/q10/duckdb/aux/module.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
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

// One ranked stream over a month: either the shared base (skipping entries the
// overlay overrides) or the overlay's own ranking.
struct Stream {
  const TpchQ10MonthData *month = nullptr;
  bool base = true;
  std::size_t pos = 0;

  void Skip() {
    if (!base)
      return;
    const auto &b = *month->base;
    while (pos < b.rank.size() && month->InOverlay(b.keys[b.rank[pos]]))
      ++pos;
  }
  bool Done() const {
    return base ? pos >= month->base->rank.size() : pos >= month->ov_rank.size();
  }
  int64_t Key() const {
    return base ? month->base->keys[month->base->rank[pos]]
                : month->ov_keys[month->ov_rank[pos]];
  }
  int64_t Val() const {
    return base ? month->base->vals[month->base->rank[pos]]
                : month->ov_vals[month->ov_rank[pos]];
  }
};

// Exhaustive path (only if a negative revenue ever appears, which breaks the
// monotone bound used by the threshold algorithm).
std::vector<std::pair<int64_t, int64_t>>
FullTopK(const TpchQ10State &side,
         const std::vector<const TpchQ10MonthData *> &months,
         std::size_t limit) {
  std::unordered_map<int64_t, int64_t> totals;
  for (const auto *m : months)
    for (const auto &[key, val] : TpchQ10MonthEffective(*m))
      totals[key] += val;
  std::vector<std::pair<int64_t, int64_t>> ranked;
  for (const auto &entry : totals) {
    if (entry.second == 0 || !side.HasCustomer(entry.first))
      continue;
    ranked.emplace_back(entry.first, entry.second);
  }
  std::sort(ranked.begin(), ranked.end(), kQ10Better);
  if (ranked.size() > limit)
    ranked.resize(limit);
  return ranked;
}

} // namespace

std::vector<std::pair<int64_t, int64_t>>
TpchQ10SelectTopK(const TpchQ10State &side,
                  const std::vector<const TpchQ10MonthData *> &months,
                  std::size_t limit) {
  std::vector<std::pair<int64_t, int64_t>> out;
  if (limit == 0)
    return out;
  for (const auto *m : months)
    if (m->has_negative || m->base->has_negative)
      return FullTopK(side, months, limit);

  std::vector<Stream> streams;
  for (const auto *m : months) {
    streams.push_back({m, true, 0});
    streams.push_back({m, false, 0});
    streams.back().Skip();
    streams[streams.size() - 2].Skip();
  }

  // heap front = current worst of the best `limit` candidates.
  std::vector<std::pair<int64_t, int64_t>> heap;
  heap.reserve(limit + 1);
  std::unordered_set<int64_t> seen;
  seen.reserve(256);

  for (;;) {
    bool progress = false;
    for (auto &s : streams) {
      if (s.Done())
        continue;
      progress = true;
      const auto key = s.Key();
      ++s.pos;
      s.Skip();
      if (!seen.insert(key).second)
        continue;
      int64_t total = 0;
      for (const auto *m : months)
        total += m->Lookup(key);
      if (total <= 0 || !side.HasCustomer(key))
        continue;
      const std::pair<int64_t, int64_t> cand{key, total};
      if (heap.size() < limit) {
        heap.push_back(cand);
        std::push_heap(heap.begin(), heap.end(), kQ10Better);
      } else if (kQ10Better(cand, heap.front())) {
        std::pop_heap(heap.begin(), heap.end(), kQ10Better);
        heap.back() = cand;
        std::push_heap(heap.begin(), heap.end(), kQ10Better);
      }
    }
    if (!progress)
      break;
    if (heap.size() == limit) {
      // Any unseen customer's total is <= the sum of the stream heads.
      int64_t threshold = 0;
      for (std::size_t i = 0; i < streams.size(); i += 2) {
        int64_t head = 0;
        if (!streams[i].Done())
          head = streams[i].Val();
        if (!streams[i + 1].Done())
          head = std::max(head, streams[i + 1].Val());
        threshold += head;
      }
      if (heap.front().second > threshold)
        break;
    }
  }
  std::sort(heap.begin(), heap.end(), kQ10Better);
  return heap;
}

void TpchQ10FinalizeAndEmit(const TpchQ10State &side,
                            const std::vector<const TpchQ10MonthData *> &months,
                            core::AuxRowSink &sink, std::size_t limit) {
  const auto ranked = TpchQ10SelectTopK(side, months, limit);

  sink.Begin({{{"c_custkey", core::AuxType::Int64, false},
               {"c_name", core::AuxType::String, false},
               {"revenue", core::AuxType::ScaledDecimal, false},
               {"c_acctbal", core::AuxType::ScaledDecimal, false},
               {"n_name", core::AuxType::String, false},
               {"c_address", core::AuxType::String, false},
               {"c_phone", core::AuxType::String, false},
               {"c_comment", core::AuxType::String, false}}});
  const std::string empty;
  for (const auto &[custkey, revenue] : ranked) {
    const auto rank = side.RankOf(custkey);
    const auto *strings = side.CustomerOf(custkey);
    const auto nation = side.nations.find(rank.nationkey);
    const auto &name = strings ? strings->name : empty;
    const auto &address = strings ? strings->address : empty;
    const auto &phone = strings ? strings->phone : empty;
    const auto &comment = strings ? strings->comment : empty;
    sink.WriteRow(
        {{core::AuxValue::Of(core::AuxType::Int64, custkey),
          core::AuxValue::Of(core::AuxType::String, name),
          core::AuxValue::Of(core::AuxType::ScaledDecimal,
                             core::ScaledDecimal{revenue, 4}),
          core::AuxValue::Of(core::AuxType::ScaledDecimal,
                             core::ScaledDecimal{rank.acctbal_raw, 2}),
          core::AuxValue::Of(core::AuxType::String,
                             nation == side.nations.end() ? std::string()
                                                          : nation->second),
          core::AuxValue::Of(core::AuxType::String, address),
          core::AuxValue::Of(core::AuxType::String, phone),
          core::AuxValue::Of(core::AuxType::String, comment)}});
  }
  sink.End();
}

} // namespace accelerator_codegen::aux_state::query
