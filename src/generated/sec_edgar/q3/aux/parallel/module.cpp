#include "sec_edgar/q3/aux/parallel/module.hpp"

#include "accelerator_codegen/runtime/parallel/aux_parallel_scan.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace accelerator_codegen::aux_state::query {
using accelerator_codegen::ChunkDouble;
using accelerator_codegen::ChunkInt32;
using accelerator_codegen::ChunkString;
using accelerator_codegen::ScanSecEdgarParallel;
using accelerator_codegen::SecEdgarNumScan;
using accelerator_codegen::SecEdgarSubScan;

namespace {
constexpr int32_t kTargetFy = 2022;

const SecEdgarQ3StateImage *AsImage(const core::StateImage &image,
                                    SecEdgarQ3ImageKind kind) {
  const auto *payload = image.As<SecEdgarQ3StateImage>();
  return payload && payload->kind == kind && payload->state ? payload : nullptr;
}

uint32_t FindOrCreateGroup(SecEdgarQ3State &state, const std::string &name,
                           int32_t cik) {
  for (uint32_t group_id = 0; group_id < state.group_names.size(); ++group_id) {
    if (state.group_names[group_id] == name && state.group_ciks[group_id] == cik) {
      return group_id;
    }
  }
  const uint32_t group_id = static_cast<uint32_t>(state.group_names.size());
  state.group_names.push_back(name);
  state.group_ciks.push_back(cik);
  state.group_sums.push_back(0.0);
  return group_id;
}
} // namespace

void SecEdgarQ3ParallelModule::Build(core::AuxEngineServices &services,
                                     core::StateBuilder &builder) {
  EmitState(BuildState(services), builder);
}

core::StagedMaintenanceOutcome SecEdgarQ3ParallelModule::StageMaintenance(
    core::AuxEngineServices &services, const core::InvalidationPlan &plan,
    core::AuxTxnBuffer &buffer) {
  return accelerator_codegen::AdoptParallelMaintenance(
      inner_.StageMaintenance(services, plan, buffer), kId, services);
}

bool SecEdgarQ3ParallelModule::CheckValidity(core::AuxEngineServices &services,
                                             const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void SecEdgarQ3ParallelModule::Execute(core::AuxEngineServices &services,
                                       const core::QueryArguments &arguments,
                                       core::AuxRowSink &sink) {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(),
      {core::SecEdgarQ3Layout::kFy2022Cohort, core::SecEdgarQ3Layout::kCikTotals});
  SecEdgarQ3State state;
  bool have_cohort = false;
  bool have_totals = false;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == core::SecEdgarQ3Layout::kFy2022Cohort) {
      const auto *typed = AsImage(image, SecEdgarQ3ImageKind::Fy2022Cohort);
      if (typed) {
        state.adsh_to_filing = typed->state->adsh_to_filing;
        have_cohort = true;
      }
    } else if (image.partition == core::SecEdgarQ3Layout::kCikTotals) {
      const auto *typed = AsImage(image, SecEdgarQ3ImageKind::CikTotals);
      if (typed) {
        state.cik_totals = typed->state->cik_totals;
        state.group_names = typed->state->group_names;
        state.group_ciks = typed->state->group_ciks;
        state.group_sums = typed->state->group_sums;
        have_totals = true;
      }
    }
  }
  if (!snapshot.available || !have_cohort || !have_totals ||
      state.group_names.size() != state.group_sums.size()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  EmitOutput(state, sink);
}

void SecEdgarQ3ParallelModule::ExecuteFallback(core::AuxEngineServices &services,
                                               const core::QueryArguments &,
                                               core::AuxRowSink &sink) {
  EmitOutput(BuildState(services), sink);
}

