#include "sec_edgar/q2/aux/parallel/module.hpp"

#include "accelerator_codegen/runtime/parallel/aux_parallel_scan.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace accelerator_codegen::aux_state::query {
using accelerator_codegen::ChunkDouble;
using accelerator_codegen::ChunkInt32;
using accelerator_codegen::ChunkNull;
using accelerator_codegen::ChunkString;
using accelerator_codegen::ScanSecEdgarParallel;
using accelerator_codegen::SecEdgarNumScan;
using accelerator_codegen::SecEdgarSubScan;

namespace {
constexpr int32_t kTargetFy = 2022;

const SecEdgarQ2StateImage *AsImage(const core::StateImage &image,
                                    SecEdgarQ2ImageKind kind) {
  const auto *payload = image.As<SecEdgarQ2StateImage>();
  return payload && payload->kind == kind && payload->state ? payload : nullptr;
}
} // namespace

void SecEdgarQ2ParallelModule::Build(core::AuxEngineServices &services,
                                     core::StateBuilder &builder) {
  EmitState(BuildState(services), builder);
}

core::StagedMaintenanceOutcome SecEdgarQ2ParallelModule::StageMaintenance(
    core::AuxEngineServices &services, const core::InvalidationPlan &plan,
    core::AuxTxnBuffer &buffer) {
  return accelerator_codegen::AdoptParallelMaintenance(
      inner_.StageMaintenance(services, plan, buffer), kId, services);
}

bool SecEdgarQ2ParallelModule::CheckValidity(core::AuxEngineServices &services,
                                             const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void SecEdgarQ2ParallelModule::Execute(core::AuxEngineServices &services,
                                       const core::QueryArguments &arguments,
                                       core::AuxRowSink &sink) {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(),
      {core::SecEdgarQ2Layout::kMaxPureMap, core::SecEdgarQ2Layout::kFy2022Cohort,
       core::SecEdgarQ2Layout::kCandidates});
  SecEdgarQ2State state;
  bool have_max = false;
  bool have_cohort = false;
  bool have_candidates = false;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == core::SecEdgarQ2Layout::kMaxPureMap) {
      const auto *typed = AsImage(image, SecEdgarQ2ImageKind::MaxPureMap);
      if (typed) {
        state.max_by_adsh_tag = typed->state->max_by_adsh_tag;
        have_max = true;
      }
    } else if (image.partition == core::SecEdgarQ2Layout::kFy2022Cohort) {
      const auto *typed = AsImage(image, SecEdgarQ2ImageKind::Fy2022Cohort);
      if (typed) {
        state.adsh_to_name = typed->state->adsh_to_name;
        have_cohort = true;
      }
    } else if (image.partition == core::SecEdgarQ2Layout::kCandidates) {
      const auto *typed = AsImage(image, SecEdgarQ2ImageKind::Candidates);
      if (typed) {
        state.candidates = typed->state->candidates;
        have_candidates = true;
      }
    }
  }
  if (!snapshot.available || !have_max || !have_cohort || !have_candidates) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  EmitOutput(state, sink);
}

void SecEdgarQ2ParallelModule::ExecuteFallback(core::AuxEngineServices &services,
                                               const core::QueryArguments &,
                                               core::AuxRowSink &sink) {
  EmitOutput(BuildState(services), sink);
}

