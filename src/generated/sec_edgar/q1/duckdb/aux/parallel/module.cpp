#include "sec_edgar/q1/duckdb/aux/parallel/module.hpp"

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
using accelerator_codegen::SecEdgarPreScan;
using accelerator_codegen::SecEdgarNumScan;
using accelerator_codegen::SecEdgarSubScan;
using accelerator_codegen::SecEdgarTagScan;
namespace {

void ApplyRow(SecEdgarQ1GroupAggregate &aggregate, int32_t line,
              const std::string &adsh, int8_t sign) {
  aggregate.cnt += sign;
  aggregate.line_sum += sign * static_cast<int64_t>(line);
  if (adsh.empty()) {
    return;
  }
  if (sign > 0) {
    aggregate.distinct_adsh.insert(adsh);
  } else if (sign < 0) {
    aggregate.distinct_adsh.erase(adsh);
  }
}

void MergeAggregate(SecEdgarQ1GroupAggregate &dst,
                    const SecEdgarQ1GroupAggregate &src) {
  dst.cnt += src.cnt;
  dst.line_sum += src.line_sum;
  dst.distinct_adsh.insert(src.distinct_adsh.begin(), src.distinct_adsh.end());
}

const SecEdgarQ1StateImage *AsImage(const core::StateImage &image,
                                    SecEdgarQ1ImageKind kind) {
  const auto *payload = image.As<SecEdgarQ1StateImage>();
  return payload && payload->kind == kind && payload->state ? payload : nullptr;
}

} // namespace

void SecEdgarQ1ParallelModule::Build(core::AuxEngineServices &services,
                                     core::StateBuilder &builder) {
  EmitState(BuildState(services), builder);
}

core::StagedMaintenanceOutcome SecEdgarQ1ParallelModule::StageMaintenance(
    core::AuxEngineServices &services, const core::InvalidationPlan &plan,
    core::AuxTxnBuffer &buffer) {
  return accelerator_codegen::AdoptParallelMaintenance(
      inner_.StageMaintenance(services, plan, buffer), kId, services);
}

bool SecEdgarQ1ParallelModule::CheckValidity(
    core::AuxEngineServices &services, const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void SecEdgarQ1ParallelModule::Execute(core::AuxEngineServices &services,
                                       const core::QueryArguments &arguments,
                                       core::AuxRowSink &sink) {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {core::SecEdgarQ1Layout::kGroupKeys,
                                     core::SecEdgarQ1Layout::kGroupAggregates});
  SecEdgarQ1State state;
  bool have_keys = false;
  bool have_aggregates = false;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == core::SecEdgarQ1Layout::kGroupKeys) {
      const auto *typed = AsImage(image, SecEdgarQ1ImageKind::GroupKeys);
      if (typed) {
        state.group_keys = typed->state->group_keys;
        have_keys = true;
      }
    } else if (image.partition == core::SecEdgarQ1Layout::kGroupAggregates) {
      const auto *typed = AsImage(image, SecEdgarQ1ImageKind::GroupAggregates);
      if (typed) {
        state.aggregates = typed->state->aggregates;
        have_aggregates = true;
      }
    }
  }
  if (!snapshot.available || !have_keys || !have_aggregates ||
      state.group_keys.size() != state.aggregates.size()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  EmitOutput(state, sink);
}

void SecEdgarQ1ParallelModule::ExecuteFallback(
    core::AuxEngineServices &services, const core::QueryArguments &,
    core::AuxRowSink &sink) {
  EmitOutput(BuildState(services), sink);
}