SecEdgarQ3State
SecEdgarQ3ParallelModule::BuildState(core::AuxEngineServices &services) const {
  const auto workers = std::max<std::size_t>(1, services.WorkerCount());
  std::vector<std::unordered_map<std::string, SecEdgarQ3FilingInfo>> local_sub(workers);
  auto consume_sub = [&](duckdb::idx_t worker, duckdb::DataChunk &chunk) {
    auto &map = local_sub[worker];
    for (duckdb::idx_t row = 0; row < chunk.size(); ++row) {
      if (ChunkInt32(chunk, 3, row) != kTargetFy) {
        continue;
      }
      map.emplace(ChunkString(chunk, 0, row),
                  SecEdgarQ3FilingInfo{ChunkString(chunk, 1, row),
                                       ChunkInt32(chunk, 2, row), 0});
    }
  };
  if (!ScanSecEdgarParallel(services, SecEdgarSubScan({"adsh", "name", "cik", "fy"}),
                            consume_sub)) {
    if (!initial_scan) {
      throw std::logic_error("SEC EDGAR Q3 parallel requires a scan source");
    }
    initial_scan->ScanSub([&](const SecEdgarQ3SubInput &input) {
      if (input.fy == kTargetFy) {
        local_sub[0].emplace(input.adsh,
                             SecEdgarQ3FilingInfo{input.name, input.cik, 0});
      }
    });
  }

  SecEdgarQ3State state;
  for (auto &part : local_sub) {
    for (auto &[adsh, info] : part) {
      info.group_id = FindOrCreateGroup(state, info.name, info.cik);
      state.adsh_to_filing.emplace(adsh, info);
    }
  }

  std::vector<std::unordered_map<int32_t, double>> local_totals(workers);
  std::vector<std::vector<double>> local_group_sums(workers);
  for (auto &sums : local_group_sums) {
    sums.assign(state.group_sums.size(), 0.0);
  }
  const auto &adsh_to_filing = state.adsh_to_filing;
  auto consume_num = [&](duckdb::idx_t worker, duckdb::DataChunk &chunk) {
    auto &totals = local_totals[worker];
    auto &groups = local_group_sums[worker];
    for (duckdb::idx_t row = 0; row < chunk.size(); ++row) {
      if (ChunkString(chunk, 1, row) != "USD") {
        continue;
      }
      bool value_null = false;
      const double value = ChunkDouble(chunk, 2, row, value_null);
      if (value_null) {
        continue;
      }
      const auto it = adsh_to_filing.find(ChunkString(chunk, 0, row));
      if (it == adsh_to_filing.end()) {
        continue;
      }
      totals[it->second.cik] += value;
      if (it->second.group_id < groups.size()) {
        groups[it->second.group_id] += value;
      }
    }
  };
  if (!ScanSecEdgarParallel(services, SecEdgarNumScan({"adsh", "uom", "value"}),
                            consume_num)) {
    initial_scan->ScanNum([&](const SecEdgarQ3NumInput &input) {
      if (input.uom != "USD" || input.value_is_null) {
        return;
      }
      const auto it = state.adsh_to_filing.find(input.adsh);
      if (it == state.adsh_to_filing.end()) {
        return;
      }
      local_totals[0][it->second.cik] += input.value;
      if (it->second.group_id < local_group_sums[0].size()) {
        local_group_sums[0][it->second.group_id] += input.value;
      }
    });
  }
  for (const auto &part : local_totals) {
    for (const auto &[cik, total] : part) {
      state.cik_totals[cik] += total;
    }
  }
  for (const auto &part : local_group_sums) {
    for (std::size_t i = 0; i < part.size(); ++i) {
      state.group_sums[i] += part[i];
    }
  }
  return state;
}

void SecEdgarQ3ParallelModule::EmitOutput(const SecEdgarQ3State &state,
                                          core::AuxRowSink &sink) const {
  sink.Begin({{{"name", core::AuxType::String, false},
               {"cik", core::AuxType::Int64, false},
               {"total_value", core::AuxType::Double, false}}});
  for (const auto &row : BuildSecEdgarQ3Output(state)) {
    sink.WriteRow({{core::AuxValue::Of(core::AuxType::String, row.name),
                    core::AuxValue::Of(core::AuxType::Int64, static_cast<int64_t>(row.cik)),
                    core::AuxValue::Of(core::AuxType::Double, row.total_value)}});
  }
  sink.End();
}

void SecEdgarQ3ParallelModule::EmitState(const SecEdgarQ3State &state,
                                         core::StateBuilder &builder) const {
  const auto typed = std::make_shared<const SecEdgarQ3State>(state);
  auto make = [&](SecEdgarQ3ImageKind kind)
      -> std::shared_ptr<const SecEdgarQ3StateImage> {
    auto image = std::make_shared<SecEdgarQ3StateImage>();
    image->kind = kind;
    image->state = typed;
    return image;
  };
  builder.Write(core::StateImage::Make(
      kId, core::SecEdgarQ3Layout::kFy2022Cohort, 0,
      make(SecEdgarQ3ImageKind::Fy2022Cohort)));
  builder.Write(core::StateImage::Make(
      kId, core::SecEdgarQ3Layout::kCikTotals, 0,
      make(SecEdgarQ3ImageKind::CikTotals)));
  builder.Finish();
}

} // namespace accelerator_codegen::aux_state::query