SecEdgarQ2State
SecEdgarQ2ParallelModule::BuildState(core::AuxEngineServices &services) const {
  const auto workers = std::max<std::size_t>(1, services.WorkerCount());
  std::vector<std::unordered_map<std::string, std::string>> local_names(workers);
  auto consume_sub = [&](duckdb::idx_t worker, duckdb::DataChunk &chunk) {
    auto &map = local_names[worker];
    for (duckdb::idx_t row = 0; row < chunk.size(); ++row) {
      if (ChunkInt32(chunk, 2, row) != kTargetFy) {
        continue;
      }
      map.emplace(ChunkString(chunk, 0, row), ChunkString(chunk, 1, row));
    }
  };
  if (!ScanSecEdgarParallel(services, SecEdgarSubScan({"adsh", "name", "fy"}),
                            consume_sub)) {
    if (!initial_scan) {
      throw std::logic_error("SEC EDGAR Q2 parallel requires a scan source");
    }
    initial_scan->ScanSub([&](const SecEdgarQ2SubInput &input) {
      if (input.fy == kTargetFy) {
        local_names[0].emplace(input.adsh, input.name);
      }
    });
  }

  SecEdgarQ2State state;
  for (const auto &part : local_names) {
    state.adsh_to_name.insert(part.begin(), part.end());
  }

  std::vector<std::unordered_map<SecEdgarQ2AdshTagKey, double, SecEdgarQ2AdshTagKeyHash>>
      local_max(workers);
  auto consume_max = [&](duckdb::idx_t worker, duckdb::DataChunk &chunk) {
    auto &map = local_max[worker];
    for (duckdb::idx_t row = 0; row < chunk.size(); ++row) {
      if (ChunkString(chunk, 2, row) != "pure") {
        continue;
      }
      bool value_null = false;
      const double value = ChunkDouble(chunk, 3, row, value_null);
      if (value_null) {
        continue;
      }
      const SecEdgarQ2AdshTagKey key{ChunkString(chunk, 0, row),
                                     ChunkString(chunk, 1, row)};
      auto it = map.find(key);
      if (it == map.end() || value > it->second) {
        map[key] = value;
      }
    }
  };
  const auto num_spec = SecEdgarNumScan({"adsh", "tag", "uom", "value"});
  if (!ScanSecEdgarParallel(services, num_spec, consume_max)) {
    initial_scan->ScanNum([&](const SecEdgarQ2NumInput &input) {
      if (input.uom != "pure" || input.value_is_null) {
        return;
      }
      const SecEdgarQ2AdshTagKey key{input.adsh, input.tag};
      auto it = local_max[0].find(key);
      if (it == local_max[0].end() || input.value > it->second) {
        local_max[0][key] = input.value;
      }
    });
  }
  for (const auto &part : local_max) {
    for (const auto &[key, value] : part) {
      auto it = state.max_by_adsh_tag.find(key);
      if (it == state.max_by_adsh_tag.end() || value > it->second) {
        state.max_by_adsh_tag[key] = value;
      }
    }
  }

  const auto &adsh_to_name = state.adsh_to_name;
  const auto &max_by_adsh_tag = state.max_by_adsh_tag;
  std::vector<std::vector<SecEdgarQ2CandidateRow>> local_candidates(workers);
  auto consume_cand = [&](duckdb::idx_t worker, duckdb::DataChunk &chunk) {
    auto &rows = local_candidates[worker];
    for (duckdb::idx_t row = 0; row < chunk.size(); ++row) {
      if (ChunkString(chunk, 2, row) != "pure") {
        continue;
      }
      bool value_null = false;
      const double value = ChunkDouble(chunk, 3, row, value_null);
      if (value_null) {
        continue;
      }
      const auto adsh = ChunkString(chunk, 0, row);
      const auto name_it = adsh_to_name.find(adsh);
      if (name_it == adsh_to_name.end()) {
        continue;
      }
      const auto tag = ChunkString(chunk, 1, row);
      const auto max_it = max_by_adsh_tag.find({adsh, tag});
      if (max_it == max_by_adsh_tag.end() || max_it->second != value) {
        continue;
      }
      rows.push_back({name_it->second, tag, value});
    }
  };
  if (!ScanSecEdgarParallel(services, num_spec, consume_cand)) {
    initial_scan->ScanNum([&](const SecEdgarQ2NumInput &input) {
      if (input.uom != "pure" || input.value_is_null) {
        return;
      }
      const auto name_it = state.adsh_to_name.find(input.adsh);
      if (name_it == state.adsh_to_name.end()) {
        return;
      }
      const auto max_it = state.max_by_adsh_tag.find({input.adsh, input.tag});
      if (max_it == state.max_by_adsh_tag.end() || max_it->second != input.value) {
        return;
      }
      local_candidates[0].push_back({name_it->second, input.tag, input.value});
    });
  }
  for (auto &part : local_candidates) {
    state.candidates.insert(state.candidates.end(),
                            std::make_move_iterator(part.begin()),
                            std::make_move_iterator(part.end()));
  }
  return state;
}

void SecEdgarQ2ParallelModule::EmitOutput(const SecEdgarQ2State &state,
                                          core::AuxRowSink &sink) const {
  sink.Begin({{{"name", core::AuxType::String, false},
               {"tag", core::AuxType::String, false},
               {"value", core::AuxType::Double, false}}});
  for (const auto &row : BuildSecEdgarQ2Output(state)) {
    sink.WriteRow({{core::AuxValue::Of(core::AuxType::String, row.name),
                    core::AuxValue::Of(core::AuxType::String, row.tag),
                    core::AuxValue::Of(core::AuxType::Double, row.value)}});
  }
  sink.End();
}

void SecEdgarQ2ParallelModule::EmitState(const SecEdgarQ2State &state,
                                         core::StateBuilder &builder) const {
  const auto typed = std::make_shared<const SecEdgarQ2State>(state);
  auto make = [&](SecEdgarQ2ImageKind kind)
      -> std::shared_ptr<const SecEdgarQ2StateImage> {
    auto image = std::make_shared<SecEdgarQ2StateImage>();
    image->kind = kind;
    image->state = typed;
    return image;
  };
  builder.Write(core::StateImage::Make(
      kId, core::SecEdgarQ2Layout::kMaxPureMap, 0,
      make(SecEdgarQ2ImageKind::MaxPureMap)));
  builder.Write(core::StateImage::Make(
      kId, core::SecEdgarQ2Layout::kFy2022Cohort, 0,
      make(SecEdgarQ2ImageKind::Fy2022Cohort)));
  builder.Write(core::StateImage::Make(
      kId, core::SecEdgarQ2Layout::kCandidates, 0,
      make(SecEdgarQ2ImageKind::Candidates)));
  builder.Finish();
}

} // namespace accelerator_codegen::aux_state::query