SecEdgarQ1State
SecEdgarQ1ParallelModule::BuildState(core::AuxEngineServices &services) const {
  const auto workers = std::max<std::size_t>(1, services.WorkerCount());
  std::vector<std::unordered_map<SecEdgarQ1GroupKey, SecEdgarQ1GroupAggregate,
                                 SecEdgarQ1GroupKeyHash>>
      local(workers);

  auto consume_pre = [&](duckdb::idx_t worker, duckdb::DataChunk &chunk) {
    auto &map = local[worker];
    for (duckdb::idx_t row = 0; row < chunk.size(); ++row) {
      if (ChunkNull(chunk, 0, row)) {
        continue;
      }
      SecEdgarQ1GroupKey key{ChunkString(chunk, 0, row), ChunkString(chunk, 1, row),
                             ChunkNull(chunk, 1, row)};
      ApplyRow(map[key], ChunkInt32(chunk, 3, row), ChunkString(chunk, 2, row), 1);
    }
  };

  const auto spec = SecEdgarPreScan({"stmt", "rfile", "adsh", "line"});
  if (!ScanSecEdgarParallel(services, spec, consume_pre)) {
    if (!initial_scan) {
      throw std::logic_error("SEC EDGAR Q1 parallel requires a scan source");
    }
    initial_scan->ScanPre([&](const SecEdgarQ1PreInput &input) {
      if (input.stmt_is_null) {
        return;
      }
      ApplyRow(local[0][{input.stmt, input.rfile, input.rfile_is_null}],
               input.line, input.adsh, 1);
    });
  }

  std::unordered_map<SecEdgarQ1GroupKey, SecEdgarQ1GroupAggregate,
                     SecEdgarQ1GroupKeyHash>
      merged;
  for (const auto &part : local) {
    for (const auto &[key, agg] : part) {
      MergeAggregate(merged[key], agg);
    }
  }

  SecEdgarQ1State state;
  state.group_keys.reserve(merged.size());
  for (const auto &[key, _] : merged) {
    state.group_keys.push_back(key);
  }
  std::sort(state.group_keys.begin(), state.group_keys.end());
  state.aggregates.resize(state.group_keys.size());
  for (std::size_t i = 0; i < state.group_keys.size(); ++i) {
    state.aggregates[i] = std::move(merged[state.group_keys[i]]);
  }
  return state;
}

void SecEdgarQ1ParallelModule::EmitOutput(const SecEdgarQ1State &state,
                                          core::AuxRowSink &sink) const {
  sink.Begin({{{"stmt", core::AuxType::String, false},
               {"rfile", core::AuxType::String, true},
               {"cnt", core::AuxType::Int64, false},
               {"num_filings", core::AuxType::Int64, false},
               {"avg_line_num", core::AuxType::Double, false}}});
  for (const auto &row : BuildSecEdgarQ1Output(state)) {
    sink.WriteRow(
        {{core::AuxValue::Of(core::AuxType::String, row.stmt),
          row.rfile_is_null
              ? core::AuxValue::Null(core::AuxType::String)
              : core::AuxValue::Of(core::AuxType::String, row.rfile),
          core::AuxValue::Of(core::AuxType::Int64, row.cnt),
          core::AuxValue::Of(core::AuxType::Int64, row.num_filings),
          core::AuxValue::Of(core::AuxType::Double, row.avg_line_num)}});
  }
  sink.End();
}

void SecEdgarQ1ParallelModule::EmitState(const SecEdgarQ1State &state,
                                         core::StateBuilder &builder) const {
  const auto typed = std::make_shared<const SecEdgarQ1State>(state);
  auto make = [&](SecEdgarQ1ImageKind kind)
      -> std::shared_ptr<const SecEdgarQ1StateImage> {
    auto image = std::make_shared<SecEdgarQ1StateImage>();
    image->kind = kind;
    image->state = typed;
    return image;
  };
  builder.Write(core::StateImage::Make(
      kId, core::SecEdgarQ1Layout::kGroupKeys, 0,
      make(SecEdgarQ1ImageKind::GroupKeys)));
  builder.Write(core::StateImage::Make(
      kId, core::SecEdgarQ1Layout::kGroupAggregates, 0,
      make(SecEdgarQ1ImageKind::GroupAggregates)));
  builder.Finish();
}

} // namespace accelerator_codegen::aux_state::query
