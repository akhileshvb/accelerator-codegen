// Micro-benchmark for the SEC-EDGAR Q3 aux accelerator on a real database:
// build, read (module + SQL table function), raw maintenance (ApplyDelta
// through the coordinator, no SQL), and write+read alternation.
//
// usage: bench_sec_edgar_q3_aux <db_path> [reads] [writes] [sql_table_function_reads]
// The database is only read; maintenance is applied to the aux state alone.
#include <malloc.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "accelerator_codegen/runtime/duckdb/duckdb_runtime.hpp"
#include "sec_edgar/q3/duckdb/aux/module.hpp"
#include "accelerator_codegen/runtime/duckdb/register.hpp"
#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"

namespace {
namespace catalog = accelerator_codegen::aux_state::engine;
namespace core = accelerator_codegen::aux_state::core;
using Clock = std::chrono::steady_clock;

double Us(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::micro>(b - a).count();
}

struct Stats {
  double p50 = 0, mean = 0, p99 = 0;
};

Stats Summarize(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  Stats s;
  if (v.empty()) {
    return s;
  }
  s.p50 = v[v.size() / 2];
  s.p99 = v[std::min(v.size() - 1, v.size() * 99 / 100)];
  for (double x : v) {
    s.mean += x;
  }
  s.mean /= static_cast<double>(v.size());
  return s;
}

void Report(const char *name, const Stats &s) {
  std::cout << std::left << std::setw(34) << name << std::right << std::fixed
            << std::setprecision(2) << " p50=" << std::setw(10) << s.p50
            << "us mean=" << std::setw(10) << s.mean << "us p99=" << std::setw(10)
            << s.p99 << "us\n";
}

core::AuxValue Str(const std::string &v) {
  return core::AuxValue::Of(core::AuxType::String, v);
}
core::AuxValue Int(int64_t v) { return core::AuxValue::Of(core::AuxType::Int64, v); }
} // namespace

int RunKernels() {
  using namespace accelerator_codegen::aux_state::query;
  std::mt19937 rng(3);
  const uint32_t n = 8251;
  SecEdgarQ3AggArray array;
  array.size = n;
  for (uint32_t c = 0; c < (n + kSecEdgarQ3AggChunk - 1) / kSecEdgarQ3AggChunk; ++c) {
    auto chunk = std::make_shared<SecEdgarQ3AggChunkData>();
    std::memset(chunk.get(), 0, sizeof(*chunk));
    for (uint32_t i = 0; i < kSecEdgarQ3AggChunk && c * kSecEdgarQ3AggChunk + i < n; ++i) {
      chunk->cnt[i] = 1 + rng() % 50;
      chunk->sum[i] = std::exp(std::uniform_real_distribution<double>(0, 14)(rng));
    }
    array.chunks.push_back(chunk);
  }
  double sum;
  uint64_t cnt;
  SecEdgarQ3Kernels::AvgInputsScalar(array, sum, cnt);
  const double thr = sum / static_cast<double>(cnt);
  auto time = [&](const char *name, auto &&fn) {
    std::vector<double> t;
    for (int i = 0; i < 2000; ++i) {
      const auto a = Clock::now();
      fn();
      t.push_back(Us(a, Clock::now()));
    }
    Report(name, Summarize(t));
  };
  volatile double sink = 0;
  time("avg inputs scalar", [&] { double s; uint64_t c; SecEdgarQ3Kernels::AvgInputsScalar(array, s, c); sink = s; });
  time("avg inputs avx2", [&] { double s; uint64_t c; SecEdgarQ3Kernels::AvgInputsAvx2(array, s, c); sink = s; });
  std::vector<SecEdgarQ3Result::Row> out;
  time("collect scalar", [&] { out.clear(); SecEdgarQ3Kernels::CollectScalar(array, thr, out); });
  const auto cand = out.size();
  time("collect avx2", [&] { out.clear(); SecEdgarQ3Kernels::CollectAvx2(array, thr, out); });
  std::cout << "candidates=" << cand << "\n";
  return 0;
}

