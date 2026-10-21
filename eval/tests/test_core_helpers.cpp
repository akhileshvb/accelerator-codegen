// Unit tests of the shared module helpers: AuxRow cells, MaintenanceRequest
// factories, FinishRebuildAll(decision), ParallelEachWhen, RunStarts,
// ParallelStableSort and SingleRowBatch.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "substrate/aux/core/aux_cells.hpp"
#include "substrate/aux/core/aux_maintenance_policy.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/parallel_sort.hpp"
#include "substrate/aux/core/partitioned_build.hpp"

using namespace accelerator_codegen::aux_state;
using maintenance::MaintenanceAction;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " +     \
                               #cond);                                                             \
    }                                                                                              \
  } while (0)

namespace {

class FakeServices final : public core::AuxEngineServices {
public:
  explicit FakeServices(std::size_t workers_p) : workers(workers_p) {}
  core::VisibilityToken CurrentVisibility() const override { throw std::logic_error("unused"); }
  core::AuxStateStore &StateStore() override { throw std::logic_error("unused"); }
  std::size_t WorkerCount() const override { return workers; }
  void ParallelFor(std::size_t n, const std::function<void(std::size_t)> &fn) const override {
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < n; ++i) {
      threads.emplace_back([&fn, i] { fn(i); });
    }
    for (auto &thread : threads) {
      thread.join();
    }
  }

private:
  std::size_t workers;
};

template <class T> core::AuxValue Cell(core::AuxType type, T payload) {
  return core::AuxValue::Of(type, std::move(payload));
}

void TestCells() {
  core::AuxRow row;
  row.values = {Cell(core::AuxType::Int64, int64_t{-7}),
                Cell(core::AuxType::UInt64, uint64_t{1} << 63),
                Cell(core::AuxType::Date, core::AuxDate{9000}),
                Cell(core::AuxType::String, std::string("AB")),
                Cell(core::AuxType::ScaledDecimal, core::ScaledDecimal{12345, 2}),
                Cell(core::AuxType::ScaledDecimal128, core::ScaledDecimal128{(__int128)1 << 100, 4}),
                core::AuxValue::Null(core::AuxType::Int64)};
  int64_t i = 0;
  int32_t days = 0, scale = 0;
  std::string s;
  char c = 0;
  double d = 0;
  __int128 wide = 0;
  CHECK(core::IntCell(row, 0, i) && i == -7);
  CHECK(!core::IntCell(row, 1, i));
  CHECK(core::IntCell(row, 2, i) && i == 9000);
  CHECK(core::DateCell(row, 2, days) && days == 9000);
  CHECK(!core::DateCell(row, 3, days));
  CHECK(core::StringCell(row, 3, s) && s == "AB");
  CHECK(core::CharCell(row, 3, c) && c == 'A');
  CHECK(!core::StringCell(row, 0, s));
  CHECK(core::DecimalCell(row, 4, i, scale) && i == 12345 && scale == 2);
  CHECK(core::DecimalCellAtScale(row, 4, 2, i) && !core::DecimalCellAtScale(row, 4, 3, i));
  CHECK(core::DoubleCell(row, 4, d) && d > 123.4499 && d < 123.4501);
  CHECK(core::WideDecimalCell(row, 5, wide, scale) && wide == ((__int128)1 << 100) && scale == 4);
  CHECK(core::WideDecimalCell(row, 4, wide, scale) && wide == 12345 && scale == 2);
  CHECK(!core::IntCell(row, 6, i) && !core::IntCell(row, 99, i));
  CHECK(core::ScaleToRaw(123, 2, 4) == 12300 && core::ScaleToRaw(-129, 2, 1) == -12 &&
        core::ScaleToRaw(5, 3, 3) == 5);
}

