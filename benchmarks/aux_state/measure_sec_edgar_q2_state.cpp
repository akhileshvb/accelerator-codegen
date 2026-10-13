// Footprint + read-path micro benchmark of the SEC-EDGAR Q2 aux state.
//   measure_sec_edgar_q2_state <db_path> [iterations]
// Prints build time, heap bytes / encoded bytes of the state, and the cost of
// the in-memory read path (FinalizeSecEdgarQ2) for several (fy, uom) draws.
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>

#include "accelerator_codegen/stateful/core/aux_calibration.hpp"
#include "accelerator_codegen/stateful/core/aux_registry.hpp"
#include "accelerator_codegen/stateful/duckdb/duckdb_aux_backend.hpp"
#include "accelerator_codegen/stateful/duckdb/duckdb_state_store.hpp"
#include "duckdb.hpp"
#include "sec_edgar/q2/duckdb/aux/duckdb_scan.hpp"
#include "sec_edgar/q2/duckdb/aux/module.hpp"

using namespace accelerator_codegen::aux_state::query;
using Clock = std::chrono::steady_clock;

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "usage: measure_sec_edgar_q2_state <db_path> [iterations]\n";
    return 2;
  }
  const int iterations = argc > 2 ? std::atoi(argv[2]) : 20000;
  const size_t flush_mb = argc > 3 ? std::atoi(argv[3]) : 0; // evict caches between writes
  duckdb::DuckDB db(argv[1]);
  duckdb::Connection con(db);
  con.Query("PRAGMA threads=" + std::string(std::getenv("ACCEL_BENCH_THREADS")
                                                ? std::getenv("ACCEL_BENCH_THREADS")
                                                : "1"));
  SecEdgarQ2DuckdbScan scan(con);
  SecEdgarQ2Config cfg{1, 2};
  if (const char *m = std::getenv("Q2_MAX_UOM_ROWS")) {
    cfg.max_uom_rows = std::strtoull(m, nullptr, 10);
  }
  if (const char *m = std::getenv("Q2_BUILD_THREADS")) {
    cfg.build_threads = static_cast<unsigned>(std::strtoul(m, nullptr, 10));
  }
  SecEdgarQ2Module module(cfg, &scan);
  const auto t0 = Clock::now();
  const auto state = module.BuildState();
  const double build_ms =
      std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  const auto bytes = EncodeSecEdgarQ2State(*state);
  uint64_t groups = 0, covered = 0;
  for (uint32_t u = 0; u < state->uoms->names.size(); ++u) {
    if (!state->uoms->excluded[u]) {
      ++covered;
    }
  }
  groups = state->LiveGroupCount();
  std::cout << "build_ms=" << std::fixed << std::setprecision(1) << build_ms
            << " heap_bytes=" << state->HeapBytes() << " encoded_bytes=" << bytes.size()
            << " covered_uoms=" << covered << "/" << state->uoms->names.size()
            << " groups=" << groups << " adsh=" << state->adsh.Size()
            << " tags=" << state->tag.Size() << "\n";
  uint64_t base_bytes = 0, base_groups = 0, escaped = 0;
  for (const auto &slice : state->slices) {
    if (slice) {
      base_bytes += slice->base->HeapBytes();
      base_groups += slice->base->n;
      escaped += slice->base->e_pos.size();
    }
  }
  std::cout << "base_bytes=" << base_bytes << " base_groups=" << base_groups
            << " escaped_counts=" << escaped
            << " dict_adsh=" << state->adsh.HeapBytes() << " dict_tag=" << state->tag.HeapBytes()
            << " fy=" << state->fy.MemoryBytes() << " names=" << state->names.MemoryBytes() << "\n";
  struct Draw {
    int64_t fy;
    const char *uom;
  };
  const Draw draws[] = {{2022, "pure"}, {2023, "pure"}, {2012, "pure"},
                        {2021, "pure"}, {2022, "EUR"},  {2022, "none"},
                        {2022, "USD"},  {2021, "USD"}, {2012, "USD"}, {2022, "shares"}};
  for (const auto &d : draws) {
    std::vector<SecEdgarQ2OutputRow> rows;
    FinalizeSecEdgarQ2(*state, d.fy, d.uom, rows);
    const auto s = Clock::now();
    size_t sink = 0;
    for (int i = 0; i < iterations; ++i) {
      FinalizeSecEdgarQ2(*state, d.fy, d.uom, rows);
      sink += rows.size();
    }
    const double us =
        std::chrono::duration<double, std::micro>(Clock::now() - s).count() / iterations;
    std::cout << "finalize fy=" << d.fy << " uom=" << d.uom << " rows=" << rows.size()
              << " us=" << std::setprecision(2) << us << (sink == 0 ? " (empty)" : "") << "\n";
  }

  {
    // cost of re-reading one group from the base table (the "lost group" path)
    auto q0 = con.Query("SELECT adsh, tag FROM num WHERE uom = 'pure' AND value IS NOT NULL LIMIT 1");
    std::vector<std::pair<std::string, std::string>> g{
        {q0->GetValue(0, 0).ToString(), q0->GetValue(1, 0).ToString()}};
    const auto l0 = Clock::now();
    size_t n = 0;
    scan.LookupNumValueCounts("pure", g, [&](const SecEdgarQ2ValueCountRow &) { ++n; });
    std::cout << "lookup of one group: ms="
              << std::chrono::duration<double, std::milli>(Clock::now() - l0).count()
              << " distinct values=" << n << "\n";
  }
  // ---- maintenance micro benchmark: bump the top 'pure' group of fy 2022.
  namespace core = accelerator_codegen::aux_state::core;
  namespace eng = accelerator_codegen::aux_state::engine::duckdb;
  eng::DuckDBStateStore store(SecEdgarQ2Module::kId);
  core::AuxTransactionCoordinator coordinator;
  const auto cost = core::LoadAuxCostModelJson("config/sec_edgar_q2_aux_cost_model.json");
  eng::DuckDBAuxBackend backend(
      coordinator, store, [](const duckdb::TableMutationEvent &) { return core::MutationBatch{}; },
      [] { return core::VisibilityToken{}; }, [cost] { return cost; }, [] { return 10.0; });
  auto shared_module = std::make_shared<SecEdgarQ2Module>(cfg, &scan);
  coordinator.Register(shared_module);
  {
    std::vector<core::StateImage> images;
    images.push_back(core::StateImage::Make(
        SecEdgarQ2Module::kId, SecEdgarQ2Module::kStatePartition, 0,
        std::shared_ptr<const SecEdgarQ2StateImage>(new SecEdgarQ2StateImage{state})));
    store.Install(std::move(images), SecEdgarQ2Module::kId);
  }
  std::vector<SecEdgarQ2OutputRow> top;
  FinalizeSecEdgarQ2(*state, 2022, "pure", top);
  if (top.empty()) {
    return 0;
  }
  // find the adsh of the top row
  auto q = con.Query("SELECT n.adsh, n.tag FROM num n JOIN sub s ON n.adsh = s.adsh WHERE "
                     "s.fy = 2022 AND n.uom = 'pure' AND n.value = " +
                     std::to_string(top[0].value) + " LIMIT 1");
  std::string adsh = q->GetValue(0, 0).ToString(), tag = q->GetValue(1, 0).ToString();
  const auto row = [&](double v) {
    using core::AuxType;
    using core::AuxValue;
    return core::AuxRow{{AuxValue::Of(AuxType::String, adsh), AuxValue::Of(AuxType::String, tag),
                         AuxValue::Of(AuxType::String, std::string("v")),
                         AuxValue::Of(AuxType::Int64, int64_t(20220101)),
                         AuxValue::Of(AuxType::Int64, int64_t(4)),
                         AuxValue::Of(AuxType::String, std::string("pure")),
                         AuxValue::Null(AuxType::String), AuxValue::Of(AuxType::Double, v),
                         AuxValue::Null(AuxType::String)}};
  };
  double v = top[0].value;
  const int writes = iterations / 10 + 1;
  const auto m0 = Clock::now();
  std::vector<char> flush(flush_mb << 20, 1);
  double maint_total = 0;
  for (int i = 0; i < writes; ++i) {
    if (flush_mb) {
      for (size_t k = 0; k < flush.size(); k += 64) {
        flush[k] = static_cast<char>(flush[k] + 1);
      }
    }
    const auto w0 = Clock::now();
    core::MutationBatch m;
    m.table = 1;
    m.kind = core::MutationKind::Update;
    m.updated_columns = {7};
    m.old_rows = {row(v)};
    v += 1.0;
    m.new_rows = {row(v)};
    coordinator.OnMutation(m);
    coordinator.PreCommit();
    coordinator.Commit();
    maint_total += std::chrono::duration<double, std::micro>(Clock::now() - w0).count();
  }
  (void)m0;
  const double maint_us = maint_total / writes;
  std::cout << "maintenance (bump of the top group) us/write=" << maint_us << "\n";
  auto snap = store.ReadPartitions({}, {SecEdgarQ2Module::kStatePartition});
  const auto *img = snap.partitions.front().As<SecEdgarQ2StateImage>();
  std::vector<SecEdgarQ2OutputRow> after;
  FinalizeSecEdgarQ2(*img->state, 2022, "pure", after);
  std::cout << "top value after " << writes << " bumps: " << after[0].value << " (expected " << v
            << ")\n";
  return 0;
}
