#include "tpch/q2/duckdb/aux/module.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <string>
#include <vector>

namespace accelerator_codegen::aux_state::query {
namespace {

core::OutputSchema Q2Schema() {
  core::OutputSchema schema;
  schema.columns = {{"s_acctbal", core::AuxType::ScaledDecimal, true},
                    {"s_name", core::AuxType::String, true},
                    {"n_name", core::AuxType::String, true},
                    {"p_partkey", core::AuxType::Int64, true},
                    {"p_mfgr", core::AuxType::String, true},
                    {"s_address", core::AuxType::String, true},
                    {"s_phone", core::AuxType::String, true},
                    {"s_comment", core::AuxType::String, true}};
  return schema;
}

} // namespace

void TpchQ2FinalizeAndEmit(const TpchQ2State &state, int32_t part_size,
                           const std::string &part_type_pattern,
                           const std::string &region_name,
                           core::AuxRowSink &sink) {
  const auto &regions = *state.region_names;
  int32_t region_key = -1;
  for (std::size_t i = 0; i < regions.size(); ++i) {
    if (!regions[i].empty() && regions[i] == region_name) {
      region_key = static_cast<int32_t>(i);
      break;
    }
  }
  const TpchQ2GroupList *group =
      region_key < 0 ? nullptr : state.FindGroup(part_size, region_key);

  // Walk the precomputed candidate list (already in output order) and stop
  // once kTpchQ2ResultLimit rows pass the type predicate.  LIKE results are
  // cached per interned type id in a local (per-call) vector.
  std::vector<TpchQ2Entry> picked;
  picked.reserve(kTpchQ2ResultLimit);
  if (group != nullptr && state.types) {
    const auto &type_names = state.types->names;
    const TpchQ2LikeAccelerator like_acc =
        TpchQ2BuildLikeAccelerator(part_type_pattern);
    std::vector<int8_t> type_ok(type_names.size(), -1);
    for (const auto &chunk : group->chunks) {
      for (const auto &e : chunk->v) {
        const auto tid = static_cast<std::size_t>(e.type_id);
        if (tid >= type_ok.size())
          continue;
        int8_t &ok = type_ok[tid];
        if (ok < 0) {
          // An empty pattern is a LIKE '' (matches only the empty string).
          ok = (part_type_pattern.empty()
                    ? type_names[tid].empty()
                    : TpchQ2LikeAcceleratedMatch(like_acc, type_names[tid]))
                   ? 1
                   : 0;
        }
        if (!ok)
          continue;
        picked.push_back(e);
        if (picked.size() >= kTpchQ2ResultLimit)
          break;
      }
      if (picked.size() >= kTpchQ2ResultLimit)
        break;
    }
  }

  sink.Begin(Q2Schema());
  const auto &nations = *state.nation_names;
  for (const auto &e : picked) {
    const auto *supplier = state.FindSupplier(e.suppkey);
    const auto *part = state.FindPart(e.partkey);
    if (!supplier || !part)
      continue;
    const auto nk = supplier->nationkey;
    if (nk < 0 || static_cast<std::size_t>(nk) >= nations.size())
      continue;
    core::AuxRow out;
    out.values.push_back(core::AuxValue::Of(
        core::AuxType::ScaledDecimal,
        core::ScaledDecimal{supplier->acctbal_raw, 2}));
    out.values.push_back(
        core::AuxValue::Of(core::AuxType::String, supplier->name));
    out.values.push_back(core::AuxValue::Of(
        core::AuxType::String, nations[static_cast<std::size_t>(nk)]));
    out.values.push_back(
        core::AuxValue::Of(core::AuxType::Int64, static_cast<int64_t>(e.partkey)));
    out.values.push_back(core::AuxValue::Of(core::AuxType::String, part->mfgr));
    out.values.push_back(
        core::AuxValue::Of(core::AuxType::String, supplier->address));
    out.values.push_back(
        core::AuxValue::Of(core::AuxType::String, supplier->phone));
    out.values.push_back(
        core::AuxValue::Of(core::AuxType::String, supplier->comment));
    sink.WriteRow(out);
  }
  sink.End();
}

} // namespace accelerator_codegen::aux_state::query