int main(int argc, char **argv) {
  if (argc > 1 && std::string(argv[1]) == "--kernels") {
    return RunKernels();
  }
  if (argc < 2) {
    std::cerr << "usage: bench_sec_edgar_q3_aux <db> [reads] [writes]\n";
    return 2;
  }
  const int reads = argc > 2 ? std::atoi(argv[2]) : 2000;
  const int writes = argc > 3 ? std::atoi(argv[3]) : 2000;
  const int tf_reads = argc > 4 ? std::atoi(argv[4]) : std::min(reads, 500);
  duckdb::DuckDB database(argv[1]);
  duckdb::Connection connection(database);
  accelerator_codegen::RegisterAcceleratorFunctions(connection);
  connection.Query("PRAGMA threads=1");
  const auto &entry = catalog::LookupSharedQuery("sec_edgar", 3);
  catalog::duckdb::DuckdbRuntime runtime(connection, entry, 10.0);

  malloc_trim(0);
  const size_t heap_before = mallinfo2().uordblks;
  const auto t0 = Clock::now();
  runtime.Build();
  malloc_trim(0);
  std::cout << "heap_bytes_after_build=" << (mallinfo2().uordblks - heap_before)
            << "\n";
  std::cout << "build_us=" << Us(t0, Clock::now())
            << " footprint_bytes=" << runtime.AuxStateFootprintBytes() << "\n";

  // sample real fy=2022 USD rows (adsh, value) to replay as updates
  auto sample = connection.Query(
      "SELECT n.adsh, n.tag, n.version, n.ddate, n.qtrs, n.uom, n.coreg, n.value "
      "FROM num n JOIN sub s ON n.adsh = s.adsh "
      "WHERE s.fy = 2022 AND n.uom = 'USD' AND n.value IS NOT NULL "
      "USING SAMPLE 20000 ROWS");
  if (!sample || sample->HasError()) {
    std::cerr << "sample failed\n";
    return 1;
  }
  std::vector<core::AuxRow> rows;
  for (duckdb::idx_t r = 0; r < sample->RowCount(); ++r) {
    core::AuxRow row;
    row.values = {Str(sample->GetValue(0, r).ToString()),
                  Str(sample->GetValue(1, r).ToString()),
                  Str(sample->GetValue(2, r).ToString()),
                  Int(sample->GetValue(3, r).GetValue<int64_t>()),
                  Int(sample->GetValue(4, r).GetValue<int64_t>()),
                  Str(sample->GetValue(5, r).ToString()),
                  Str(sample->GetValue(6, r).ToString()),
                  core::AuxValue::Of(core::AuxType::Double,
                                     sample->GetValue(7, r).GetValue<double>())};
    rows.push_back(std::move(row));
  }
  std::mt19937 rng(1);

  std::vector<double> t;
  for (int i = 0; i < reads; ++i) {
    const auto a = Clock::now();
    auto r = runtime.ExecuteCollect();
    t.push_back(Us(a, Clock::now()));
    if (r.size() != 100) {
      std::cerr << "unexpected row count " << r.size() << "\n";
    }
  }
  Report("read: module Execute (steady)", Summarize(t));

  t.clear();
  {
    const auto a = Clock::now();
    auto r = connection.Query(entry.duckdb_aux_sql);
    std::cout << "first SQL table function call: " << Us(a, Clock::now())
              << "us\n";
  }
  for (int i = 0; i < tf_reads; ++i) {
    const auto a = Clock::now();
    auto r = connection.Query(entry.duckdb_aux_sql);
    for (duckdb::idx_t row = 0; row < r->RowCount(); ++row) {
      for (duckdb::idx_t c = 0; c < r->ColumnCount(); ++c) {
        (void)r->GetValue(c, row);
      }
    }
    t.push_back(Us(a, Clock::now()));
  }
  Report("read: SQL table function (steady)", Summarize(t));

  // maintenance only: single-row value update through the coordinator
  t.clear();
  for (int i = 0; i < writes; ++i) {
    const auto &old_row = rows[rng() % rows.size()];
    core::MutationBatch batch;
    batch.table = 1;
    batch.kind = core::MutationKind::Update;
    batch.updated_columns = {7};
    batch.old_rows = {old_row};
    batch.new_rows = {old_row};
    batch.new_rows[0].values[7] = core::AuxValue::Of(
        core::AuxType::Double,
        std::get<double>(old_row.values[7].payload) + 1.0 + (rng() % 1000));
    const auto a = Clock::now();
    runtime.ApplyMutationBatch(batch);
    t.push_back(Us(a, Clock::now()));
  }
  Report("write: ApplyDelta 1-row update", Summarize(t));

  // write + read alternation (every read follows a write)
  t.clear();
  std::vector<double> tw;
  for (int i = 0; i < writes; ++i) {
    const auto &old_row = rows[rng() % rows.size()];
    core::MutationBatch batch;
    batch.table = 1;
    batch.kind = core::MutationKind::Update;
    batch.updated_columns = {7};
    batch.old_rows = {old_row};
    batch.new_rows = {old_row};
    batch.new_rows[0].values[7] = core::AuxValue::Of(
        core::AuxType::Double,
        std::get<double>(old_row.values[7].payload) + 1.0 + (rng() % 1000));
    const auto a = Clock::now();
    runtime.ApplyMutationBatch(batch);
    const auto b = Clock::now();
    auto r = runtime.ExecuteCollect();
    tw.push_back(Us(a, b));
    t.push_back(Us(b, Clock::now()));
    (void)r;
  }
  Report("write (before read)", Summarize(tw));
  Report("read right after write", Summarize(t));

  // batch of 1000 inserts
  t.clear();
  for (int rep = 0; rep < 50; ++rep) {
    core::MutationBatch batch;
    batch.table = 1;
    batch.kind = core::MutationKind::Insert;
    for (int i = 0; i < 1000; ++i) {
      batch.new_rows.push_back(rows[rng() % rows.size()]);
    }
    const auto a = Clock::now();
    runtime.ApplyMutationBatch(batch);
    t.push_back(Us(a, Clock::now()));
  }
  Report("write: ApplyDelta 1000-row insert", Summarize(t));
  return 0;
}
