#include "sec_edgar/q1/duckdb/aux/parallel/module.hpp"

#include "backends/duckdb/api/parallel/aux_parallel_scan.hpp"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace accelerator_codegen::aux_state::query {

void SecEdgarQ1ParallelModule::Build(core::AuxEngineServices &services,
                                     core::StateBuilder &builder) {
  auto state = std::make_shared<const SecEdgarQ1State>(BuildState(services));
  auto image = std::make_shared<SecEdgarQ1StateImage>();
  image->state = std::move(state);
  builder.Write(core::StateImage::Make(
      kId, SecEdgarQ1Module::kStatePartition, 0,
      std::shared_ptr<const SecEdgarQ1StateImage>(std::move(image))));
  builder.Finish();
}

core::StagedMaintenanceOutcome SecEdgarQ1ParallelModule::StageMaintenance(
    core::AuxEngineServices &services, const core::InvalidationPlan &,
    core::AuxTxnBuffer &buffer) {
  auto outcome = inner_.StageForAccelerator(services, buffer, kId);
  if (outcome.action == core::MaintenanceAction::ApplyDelta) {
    // The delta is applied to the shared typed state; no scan is involved, so
    // it is safe (and far cheaper than a parallel rebuild) on every engine.
    return outcome;
  }
  return accelerator_codegen::AdoptParallelMaintenance(std::move(outcome), kId,
                                                       services);
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
  const auto state = inner_.LoadState(services, nullptr, kId);
  if (!state) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  SecEdgarQ1Module::EmitOutput(*state, sink);
}

SecEdgarQ1State
SecEdgarQ1ParallelModule::BuildState(core::AuxEngineServices &services) const {
  const auto workers = std::max<std::size_t>(1, services.WorkerCount());
  std::vector<q1::Builder> local(workers);
  const auto spec = SecEdgarPreScan({"stmt", "rfile", "adsh", "line"});
  const bool scanned = ScanSecEdgarParallel(
      services, spec, [&](duckdb::idx_t worker, duckdb::DataChunk &chunk) {
        local[worker].ConsumeChunk(chunk);
      });
  if (!scanned) {
    if (!initial_scan) {
      throw std::logic_error("SEC EDGAR Q1 parallel requires a scan source");
    }
    const bool chunked = initial_scan->ScanPreChunks(
        [&](duckdb::DataChunk &chunk) { local[0].ConsumeChunk(chunk); });
    if (!chunked) {
      initial_scan->ScanPre(
          [&](const SecEdgarQ1PreInput &input) { local[0].ConsumeRow(input); });
    }
  }
  for (std::size_t i = 1; i < local.size(); ++i) {
    local[0].Merge(std::move(local[i]));
  }
  return local[0].Finish(&services);
}

} // namespace accelerator_codegen::aux_state::query
