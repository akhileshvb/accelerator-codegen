// Randomized differential fuzz of the TPC-H aux accelerators on DuckDB,
// with writes captured by the engine (the DuckDB fork's MutationRecorder via
// the database-level capture), not built by the harness.
//
// Per query: a copy of the TPC-H database, the shared runtime built and
// registered, then T random write transactions on the runtime's connection:
// 1-4 statements each from tpch_fuzz_writes.hpp (valid TPC-H data: unique
// keys, resolving foreign keys), on random TPC-H tables. 15% roll back. After every
// transaction a separate connection compares the accelerator with the
// reference SQL. With R > 0, R reader threads meanwhile run both inside one
// transaction (snapshot consistency) for the whole run. A reader must never
// fall back to SQL (that would not read its snapshot): counted and failed.
//
// The write frequency: the writer waits write_interval_ms before each
// transaction (0: back to back); the runtime's expected reads per write is
// the number of reader queries since the previous write, as a workload's.
//
// Usage: test_tpch_aux_fuzz <tpch.duckdb> <queries|all> [txns=200] [seed=1]
//                           [readers=0] [workdir=/tmp] [write_interval_ms=0]
#include "eval/tests/test_support/db_paths.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "backends/duckdb/api/register.hpp"
#include "catalog/query_catalog.hpp"
#include "substrate/aux/core/aux_execute_stats.hpp"
#include "duckdb.hpp"
#include "duckdb/accelerator/mutation_recorder.hpp"
#include "eval/tests/test_support/cross_accel_helpers.hpp"
#include "eval/tests/tpch_fuzz_writes.hpp"

