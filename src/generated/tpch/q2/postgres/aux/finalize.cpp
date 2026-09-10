#include "tpch/q2/postgres/aux/module.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <string>
#include <vector>

namespace accelerator_codegen::aux_state::query {
namespace {

struct Q2OutputRow {
  int64_t acctbal_raw = 0;
  const std::string *supplier_name = nullptr;
  const std::string *nation_name = nullptr;
  int64_t partkey = 0;
  const std::string *mfgr = nullptr;
  const std::string *address = nullptr;
  const std::string *phone = nullptr;
  const std::string *comment = nullptr;
};

bool Q2RowBetter(const Q2OutputRow &lhs, const Q2OutputRow &rhs) {
  if (lhs.acctbal_raw != rhs.acctbal_raw) {
    return lhs.acctbal_raw > rhs.acctbal_raw;
  }
  if (*lhs.nation_name != *rhs.nation_name) {
    return *lhs.nation_name < *rhs.nation_name;
  }
  if (*lhs.supplier_name != *rhs.supplier_name) {
    return *lhs.supplier_name < *rhs.supplier_name;
  }
  return lhs.partkey < rhs.partkey;
}

} // namespace

void TpchQ2FinalizeAndEmit(const TpchQ2State &state, int32_t part_size,
                           const std::string &part_type_pattern,
                           const std::string &region_name,
                           core::AuxRowSink &sink) {
  const auto &regions = *state.region_names;
  int32_t region_key = -1;
  for (std::size_t i = 0; i < regions.size(); ++i) {
    if (regions[i] == region_name) {
      region_key = static_cast<int32_t>(i);
      break;
    }
  }
  if (region_key < 0) {
    core::OutputSchema schema;
    schema.columns = {{"s_acctbal", core::AuxType::ScaledDecimal, true},
                      {"s_name", core::AuxType::String, true},
                      {"n_name", core::AuxType::String, true},
                      {"p_partkey", core::AuxType::Int64, true},
                      {"p_mfgr", core::AuxType::String, true},
                      {"s_address", core::AuxType::String, true},
                      {"s_phone", core::AuxType::String, true},
                      {"s_comment", core::AuxType::String, true}};
    sink.Begin(schema);
    sink.End();
    return;
  }

  // Use a max-heap of the "best" rows; Q2 orders descending account balance,
  // ascending nation name/supplier name/partkey. A max heap on the comparator
  // gives us the top-k as its largest elements.
  auto less = [](const Q2OutputRow &lhs, const Q2OutputRow &rhs) {
    return Q2RowBetter(lhs, rhs);
  };
  std::priority_queue<Q2OutputRow, std::vector<Q2OutputRow>, decltype(less)>
      heap(less);

  // Use the size-index to skip all non-matching parts without touching the
  // full 200K part table.  This makes the hot path O(matching parts), not
  // O(total parts).
  const auto size_it = state.parts_by_size.find(part_size);
  if (size_it == state.parts_by_size.end()) {
    core::OutputSchema schema;
    schema.columns = {{"s_acctbal", core::AuxType::ScaledDecimal, true},
                      {"s_name", core::AuxType::String, true},
                      {"n_name", core::AuxType::String, true},
                      {"p_partkey", core::AuxType::Int64, true},
                      {"p_mfgr", core::AuxType::String, true},
                      {"s_address", core::AuxType::String, true},
                      {"s_phone", core::AuxType::String, true},
                      {"s_comment", core::AuxType::String, true}};
    sink.Begin(schema);
    sink.End();
    return;
  }
  const TpchQ2LikeAccelerator like_acc =
      TpchQ2BuildLikeAccelerator(part_type_pattern);
  const auto &nations = *state.nation_names;
  for (const auto partkey : size_it->second) {
    const auto *part = state.FindPart(partkey);
    if (!part) {
      continue;
    }
    if (!TpchQ2LikeAcceleratedMatch(like_acc, part->type)) {
      continue;
    }
    const auto key = TpchQ2CellKey(region_key, partkey);
    const auto *cell = state.FindRegionalMin(key);
    if (cell == nullptr) {
      continue;
    }
    for (const auto suppkey : cell->tied_suppkeys) {
      const auto *supplier = state.FindSupplier(suppkey);
      if (!supplier) {
        continue;
      }
      const auto nk = supplier->nationkey;
      if (nk < 0 || static_cast<std::size_t>(nk) >= nations.size() ||
          nations[static_cast<std::size_t>(nk)].empty()) {
        continue;
      }
      Q2OutputRow row;
      row.acctbal_raw = supplier->acctbal_raw;
      row.supplier_name = &supplier->name;
      row.nation_name = &nations[static_cast<std::size_t>(nk)];
      row.partkey = partkey;
      row.mfgr = &part->mfgr;
      row.address = &supplier->address;
      row.phone = &supplier->phone;
      row.comment = &supplier->comment;
      heap.push(row);
      if (heap.size() > kTpchQ2ResultLimit) {
        heap.pop();
      }
    }
  }

  std::vector<Q2OutputRow> rows;
  rows.reserve(heap.size());
  while (!heap.empty()) {
    rows.push_back(heap.top());
    heap.pop();
  }
  std::sort(rows.begin(), rows.end(),
            [](const Q2OutputRow &lhs, const Q2OutputRow &rhs) {
              return Q2RowBetter(lhs, rhs);
            });

  core::OutputSchema schema;
  schema.columns = {{"s_acctbal", core::AuxType::ScaledDecimal, true},
                    {"s_name", core::AuxType::String, true},
                    {"n_name", core::AuxType::String, true},
                    {"p_partkey", core::AuxType::Int64, true},
                    {"p_mfgr", core::AuxType::String, true},
                    {"s_address", core::AuxType::String, true},
                    {"s_phone", core::AuxType::String, true},
                    {"s_comment", core::AuxType::String, true}};
  sink.Begin(schema);
  for (const auto &row : rows) {
    core::AuxRow out;
    out.values.push_back(
        core::AuxValue::Of(core::AuxType::ScaledDecimal,
                           core::ScaledDecimal{row.acctbal_raw, 2}));
    out.values.push_back(
        core::AuxValue::Of(core::AuxType::String, *row.supplier_name));
    out.values.push_back(
        core::AuxValue::Of(core::AuxType::String, *row.nation_name));
    out.values.push_back(core::AuxValue::Of(core::AuxType::Int64, row.partkey));
    out.values.push_back(core::AuxValue::Of(core::AuxType::String, *row.mfgr));
    out.values.push_back(core::AuxValue::Of(core::AuxType::String, *row.address));
    out.values.push_back(core::AuxValue::Of(core::AuxType::String, *row.phone));
    out.values.push_back(core::AuxValue::Of(core::AuxType::String, *row.comment));
    sink.WriteRow(out);
  }
  sink.End();
}

} // namespace accelerator_codegen::aux_state::query
