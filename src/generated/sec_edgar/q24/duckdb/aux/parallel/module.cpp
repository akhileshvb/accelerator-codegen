#include "sec_edgar/q24/duckdb/aux/parallel/module.hpp"

#include "accelerator_codegen/runtime/parallel/aux_parallel_scan.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace accelerator_codegen::aux_state::query {
using accelerator_codegen::ChunkDouble;
using accelerator_codegen::ChunkInt32;
using accelerator_codegen::ChunkString;
using accelerator_codegen::ScanSecEdgarParallel;
using accelerator_codegen::SecEdgarNumScan;
using accelerator_codegen::SecEdgarPreScan;

namespace {
constexpr int32_t kFirstDate = 20230101;
constexpr int32_t kLastDate = 20231231;

const SecEdgarQ24StateImage *AsImage(const core::StateImage &image,
                                     SecEdgarQ24ImageKind kind) {
  const auto *payload = image.As<SecEdgarQ24StateImage>();
  return payload && payload->kind == kind && payload->state ? payload : nullptr;
}
} // namespace

void SecEdgarQ24ParallelModule::Build(core::AuxEngineServices &services,
                                      core::StateBuilder &builder) {
  EmitState(BuildState(services), builder);
}

core::StagedMaintenanceOutcome SecEdgarQ24ParallelModule::StageMaintenance(
    core::AuxEngineServices &services, const core::InvalidationPlan &plan,
    core::AuxTxnBuffer &buffer) {
  return accelerator_codegen::AdoptParallelMaintenance(
      inner_.StageMaintenance(services, plan, buffer), kId, services);
}

bool SecEdgarQ24ParallelModule::CheckValidity(core::AuxEngineServices &services,
                                              const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void SecEdgarQ24ParallelModule::Execute(core::AuxEngineServices &services,
                                        const core::QueryArguments &arguments,
                                        core::AuxRowSink &sink) {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {core::SecEdgarQ24Layout::kTagVersions,
                                     core::SecEdgarQ24Layout::kGroupAggregates});
  SecEdgarQ24State state;
  bool have_tags = false;
  bool have_aggregates = false;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == core::SecEdgarQ24Layout::kTagVersions) {
      const auto *typed = AsImage(image, SecEdgarQ24ImageKind::TagVersions);
      if (typed) {
        state.tag_versions = typed->state->tag_versions;
        have_tags = true;
      }
    } else if (image.partition == core::SecEdgarQ24Layout::kGroupAggregates) {
      const auto *typed = AsImage(image, SecEdgarQ24ImageKind::Aggregates);
      if (typed) {
        state.aggregates = typed->state->aggregates;
        have_aggregates = true;
      }
    }
  }
  if (!snapshot.available || !have_tags || !have_aggregates ||
      state.tag_versions.size() != state.aggregates.size()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  EmitOutput(state, sink);
}

void SecEdgarQ24ParallelModule::ExecuteFallback(core::AuxEngineServices &services,
                                                const core::QueryArguments &,
                                                core::AuxRowSink &sink) {
  EmitOutput(BuildState(services), sink);
}