namespace {

using accelerator_codegen::aux_state::engine::duckdb::DuckdbRuntime;
namespace engine = accelerator_codegen::aux_state::engine;
namespace ts = accelerator_codegen::test_support;

void Exec(duckdb::Connection &c, const std::string &sql) {
  auto r = c.Query(sql);
  if (!r || r->HasError()) {
    throw std::runtime_error(sql + ": " + (r ? r->GetError() : std::string("null")));
  }
}

int64_t Scalar(duckdb::Connection &c, const std::string &sql) {
  auto r = c.Query(sql);
  if (!r || r->HasError() || r->RowCount() == 0) {
    throw std::runtime_error(sql + ": " + (r ? r->GetError() : std::string("null")));
  }
  auto v = r->GetValue(0, 0);
  return v.IsNull() ? 0 : v.GetValue<int64_t>();
}

std::string ComparisonError(duckdb::Connection &c, int q) {
  try {
    ts::RequireCrossAccelQueryCorrectness(c, q, "Q" + std::to_string(q));
    return {};
  } catch (const std::exception &e) {
    return e.what();
  }
}

// ACCEL_FUZZ_TRACE=1: log every mutation event the engine reports.
class TraceConsumer final : public duckdb::MutationConsumer {
public:
  void OnTableMutation(duckdb::ClientContext &, const duckdb::TableMutationEvent &event) override {
    std::cerr << "  [capture] " << event.table_name << " kind=" << int(event.kind) << " rows=" << event.row_count
              << " updated_cols=" << event.updated_columns.size() << "\n";
  }
};

struct Result {
  std::string execute_counts; // how the accelerator queries were answered
  long commits = 0, rollbacks = 0, statements = 0, checks = 0, reader_checks = 0;
  std::vector<std::string> failures;
};

Result FuzzQuery(const std::string &src_db, const std::string &workdir, int q, int txns, uint64_t seed,
                 int readers, int write_interval_ms) {
  const std::string db_path =
      workdir + "/tpch_aux_fuzz_q" + std::to_string(q) + "_" + std::to_string(::getpid()) + ".duckdb";
  std::filesystem::remove(db_path);
  std::filesystem::remove(db_path + ".wal");
  std::filesystem::copy_file(src_db, db_path);
  Result result;
  {
    duckdb::DuckDB db(db_path);
    duckdb::Connection writer_conn(db);
    accelerator_codegen::RegisterAcceleratorFunctions(writer_conn);
    const auto &entry = engine::LookupSharedQuery(engine::SharedWorkloadKind::Tpch, q);
    accelerator_codegen::aux_state::core::ResetAuxExecuteCounts();
    DuckdbRuntime runtime(writer_conn, entry, 10.0);
    runtime.Build();
    if (std::getenv("ACCEL_FUZZ_TRACE")) {
      writer_conn.context->registered_state->GetOrCreate<TraceConsumer>("accel_fuzz_trace");
    }

    duckdb::Connection checker(db);
    accelerator_codegen::RegisterAcceleratorFunctions(checker);
    if (auto err = ComparisonError(checker, q); !err.empty()) {
      result.failures.push_back("after build: " + err);
    }

    std::atomic<bool> stop {false};
    std::mutex failure_mutex;
    std::vector<std::thread> threads;
    std::atomic<long> reader_checks {0};
    for (int r = 0; r < readers; r++) {
      threads.emplace_back([&, r] {
        duckdb::Connection rc(db);
        accelerator_codegen::RegisterAcceleratorFunctions(rc);
        while (!stop.load()) {
          try {
            Exec(rc, "BEGIN TRANSACTION");
            auto err = ComparisonError(rc, q); // aux and base in one snapshot
            Exec(rc, "COMMIT");
            reader_checks++;
            if (!err.empty()) {
              std::lock_guard<std::mutex> l(failure_mutex);
              result.failures.push_back("reader " + std::to_string(r) + ": " + err);
            }
          } catch (const std::exception &e) {
            std::lock_guard<std::mutex> l(failure_mutex);
            result.failures.push_back("reader " + std::to_string(r) + " error: " + e.what());
            return;
          }
        }
      });
    }

    ts::TpchFuzzWrites writer(ts::FuzzDialect::DuckDB, seed * 1000 + static_cast<uint64_t>(q),
                              [&](const std::string &sql) { return Scalar(writer_conn, sql); });
    std::mt19937_64 rng(seed * 7919 + static_cast<uint64_t>(q));
    auto failure_count = [&] {
      std::lock_guard<std::mutex> l(failure_mutex);
      return result.failures.size();
    };
    long reads_at_last_write = 0;
    try {
    // ACCEL_FUZZ_KEEP_DB=1: stop at the first failure and keep the database.
    const bool keep_db = std::getenv("ACCEL_FUZZ_KEEP_DB") != nullptr;
    for (int t = 0; t < txns && failure_count() < (keep_db ? 1 : 5); t++) {
      if (write_interval_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(write_interval_ms));
      }
      const long reads = reader_checks.load();
      runtime.SetExpectedQueriesBeforeNextWrite(std::max<double>(1.0, reads - reads_at_last_write));
      reads_at_last_write = reads;
      runtime.BeginWrite();
      const int statements = 1 + static_cast<int>(rng() % 4);
      std::string last;
      try {
        for (int i = 0; i < statements; i++) {
          const auto next = writer.Next();
          last += (last.empty() ? "" : " ;; ") + next;
          // A generator entry may be several statements separated by ";\n".
          std::vector<std::string> parts;
          for (std::size_t pos = 0;;) {
            const auto end = next.find(";\n", pos);
            parts.push_back(next.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
            if (end == std::string::npos) {
              break;
            }
            pos = end + 2;
          }
          for (const auto &one : parts) {
            if (one.find_first_not_of(" \n") != std::string::npos) {
              if (std::getenv("ACCEL_FUZZ_TRACE")) {
                std::cerr << "txn " << t << ": " << one.substr(0, 160) << "\n";
              }
              Exec(writer_conn, one);
              result.statements++;
            }
          }
        }
      } catch (const std::exception &e) {
        runtime.RollbackWrite();
        std::lock_guard<std::mutex> l(failure_mutex);
        result.failures.push_back("txn " + std::to_string(t) + " statement failed: " + e.what() + " (statements: " + last + ")");
        break;
      }
      if (rng() % 100 < 15) {
        runtime.RollbackWrite();
        result.rollbacks++;
      } else {
        runtime.CommitWrite();
        result.commits++;
      }
      result.checks++;
      if (auto err = ComparisonError(checker, q); !err.empty()) {
        std::lock_guard<std::mutex> l(failure_mutex);
        result.failures.push_back("after txn " + std::to_string(t) + " (statements: " + last + "): " + err);
      }
    }
    } catch (const std::exception &e) { // e.g. CommitWrite: still join the readers
      std::lock_guard<std::mutex> l(failure_mutex);
      result.failures.push_back(std::string("writer error: ") + e.what());
    }
    stop = true;
    for (auto &th : threads) {
      th.join();
    }
    result.reader_checks = reader_checks.load();
    result.execute_counts = accelerator_codegen::aux_state::core::FormatAuxExecuteCounts(
        accelerator_codegen::aux_state::core::ReadAuxExecuteCounts());
    if (const auto fallbacks = runtime.reader_fallbacks.load(); fallbacks > 0) {
      result.failures.push_back(std::to_string(fallbacks) +
                                " reader queries fell back to SQL (state was invalid for them)");
    }
  }
  if (std::getenv("ACCEL_FUZZ_KEEP_DB") && !result.failures.empty()) {
    std::cout << "  kept " << db_path << std::endl;
    return result;
  }
  std::filesystem::remove(db_path);
  std::filesystem::remove(db_path + ".wal");
  return result;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    std::cerr << "usage: test_tpch_aux_fuzz <tpch.duckdb> <queries|all> [txns] [seed] [readers] [workdir] [write_interval_ms]\n";
    return 2;
  }
  const std::string src = argv[1];
  std::vector<int> queries;
  if (std::string(argv[2]) == "all") {
    for (int q = 1; q <= 22; q++) {
      queries.push_back(q);
    }
  } else {
    std::stringstream ss(argv[2]);
    std::string item;
    while (std::getline(ss, item, ',')) {
      queries.push_back(std::stoi(item));
    }
  }
  const int txns = argc > 3 ? std::atoi(argv[3]) : 200;
  const uint64_t seed = argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 1;
  const int readers = argc > 5 ? std::atoi(argv[5]) : 0;
  accelerator_codegen::test_support::ScratchDir scratch("tpch_aux_fuzz");
  const std::string workdir = argc > 6 ? std::string(argv[6]) : scratch.path().string();
  const int write_interval_ms = argc > 7 ? std::atoi(argv[7]) : 0;
  int failed = 0;
  for (int q : queries) {
    const auto start = std::chrono::steady_clock::now();
    Result r;
    try {
      r = FuzzQuery(src, workdir, q, txns, seed, readers, write_interval_ms);
    } catch (const std::exception &e) {
      r.failures.push_back(std::string("error: ") + e.what());
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << (r.failures.empty() ? "PASS" : "FAIL") << " Q" << q << ": " << r.commits << " commits, "
              << r.rollbacks << " rollbacks, " << r.statements << " statements, " << r.checks << " checks, "
              << r.reader_checks << " reader checks, " << secs << " s [" << r.execute_counts << "]"
              << std::endl;
    for (std::size_t i = 0; i < r.failures.size() && i < 3; i++) {
      std::cout << "  " << r.failures[i] << std::endl;
    }
    failed += r.failures.empty() ? 0 : 1;
  }
  std::cout << (failed == 0 ? "ALL PASS" : std::to_string(failed) + " queries FAILED") << std::endl;
  return failed == 0 ? 0 : 1;
}
