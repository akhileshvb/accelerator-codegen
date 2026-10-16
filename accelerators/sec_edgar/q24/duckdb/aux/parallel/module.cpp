#include "sec_edgar/q24/duckdb/aux/parallel/module.hpp"

#include "backends/duckdb/api/parallel/aux_parallel_scan.hpp"
#include "sec_edgar/q24/duckdb/base/query.hpp"

#include <algorithm>
#include <vector>

namespace accelerator_codegen::aux_state::query {
namespace {

// Zero-copy string column view over a DataChunk vector.
struct ChunkStrings {
  duckdb::UnifiedVectorFormat fmt;
  const duckdb::string_t *data = nullptr;
  void Init(duckdb::Vector &vec, duckdb::idx_t n) {
    vec.ToUnifiedFormat(n, fmt);
    data = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(fmt);
  }
  Q24Str At(duckdb::idx_t i) const {
    const auto idx = fmt.sel->get_index(i);
    if (!fmt.validity.RowIsValid(idx)) return Q24Str::Null();
    return Q24Str::Of(data[idx].GetData(), data[idx].GetSize());
  }
};

} // namespace

void SecEdgarQ24ParallelModule::Build(core::AuxEngineServices &services,
                                      core::StateBuilder &builder) {
  SecEdgarQ24Module::EmitStateImages(kId, BuildState(services), builder);
}

core::StagedMaintenanceOutcome SecEdgarQ24ParallelModule::StageMaintenance(
    core::AuxEngineServices &services, const core::InvalidationPlan &plan,
    core::AuxTxnBuffer &buffer) {
  auto outcome = inner_.StageMaintenance(services, plan, buffer);
  // The shared incremental state is exact, so (unlike the other SEC EDGAR
  // parallel modules) ApplyDelta payloads are published as-is under this
  // module's accelerator id.
  for (auto &image : outcome.updates) image.accelerator = kId;
  return outcome;
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
  std::string bucket;
  if (!SecEdgarQ24Module::ParseArguments(arguments, bucket)) {
    SecEdgarQ24Module::EmitOutput(Q24GroupState(), std::string(), sink);
    return;
  }
  std::shared_ptr<const Q24GroupState> groups;
  if (!inner_.LoadGroups(services, groups)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  SecEdgarQ24Module::EmitOutput(*groups, bucket, sink);
}

SecEdgarQ24State
SecEdgarQ24ParallelModule::BuildState(core::AuxEngineServices &services) const {
  const auto workers = std::max<std::size_t>(1, services.WorkerCount());

  // 1. pre -> exact membership base (worker-local dictionaries, then merge).
  std::vector<Q24RawPre> parts(workers);
  const bool pre_ok = ScanSecEdgarParallel(
      services, CreateSecEdgarQ24PreScanSpec(),
      [&](duckdb::idx_t worker, duckdb::DataChunk &chunk) {
        ChunkStrings a, t, v;
        a.Init(chunk.data[0], chunk.size());
        t.Init(chunk.data[1], chunk.size());
        v.Init(chunk.data[2], chunk.size());
        auto &part = parts[worker];
        for (duckdb::idx_t i = 0; i < chunk.size(); ++i)
          part.Add(a.At(i), t.At(i), v.At(i));
      });
  if (!pre_ok) return inner_.BuildState(services); // no native scan context

  SecEdgarQ24State state;
  auto pre = std::make_shared<Q24PreState>();
  pre->base = BuildQ24PreBase(parts, &services);
  const Q24PreBase &base = *pre->base;

  // 2. qualifying num rows without a pre match (read-only probes per worker).
  std::vector<Q24OrphanAccumulator> accs(workers);
  ScanSecEdgarParallel(
      services, CreateSecEdgarQ24AuxNumScanSpec(),
      [&](duckdb::idx_t worker, duckdb::DataChunk &chunk) {
        const auto n = chunk.size();
        ChunkStrings a, t, v, uom;
        a.Init(chunk.data[0], n);
        t.Init(chunk.data[1], n);
        v.Init(chunk.data[2], n);
        uom.Init(chunk.data[4], n);
        duckdb::UnifiedVectorFormat value_fmt, date_fmt;
        chunk.data[3].ToUnifiedFormat(n, value_fmt);
        chunk.data[5].ToUnifiedFormat(n, date_fmt);
        const auto *values = duckdb::UnifiedVectorFormat::GetData<double>(value_fmt);
        const auto *dates = duckdb::UnifiedVectorFormat::GetData<int32_t>(date_fmt);
        auto &acc = accs[worker];
        for (duckdb::idx_t i = 0; i < n; ++i) {
          // Pushed-down filters are re-checked: cheap and keeps the result
          // exact even if a scan implementation only applies them partially.
          const auto vi = value_fmt.sel->get_index(i);
          const auto di = date_fmt.sel->get_index(i);
          if (!value_fmt.validity.RowIsValid(vi) ||
              !date_fmt.validity.RowIsValid(di))
            continue;
          int32_t year;
          if (!Q24DdateYear(dates[di], year)) continue;
          const Q24Str u = uom.At(i);
          if (u.null) continue;
          const Q24Str as = a.At(i), ts = t.At(i), vs = v.At(i);
          if (!base.Contains(as, ts, vs)) acc.Add(as, ts, vs, year, u, values[vi]);
        }
      });
  for (std::size_t w = 1; w < workers; ++w) accs[0].MergeFrom(accs[w]);
  state.pre = std::move(pre);
  state.groups = accs[0].Finish();
  return state;
}

} // namespace accelerator_codegen::aux_state::query