SecEdgarQ24State
SecEdgarQ24ParallelModule::BuildState(core::AuxEngineServices &services) const {
  const auto workers = std::max<std::size_t>(1, services.WorkerCount());
  const auto num_spec =
      SecEdgarNumScan({"adsh", "tag", "version", "uom", "value", "ddate"});

  std::vector<std::set<SecEdgarQ24TagVersionKey>> local_groups(workers);
  auto consume_groups = [&](duckdb::idx_t worker, duckdb::DataChunk &chunk) {
    auto &groups = local_groups[worker];
    for (duckdb::idx_t row = 0; row < chunk.size(); ++row) {
      if (ChunkString(chunk, 3, row) != "USD") {
        continue;
      }
      bool value_null = false;
      (void)ChunkDouble(chunk, 4, row, value_null);
      if (value_null) {
        continue;
      }
      const auto ddate = ChunkInt32(chunk, 5, row);
      if (ddate < kFirstDate || ddate > kLastDate) {
        continue;
      }
      groups.insert({ChunkString(chunk, 1, row), ChunkString(chunk, 2, row)});
    }
  };
  if (!ScanSecEdgarParallel(services, num_spec, consume_groups)) {
    if (!initial_scan) {
      throw std::logic_error("SEC EDGAR Q24 parallel requires a scan source");
    }
    initial_scan->ScanNum([&](const SecEdgarQ24NumInput &input) {
      if (input.uom == "USD" && !input.value_is_null && input.ddate >= kFirstDate &&
          input.ddate <= kLastDate) {
        local_groups[0].insert({input.tag, input.version});
      }
    });
  }

  SecEdgarQ24State state;
  std::set<SecEdgarQ24TagVersionKey> groups;
  for (const auto &part : local_groups) {
    groups.insert(part.begin(), part.end());
  }
  state.tag_versions.assign(groups.begin(), groups.end());
  state.aggregates.resize(state.tag_versions.size());
  std::unordered_map<SecEdgarQ24TagVersionKey, uint32_t, SecEdgarQ24TagVersionKeyHash>
      group_index;
  group_index.reserve(state.tag_versions.size() * 2);
  for (uint32_t i = 0; i < state.tag_versions.size(); ++i) {
    group_index.emplace(state.tag_versions[i], i);
  }

  const auto &group_ids = group_index;
  std::vector<SecEdgarQ24PreMembership> local_pre(workers);
  auto consume_pre = [&](duckdb::idx_t worker, duckdb::DataChunk &chunk) {
    auto &membership = local_pre[worker];
    for (duckdb::idx_t row = 0; row < chunk.size(); ++row) {
      const SecEdgarQ24TagVersionKey key{ChunkString(chunk, 1, row),
                                         ChunkString(chunk, 2, row)};
      if (group_ids.find(key) == group_ids.end()) {
        continue;
      }
      membership.Insert(ChunkString(chunk, 0, row), key.tag, key.version);
    }
  };
  if (!ScanSecEdgarParallel(services, SecEdgarPreScan({"adsh", "tag", "version"}),
                            consume_pre)) {
    if (!initial_scan) {
      throw std::logic_error("SEC EDGAR Q24 parallel requires a scan source");
    }
    initial_scan->ScanPre([&](const SecEdgarQ24PreInput &input) {
      if (group_index.find({input.tag, input.version}) == group_index.end()) {
        return;
      }
      local_pre[0].Insert(input.adsh, input.tag, input.version);
    });
  }
  for (auto &part : local_pre) {
    for (uint32_t adsh_id = 0; adsh_id < part.adshs.size(); ++adsh_id) {
      if (adsh_id >= part.by_adsh.size()) {
        continue;
      }
      for (const auto tv_id : part.by_adsh[adsh_id]) {
        if (tv_id >= part.tvs.size()) {
          continue;
        }
        const auto &tv = part.tvs[tv_id];
        state.pre_membership.Insert(part.adshs[adsh_id], tv.tag, tv.version);
      }
    }
  }
  state.pre_membership.Finalize();

  std::vector<std::vector<SecEdgarQ24Aggregate>> local_agg(workers);
  for (auto &part : local_agg) {
    part.assign(state.aggregates.size(), {});
  }
  const auto &membership = state.pre_membership;
  auto consume_num = [&](duckdb::idx_t worker, duckdb::DataChunk &chunk) {
    auto &aggs = local_agg[worker];
    for (duckdb::idx_t row = 0; row < chunk.size(); ++row) {
      if (ChunkString(chunk, 3, row) != "USD") {
        continue;
      }
      bool value_null = false;
      const double value = ChunkDouble(chunk, 4, row, value_null);
      if (value_null) {
        continue;
      }
      const auto ddate = ChunkInt32(chunk, 5, row);
      if (ddate < kFirstDate || ddate > kLastDate) {
        continue;
      }
      const auto adsh = ChunkString(chunk, 0, row);
      const auto tag = ChunkString(chunk, 1, row);
      const auto version = ChunkString(chunk, 2, row);
      if (membership.Contains(adsh, tag, version)) {
        continue;
      }
      const auto group_it = group_ids.find({tag, version});
      if (group_it != group_ids.end()) {
        aggs[group_it->second].count++;
        aggs[group_it->second].total += value;
      }
    }
  };
  if (!ScanSecEdgarParallel(services, num_spec, consume_num)) {
    initial_scan->ScanNum([&](const SecEdgarQ24NumInput &input) {
      if (input.uom != "USD" || input.value_is_null || input.ddate < kFirstDate ||
          input.ddate > kLastDate) {
        return;
      }
      if (state.pre_membership.Contains(input.adsh, input.tag, input.version)) {
        return;
      }
      const auto group_it = group_index.find({input.tag, input.version});
      if (group_it != group_index.end()) {
        local_agg[0][group_it->second].count++;
        local_agg[0][group_it->second].total += input.value;
      }
    });
  }
  for (const auto &part : local_agg) {
    for (std::size_t i = 0; i < part.size(); ++i) {
      state.aggregates[i].count += part[i].count;
      state.aggregates[i].total += part[i].total;
    }
  }
  return state;
}

void SecEdgarQ24ParallelModule::EmitOutput(const SecEdgarQ24State &state,
                                           core::AuxRowSink &sink) const {
  sink.Begin({{{"tag", core::AuxType::String, false},
               {"version", core::AuxType::String, false},
               {"cnt", core::AuxType::Int64, false},
               {"total", core::AuxType::Double, false}}});
  for (const auto &row : BuildSecEdgarQ24Output(state)) {
    sink.WriteRow({{core::AuxValue::Of(core::AuxType::String, row.tag),
                    core::AuxValue::Of(core::AuxType::String, row.version),
                    core::AuxValue::Of(core::AuxType::Int64, row.count),
                    core::AuxValue::Of(core::AuxType::Double, row.total)}});
  }
  sink.End();
}

void SecEdgarQ24ParallelModule::EmitState(const SecEdgarQ24State &state,
                                          core::StateBuilder &builder) const {
  const auto typed = std::make_shared<const SecEdgarQ24State>(state);
  auto make = [&](SecEdgarQ24ImageKind kind)
      -> std::shared_ptr<const SecEdgarQ24StateImage> {
    auto image = std::make_shared<SecEdgarQ24StateImage>();
    image->kind = kind;
    image->state = typed;
    return image;
  };
  builder.Write(core::StateImage::Make(
      kId, core::SecEdgarQ24Layout::kPreMembership, 0,
      make(SecEdgarQ24ImageKind::PreMembership)));
  builder.Write(core::StateImage::Make(
      kId, core::SecEdgarQ24Layout::kTagVersions, 0,
      make(SecEdgarQ24ImageKind::TagVersions)));
  builder.Write(core::StateImage::Make(kId, core::SecEdgarQ24Layout::kGroupAggregates,
                                       0, make(SecEdgarQ24ImageKind::Aggregates)));
  builder.Finish();
}

} // namespace accelerator_codegen::aux_state::query
