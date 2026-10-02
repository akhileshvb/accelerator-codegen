#include "sec_edgar/q6/duckdb/aux/parallel/module.hpp"

#include "accelerator_codegen/runtime/parallel/aux_parallel_scan.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include "sec_edgar/q6/layout.hpp"

namespace accelerator_codegen::aux_state::query {
using accelerator_codegen::ChunkDouble;
using accelerator_codegen::ChunkInt32;
using accelerator_codegen::ChunkString;
using accelerator_codegen::ScanSecEdgarParallel;
using accelerator_codegen::SecEdgarNumScan;
using accelerator_codegen::SecEdgarPreScan;
using accelerator_codegen::SecEdgarSubScan;

namespace {
constexpr int32_t kTargetFy = 2023;

const SecEdgarQ6StateImage *AsImage(const core::StateImage &image,
                                    SecEdgarQ6ImageKind kind) {
  const auto *payload = image.As<SecEdgarQ6StateImage>();
  return payload && payload->kind == kind && payload->state ? payload : nullptr;
}

void ApplyNumContribution(SecEdgarQ6State &state, const std::string &adsh,
                          const std::string &tag, const std::string &version,
                          double value) {
  const auto name_it = state.adsh_to_name.find(adsh);
  if (name_it == state.adsh_to_name.end()) {
    return;
  }
  const auto pre_it = state.pre_plabels.find({adsh, tag, version});
  if (pre_it == state.pre_plabels.end()) {
    return;
  }
  for (const auto &plabel : pre_it->second) {
    auto &aggregate = state.aggregates[{name_it->second, tag, plabel}];
    aggregate.sum += value;
    aggregate.count += 1;
  }
}
} // namespace

void SecEdgarQ6ParallelModule::Build(core::AuxEngineServices &services,
                                     core::StateBuilder &builder) {
  EmitState(BuildState(services), builder);
}

core::StagedMaintenanceOutcome SecEdgarQ6ParallelModule::StageMaintenance(
    core::AuxEngineServices &services, const core::InvalidationPlan &plan,
    core::AuxTxnBuffer &buffer) {
  return accelerator_codegen::AdoptParallelMaintenance(
      inner_.StageMaintenance(services, plan, buffer), kId, services);
}

bool SecEdgarQ6ParallelModule::CheckValidity(core::AuxEngineServices &services,
                                             const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void SecEdgarQ6ParallelModule::Execute(core::AuxEngineServices &services,
                                       const core::QueryArguments &arguments,
                                       core::AuxRowSink &sink) {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(),
      {core::SecEdgarQ6Layout::kFy2023Cohort, core::SecEdgarQ6Layout::kPreIsBridge,
       core::SecEdgarQ6Layout::kGroupAggregates});
  SecEdgarQ6State state;
  bool have_cohort = false;
  bool have_pre = false;
  bool have_aggregates = false;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == core::SecEdgarQ6Layout::kFy2023Cohort) {
      const auto *typed = AsImage(image, SecEdgarQ6ImageKind::Fy2023Cohort);
      if (typed) {
        state.adsh_to_name = typed->state->adsh_to_name;
        have_cohort = true;
      }
    } else if (image.partition == core::SecEdgarQ6Layout::kPreIsBridge) {
      const auto *typed = AsImage(image, SecEdgarQ6ImageKind::PreIsBridge);
      if (typed) {
        state.pre_plabels = typed->state->pre_plabels;
        have_pre = true;
      }
    } else if (image.partition == core::SecEdgarQ6Layout::kGroupAggregates) {
      const auto *typed = AsImage(image, SecEdgarQ6ImageKind::GroupAggregates);
      if (typed) {
        state.aggregates = typed->state->aggregates;
        have_aggregates = true;
      }
    }
  }
  if (!snapshot.available || !have_cohort || !have_pre || !have_aggregates) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  EmitOutput(state, sink);
}

void SecEdgarQ6ParallelModule::ExecuteFallback(core::AuxEngineServices &services,
                                               const core::QueryArguments &,
                                               core::AuxRowSink &sink) {
  EmitOutput(BuildState(services), sink);
}

