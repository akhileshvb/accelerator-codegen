// GENERATED BACKEND - test MODULE for the partitioned build scan (S8)
//
// Built only into accelerator_codegen_pg_scan_test (LOAD 'accelerator_codegen_pg'
// first: the workers and ScanTablePartitioned come from the production module).
// scan_test_partitioned(participants, work) builds the toy module's state
// (eval/tests/test_support) from `lineitem` with core::PartitionedBuild at
// several partition counts inside one snapshot, and raises an error unless
// every state equals the serial scan's and SQL's answer. `work` adds that many
// hash rounds per row (an expensive state insert). Returns one timing line per
// partition count.
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

extern "C" {
#include <postgres.h>
#include <fmgr.h>
#include <utils/builtins.h>
PG_MODULE_MAGIC;
PG_FUNCTION_INFO_V1(scan_test_partitioned);
}

#include <chrono>
#include <sstream>
#include <stdexcept>

#include "backends/postgres/api/engine_helpers.hpp"
#include "eval/tests/test_support/toy_build_state.hpp"
#include "substrate/aux/core/partitioned_build.hpp"
#include "substrate/aux/runtime/thread_budget.hpp"

namespace core = accelerator_codegen::aux_state::core;
namespace runtime = accelerator_codegen::aux_state::runtime;
namespace test = accelerator_codegen::aux_state::test;
namespace pg = accelerator_codegen::aux_state::engine::postgres;

namespace {

class BuildServices final : public core::AuxEngineServices {
public:
  explicit BuildServices(std::size_t workers_p) : workers(workers_p), pool(workers_p) {}
  core::VisibilityToken CurrentVisibility() const override { throw std::logic_error("unused"); }
  core::AuxStateStore &StateStore() override { throw std::logic_error("unused"); }
  std::size_t WorkerCount() const override { return workers; }
  void ParallelFor(std::size_t n, const std::function<void(std::size_t)> &fn) const override {
    pool.ParallelFor(n, fn, workers);
  }

private:
  std::size_t workers;
  mutable runtime::ThreadBudget pool;
};

test::ToyState BuildToy(std::size_t partitions, int work) {
  BuildServices services(8);
  return core::PartitionedBuild<test::ToyState>(
      services, [] { return test::ToyState{}; },
      [&](std::size_t parts, auto &&sink) {
        pg::detail::ScanPartitioned(services, "lineitem",
                                    {pg::detail::IntColumn("l_orderkey"), pg::detail::DateColumn("l_shipdate"),
                                     pg::detail::DecimalColumn("l_quantity", 2),
                                     pg::detail::TextColumn("l_returnflag"), pg::detail::TextColumn("l_comment")},
                                    parts, sink);
      },
      [work](test::ToyState &state, const pg::detail::BuildRow &row) { state.Add(row, work); },
      [](test::ToyState &into, test::ToyState &&from) { into.Merge(std::move(from)); }, partitions);
}

test::ToyState SqlAnswer() {
  test::ToyState truth;
  pg::detail::ScanTypedTuplesParallel(
      "SELECT count(*), sum(l_orderkey), (sum(l_quantity) * 100)::bigint, "
      "max(l_shipdate) - DATE '1970-01-01' FROM lineitem",
      [&](HeapTuple tuple, TupleDesc desc) {
        truth.rows = pg::detail::TypedInt(tuple, desc, 0);
        truth.key_sum = pg::detail::TypedInt(tuple, desc, 1);
        truth.qty_raw = pg::detail::TypedInt(tuple, desc, 2);
        truth.date_max = pg::detail::TypedInt(tuple, desc, 3);
      });
  return truth;
}

std::string RunTest(int participants, int work) {
  pg::SetBuildScanParticipants(participants);
  const test::ToyState truth = SqlAnswer();
  std::ostringstream report;
  const test::ToyState serial = BuildToy(1, work);
  if (serial.rows != truth.rows || serial.key_sum != truth.key_sum || serial.qty_raw != truth.qty_raw ||
      serial.date_max != truth.date_max || serial.exact_sum != static_cast<double>(truth.qty_raw) * 0.25)
    throw std::runtime_error("the serial scan disagrees with SQL");
  for (const std::size_t partitions : {1, 2, 4, 8, 16, 32, 64}) {
    const auto start = std::chrono::steady_clock::now();
    const test::ToyState state = BuildToy(partitions, work);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (!(state == serial))
      throw std::runtime_error("partitions=" + std::to_string(partitions) + " differ from the serial scan");
    report << "P=" << partitions << " rows=" << state.rows << " " << seconds << " s\n";
  }
  return report.str();
}

} // namespace

extern "C" {
Datum scan_test_partitioned(PG_FUNCTION_ARGS) {
  std::string report;
  try {
    report = RunTest(PG_GETARG_INT32(0), PG_GETARG_INT32(1));
  } catch (const std::exception &error) {
    ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("scan_test_partitioned: %s", error.what())));
  }
  PG_RETURN_TEXT_P(cstring_to_text(report.c_str()));
}
}

#endif