void TestMaintenance() {
  const auto rebuild = core::MaintenanceRequest::RebuildOnly(3.0);
  CHECK(!rebuild.incremental_supported && rebuild.full_rebuild_supported &&
        rebuild.whole_state_required && rebuild.expected_queries_before_next_write == 3.0);
  const auto single = core::MaintenanceRequest::IncrementalSingleState(50, 4.0);
  CHECK(single.rows_touched == 50 && single.affected_partition_count == 1 &&
        single.incremental_supported && !single.partition_rebuild_supported &&
        !single.whole_state_required);
  const auto partitioned = core::MaintenanceRequest::IncrementalPartitioned(50, 6, 4.0);
  CHECK(partitioned.affected_partition_count == 6 && partitioned.partition_rebuild_supported &&
        partitioned.incremental_supported && !partitioned.whole_state_required);
  CHECK(partitioned.deferred_rebuild_supported && partitioned.data_rows_hint == 0);

  for (const auto action :
       {MaintenanceAction::ApplyDelta, MaintenanceAction::RebuildPartition,
        MaintenanceAction::RebuildAll, MaintenanceAction::InvalidateAndRebuildOnRead}) {
    core::StagedMaintenanceOutcome outcome;
    core::MaintenanceDecision decision;
    decision.action = action;
    core::FinishRebuildAll(outcome, decision);
    CHECK(outcome.action == (action == MaintenanceAction::InvalidateAndRebuildOnRead
                                 ? action
                                 : MaintenanceAction::RebuildAll));
    CHECK(outcome.updates.empty());
  }
}

void TestParallel() {
  for (const std::size_t workers : {1, 4}) {
    FakeServices services(workers);
    for (const std::size_t work : {10, 1000}) {
      std::vector<std::atomic<int>> hits(7);
      std::atomic<std::size_t> pool_tasks{0};
      core::ParallelEachWhen(services, hits.size(), work, 100, [&](std::size_t i) {
        hits[i]++;
        pool_tasks++;
      });
      for (auto &hit : hits) {
        CHECK(hit == 1);
      }
      CHECK(pool_tasks == hits.size());
    }
  }
  // Below the threshold everything runs on the calling thread.
  FakeServices services(4);
  const auto self = std::this_thread::get_id();
  core::ParallelEachWhen(services, 8, 1, 100,
                         [&](std::size_t) { CHECK(std::this_thread::get_id() == self); });

  const std::vector<int> keys = {1, 1, 2, 3, 3, 3};
  const auto id = [](int k) { return k; };
  CHECK((core::RunStarts(keys, id) == std::vector<std::size_t>{0, 2, 3, 6}));
  CHECK((core::RunStarts(std::vector<int>{}, id) == std::vector<std::size_t>{0}));
  CHECK((core::RunStarts(std::vector<int>{5}, id) == std::vector<std::size_t>{0, 1}));

  for (const std::size_t workers : {1, 3, 5, 8, 16, 48}) {
    FakeServices sort_services(workers);
    std::vector<std::pair<int, int>> values;
    for (int i = 0; i < 5000; ++i) {
      values.emplace_back((i * 7919) % 101, i);
    }
    auto expected = values;
    std::stable_sort(expected.begin(), expected.end(),
                     [](const auto &a, const auto &b) { return a.first < b.first; });
    core::ParallelStableSort(sort_services, values,
                             [](const auto &a, const auto &b) { return a.first < b.first; }, 100);
    CHECK(values == expected);
  }

  // Adjacent pairwise merge keeps the order of an appending merge, at any part count.
  for (const std::size_t parts : {1, 2, 3, 5, 8, 13}) {
    FakeServices merge_services(4);
    std::vector<std::vector<int>> pieces(parts);
    std::vector<int> expected;
    for (std::size_t p = 0; p < parts; ++p) {
      pieces[p] = {static_cast<int>(p), static_cast<int>(p) + 100};
      expected.insert(expected.end(), pieces[p].begin(), pieces[p].end());
    }
    core::MergeAdjacent(merge_services, pieces, core::AppendRows<int>);
    CHECK(pieces[0] == expected);
  }
}

void TestBuildHelpers() {
  FakeServices one(1), many(4);
  CHECK(core::BuildPartitions(one) == 1 && core::BuildPartitions(many) == 16);
  const int row = 42;
  const core::SingleRowBatch<int> batch{row};
  CHECK(batch.size() == 1 && batch.Row(0) == 42);
  core::Buckets<int> into(2), from(2);
  into[0] = {1};
  from[0] = {2};
  from[1] = {3};
  core::AppendBuckets(into, std::move(from));
  CHECK((into[0] == std::vector<int>{1, 2}) && (into[1] == std::vector<int>{3}));
}

} // namespace

int main() {
  try {
    TestCells();
    TestMaintenance();
    TestParallel();
    TestBuildHelpers();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "test_core_helpers: FAIL %s\n", error.what());
    return 1;
  }
  std::puts("test_core_helpers: PASS");
  return 0;
}