SecEdgarQ6State
SecEdgarQ6ParallelModule::BuildState(core::AuxEngineServices &services) const {
  const auto workers = std::max<std::size_t>(1, services.WorkerCount());
  std::vector<std::unordered_map<std::string, std::string>> local_names(workers);
  auto consume_sub = [&](duckdb::idx_t worker, duckdb::DataChunk &chunk) {
    auto &map = local_names[worker];
    for (duckdb::idx_t row = 0; row < chunk.size(); ++row) {
      if (ChunkInt32(chunk, 2, row) == kTargetFy) {
        map.emplace(ChunkString(chunk, 0, row), ChunkString(chunk, 1, row));
      }
    }
  };
  if (!ScanSecEdgarParallel(services, SecEdgarSubScan({"adsh", "name", "fy"}),
                            consume_sub)) {
    if (!initial_scan) {
      throw std::logic_error("SEC EDGAR Q6 parallel requires a scan source");
    }
    initial_scan->ScanSub([&](const SecEdgarQ6SubInput &input) {
      if (input.fy == kTargetFy) {
        local_names[0].emplace(input.adsh, input.name);
      }
    });
  }

  SecEdgarQ6State state;
  for (const auto &part : local_names) {
    state.adsh_to_name.insert(part.begin(), part.end());
  }

  const auto &adsh_to_name = state.adsh_to_name;
  std::vector<std::unordered_map<SecEdgarQ6PreKey, std::vector<std::string>,
                                 SecEdgarQ6PreKeyHash>>
      local_pre(workers);
  auto consume_pre = [&](duckdb::idx_t worker, duckdb::DataChunk &chunk) {
    auto &map = local_pre[worker];
    for (duckdb::idx_t row = 0; row < chunk.size(); ++row) {
      if (ChunkString(chunk, 3, row) != "IS") {
        continue;
      }
      const auto adsh = ChunkString(chunk, 0, row);
      if (adsh_to_name.find(adsh) == adsh_to_name.end()) {
        continue;
      }
      map[{adsh, ChunkString(chunk, 1, row), ChunkString(chunk, 2, row)}].push_back(
          ChunkString(chunk, 4, row));
    }
  };
  if (!ScanSecEdgarParallel(
          services, SecEdgarPreScan({"adsh", "tag", "version", "stmt", "plabel"}),
          consume_pre)) {
    initial_scan->ScanPre([&](const SecEdgarQ6PreInput &input) {
      if (input.stmt != "IS" ||
          state.adsh_to_name.find(input.adsh) == state.adsh_to_name.end()) {
        return;
      }
      local_pre[0][{input.adsh, input.tag, input.version}].push_back(input.plabel);
    });
  }
  for (auto &part : local_pre) {
    for (auto &[key, labels] : part) {
      auto &dst = state.pre_plabels[key];
      dst.insert(dst.end(), labels.begin(), labels.end());
    }
  }

  std::vector<SecEdgarQ6State> local_agg(workers);
  for (auto &part : local_agg) {
    part.adsh_to_name = state.adsh_to_name;
    part.pre_plabels = state.pre_plabels;
  }
  auto consume_num = [&](duckdb::idx_t worker, duckdb::DataChunk &chunk) {
    auto &local = local_agg[worker];
    for (duckdb::idx_t row = 0; row < chunk.size(); ++row) {
      if (ChunkString(chunk, 3, row) != "USD") {
        continue;
      }
      bool value_null = false;
      const double value = ChunkDouble(chunk, 4, row, value_null);
      if (value_null) {
        continue;
      }
      ApplyNumContribution(local, ChunkString(chunk, 0, row),
                           ChunkString(chunk, 1, row), ChunkString(chunk, 2, row),
                           value);
    }
  };
  if (!ScanSecEdgarParallel(
          services, SecEdgarNumScan({"adsh", "tag", "version", "uom", "value"}),
          consume_num)) {
    initial_scan->ScanNum([&](const SecEdgarQ6NumInput &input) {
      if (input.uom != "USD" || input.value_is_null) {
        return;
      }
      ApplyNumContribution(local_agg[0], input.adsh, input.tag, input.version,
                           input.value);
    });
  }
  for (auto &part : local_agg) {
    for (const auto &[key, agg] : part.aggregates) {
      auto &dst = state.aggregates[key];
      dst.sum += agg.sum;
      dst.count += agg.count;
    }
  }
  return state;
}

void SecEdgarQ6ParallelModule::EmitOutput(const SecEdgarQ6State &state,
                                          core::AuxRowSink &sink) const {
  sink.Begin({{{"name", core::AuxType::String, false},
               {"stmt", core::AuxType::String, false},
               {"tag", core::AuxType::String, false},
               {"plabel", core::AuxType::String, false},
               {"total_value", core::AuxType::Double, false},
               {"cnt", core::AuxType::Int64, false}}});
  for (const auto &row : BuildSecEdgarQ6Output(state)) {
    sink.WriteRow({{core::AuxValue::Of(core::AuxType::String, row.name),
                    core::AuxValue::Of(core::AuxType::String, row.stmt),
                    core::AuxValue::Of(core::AuxType::String, row.tag),
                    core::AuxValue::Of(core::AuxType::String, row.plabel),
                    core::AuxValue::Of(core::AuxType::Double, row.total_value),
                    core::AuxValue::Of(core::AuxType::Int64, row.cnt)}});
  }
  sink.End();
}

void SecEdgarQ6ParallelModule::EmitState(const SecEdgarQ6State &state,
                                         core::StateBuilder &builder) const {
  const auto typed = std::make_shared<const SecEdgarQ6State>(state);
  auto make = [&](SecEdgarQ6ImageKind kind)
      -> std::shared_ptr<const SecEdgarQ6StateImage> {
    auto image = std::make_shared<SecEdgarQ6StateImage>();
    image->kind = kind;
    image->state = typed;
    return image;
  };
  builder.Write(core::StateImage::Make(
      kId, core::SecEdgarQ6Layout::kFy2023Cohort, 0,
      make(SecEdgarQ6ImageKind::Fy2023Cohort)));
  builder.Write(core::StateImage::Make(
      kId, core::SecEdgarQ6Layout::kPreIsBridge, 0,
      make(SecEdgarQ6ImageKind::PreIsBridge)));
  builder.Write(core::StateImage::Make(
      kId, core::SecEdgarQ6Layout::kGroupAggregates, 0,
      make(SecEdgarQ6ImageKind::GroupAggregates)));
  builder.Finish();
}

} // namespace accelerator_codegen::aux_state::query
