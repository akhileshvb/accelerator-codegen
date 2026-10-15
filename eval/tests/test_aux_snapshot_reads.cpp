// Snapshot consistency of the C++ aux accelerators under concurrent commits,
// through the real DuckDB machinery: the shared runtime built and registered,
// writes captured by the engine (the database-level capture) and
// committed through the runtime (BeginWrite / CommitWrite), readers in their
// own connections calling the table function.
//
// Each reader, inside ONE read transaction: the accelerator, a pause, the
// reference SQL, a pause, the accelerator again. Both accelerator answers
// must equal the reference: every read of a transaction sees its snapshot,
// whatever commits in between. Mismatches are counted per call (first /
// second), so a read that drifts to a newer commit shows as a second-call
// mismatch.
//
// The writes: partsupp / supplier / nation writes (tpch_fuzz_writes.hpp) plus
// "hot" writes on the suppliers of GERMANY and FRANCE (whole-supplier repricing
// and restocking, supplier moves between the two, partsupp deletes), so the
// answers the readers ask for change on most commits.
//
// mode=shared (default): the setting above. mode=private: no runtime is
// registered and the writes are plain SQL commits, so every table-function
// call builds a private runtime for that one query (the "no shared runtime"
// path of duckdb_aux_table_functions.cpp); that path must answer at the
// reader's snapshot too.
//
// Usage: test_aux_snapshot_reads <tpch.duckdb> <queries> [seconds=3]
//                                [readers=4] [mode=shared|private] [seed=1]
// The database is copied into a scratch directory ($TMPDIR) per query.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "backends/duckdb/api/register.hpp"
#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"
#include "eval/tests/test_support/db_paths.hpp"
#include "eval/tests/tpch_fuzz_writes.hpp"

namespace {

using accelerator_codegen::aux_state::engine::duckdb::DuckdbRuntime;
namespace engine = accelerator_codegen::aux_state::engine;
namespace ts = accelerator_codegen::test_support;
using Rows = std::vector<std::string>; // one canonical string per row, sorted

std::unique_ptr<duckdb::MaterializedQueryResult> Query(duckdb::Connection &c, const std::string &sql) {
  auto r = c.Query(sql);
  if (!r || r->HasError()) {
    throw std::runtime_error(sql.substr(0, 300) + ": " + (r ? r->GetError() : std::string("null")));
  }
  return r;
}

int64_t Scalar(duckdb::Connection &c, const std::string &sql) {
  auto r = Query(c, sql);
  if (r->RowCount() == 0) {
    return 0;
  }
  auto v = r->GetValue(0, 0);
  return v.IsNull() ? 0 : v.GetValue<int64_t>();
}

// Order-insensitive canonical rows: numbers at two decimals (the aux kernels'
// fixed point and the reference's DECIMAL agree there), the rest as text.
Rows Collect(duckdb::Connection &c, const std::string &sql) {
  auto r = Query(c, sql);
  Rows rows;
  rows.reserve(r->RowCount());
  for (duckdb::idx_t i = 0; i < r->RowCount(); ++i) {
    std::string row;
    for (duckdb::idx_t j = 0; j < r->ColumnCount(); ++j) {
      const auto v = r->GetValue(j, i);
      if (j) {
        row += '|';
      }
      if (v.IsNull()) {
        row += "NULL";
      } else if (v.type().IsIntegral()) {
        row += std::to_string(v.GetValue<int64_t>());
      } else if (v.type().IsNumeric()) {
        row += std::to_string(std::llround(v.GetValue<double>() * 100.0));
      } else {
        row += v.ToString();
      }
    }
    rows.push_back(std::move(row));
  }
  std::sort(rows.begin(), rows.end());
  return rows;
}

// The first difference, for the report.
std::string Diff(const Rows &accel, const Rows &ref) {
  std::ostringstream out;
  out << "accel_rows=" << accel.size() << " ref_rows=" << ref.size();
  std::vector<std::string> only_a, only_r;
  std::set_difference(accel.begin(), accel.end(), ref.begin(), ref.end(), std::back_inserter(only_a));
  std::set_difference(ref.begin(), ref.end(), accel.begin(), accel.end(), std::back_inserter(only_r));
  out << " only_accel=" << only_a.size() << (only_a.empty() ? "" : " e.g. " + only_a.front())
      << " only_ref=" << only_r.size() << (only_r.empty() ? "" : " e.g. " + only_r.front());
  return out.str();
}

struct Case {
  std::string label, accel_sql, ref_sql;
};

std::string Q11Sql(const std::string &nation, const std::string &fraction) {
  const std::string from = "FROM partsupp, supplier, nation WHERE ps_suppkey = s_suppkey AND s_nationkey = n_nationkey "
                           "AND n_name = '" + nation + "'";
  return "SELECT ps_partkey, SUM(ps_supplycost * ps_availqty) AS value " + from +
         " GROUP BY ps_partkey HAVING SUM(ps_supplycost * ps_availqty) > (SELECT SUM(ps_supplycost * ps_availqty) * " +
         fraction + " " + from + ") ORDER BY value DESC";
}

std::vector<Case> CasesFor(const engine::SharedQueryCatalogEntry &entry) {
  if (entry.query_id == 11) {
    std::vector<Case> cases;
    for (const auto &[nation, fraction] : std::vector<std::pair<std::string, std::string>>{
             {"GERMANY", "0.0001"}, {"FRANCE", "0.001"}, {"GERMANY", "0.00001"}, {"FRANCE", "0.0001"},
             {"ROMANIA", "0.0001"}, {"JAPAN", "0.000005"}}) {
      cases.push_back({nation + "," + fraction,
                       "SELECT * FROM accel_tpch_q11_aux('" + nation + "', " + fraction + ")", Q11Sql(nation, fraction)});
    }
    return cases;
  }
  return {{"benchmark args", entry.duckdb_aux_sql, entry.base_sql}};
}

// One write transaction's statements (a generator entry may hold several, ";\n"-separated).
std::vector<std::string> NextWrite(ts::TpchFuzzWrites &fuzz, std::mt19937_64 &rng) {
  const auto salt = std::to_string(rng() % 1000000);
  const std::string supplier =
      "(SELECT s_suppkey FROM supplier WHERE s_nationkey IN (6, 7) ORDER BY hash(s_suppkey, " + salt + ") LIMIT 1)";
  std::string sql;
  switch (rng() % 8) {
  case 0:
    sql = "UPDATE partsupp SET ps_supplycost = ps_supplycost * " + std::to_string(50 + rng() % 101) +
          " / 100.0 WHERE ps_suppkey IN " + supplier;
    break;
  case 1:
    sql = "UPDATE partsupp SET ps_availqty = greatest(1, ps_availqty + " + std::to_string(int(rng() % 2001) - 1000) +
          ") WHERE ps_suppkey IN " + supplier;
    break;
  case 2:
    sql = "UPDATE supplier SET s_nationkey = CASE WHEN s_nationkey = 6 THEN 7 ELSE 6 END WHERE s_suppkey IN " +
          supplier;
    break;
  case 3:
    sql = "DELETE FROM partsupp WHERE rowid IN (SELECT rowid FROM partsupp WHERE ps_suppkey IN " + supplier +
          " ORDER BY hash(rowid, " + salt + ") LIMIT 2)";
    break;
  default: // partsupp / supplier / nation (and part) writes of the fuzz generator
    sql = fuzz.Next({0, 0, 0, 10, 45, 35, 2, 0});
  }
  std::vector<std::string> out;
  for (std::size_t pos = 0;;) {
    const auto end = sql.find(";\n", pos);
    auto one = sql.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    if (one.find_first_not_of(" \n") != std::string::npos) {
      out.push_back(std::move(one));
    }
    if (end == std::string::npos) {
      break;
    }
    pos = end + 2;
  }
  return out;
}

struct Outcome {
  long reads = 0, commits = 0, write_errors = 0, first_mismatches = 0, second_mismatches = 0, final_mismatches = 0;
  long reader_errors = 0, reader_fallbacks = 0;
  std::vector<std::string> examples;
};

Outcome RunQuery(const std::string &src, int q, int seconds, int readers, bool shared, uint64_t seed) {
  const auto &entry = engine::LookupSharedQuery(engine::SharedWorkloadKind::Tpch, q);
  const auto cases = CasesFor(entry);
  ts::ScratchDir scratch("aux_snapshot_reads_q" + std::to_string(q));
  const auto path = ts::PrivateDbCopy(src, scratch);
  Outcome out;
  std::mutex mutex;
  auto example = [&](const std::string &text) {
    std::lock_guard<std::mutex> lock(mutex);
    if (out.examples.size() < 6) {
      out.examples.push_back(text);
    }
  };
  duckdb::DuckDB db(path);
  duckdb::Connection wc(db);
  accelerator_codegen::RegisterAcceleratorFunctions(wc);
  std::unique_ptr<DuckdbRuntime> runtime;
  if (shared) {
    runtime = std::make_unique<DuckdbRuntime>(wc, entry, 10.0);
    runtime->Build();
  }
  std::atomic<bool> stop{false};
  std::atomic<long> reads{0}, first{0}, second{0}, reader_errors{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < readers; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937_64 rng(seed * 1000003 + static_cast<uint64_t>(t));
      duckdb::Connection rc(db);
      accelerator_codegen::RegisterAcceleratorFunctions(rc);
      while (!stop.load()) {
        const auto &c = cases[rng() % cases.size()];
        try {
          Query(rc, "BEGIN TRANSACTION");
          const auto a = Collect(rc, c.accel_sql);
          std::this_thread::sleep_for(std::chrono::microseconds(rng() % 3000));
          const auto ref = Collect(rc, c.ref_sql);
          std::this_thread::sleep_for(std::chrono::microseconds(rng() % 3000));
          const auto b = Collect(rc, c.accel_sql);
          Query(rc, "COMMIT");
          ++reads;
          if (a != ref) {
            ++first;
            example("Q" + std::to_string(q) + " [" + c.label + "] first call: " + Diff(a, ref));
          }
          if (b != ref) {
            ++second;
            example("Q" + std::to_string(q) + " [" + c.label + "] second call: " + Diff(b, ref));
          }
        } catch (const std::exception &e) {
          ++reader_errors;
          example("Q" + std::to_string(q) + " reader error: " + std::string(e.what()).substr(0, 300));
          try {
            rc.Query("ROLLBACK");
          } catch (...) {
          }
        }
      }
    });
  }
  ts::TpchFuzzWrites fuzz(ts::FuzzDialect::DuckDB, seed * 1000 + static_cast<uint64_t>(q),
                          [&](const std::string &sql) { return Scalar(wc, sql); });
  std::mt19937_64 rng(seed * 7919 + static_cast<uint64_t>(q));
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  try {
    while (std::chrono::steady_clock::now() < end) {
      const int statements = 1 + static_cast<int>(rng() % 3);
      std::vector<std::string> txn;
      for (int i = 0; i < statements; ++i) {
        for (auto &s : NextWrite(fuzz, rng)) {
          txn.push_back(std::move(s));
        }
      }
      if (shared) {
        runtime->BeginWrite();
      } else {
        Query(wc, "BEGIN TRANSACTION");
      }
      bool ok = true;
      for (const auto &s : txn) {
        auto r = wc.Query(s);
        if (!r || r->HasError()) {
          ok = false;
          ++out.write_errors;
          example("Q" + std::to_string(q) + " write failed: " + s.substr(0, 200) + ": " +
                  (r ? r->GetError() : std::string("null")).substr(0, 200));
          break;
        }
      }
      if (shared) {
        ok ? runtime->CommitWrite() : runtime->RollbackWrite();
      } else {
        Query(wc, ok ? "COMMIT" : "ROLLBACK");
      }
      out.commits += ok ? 1 : 0;
      std::this_thread::sleep_for(std::chrono::microseconds(rng() % 2000));
    }
  } catch (const std::exception &e) {
    example(std::string("writer error: ") + e.what());
    ++out.write_errors;
  }
  stop = true;
  for (auto &th : threads) {
    th.join();
  }
  // Quiescent: every case once more (outside any concurrency).
  duckdb::Connection checker(db);
  accelerator_codegen::RegisterAcceleratorFunctions(checker);
  for (const auto &c : cases) {
    const auto a = Collect(checker, c.accel_sql);
    const auto ref = Collect(checker, c.ref_sql);
    if (a != ref) {
      ++out.final_mismatches;
      example("Q" + std::to_string(q) + " [" + c.label + "] after the run: " + Diff(a, ref));
    }
  }
  out.reads = reads;
  out.first_mismatches = first;
  out.second_mismatches = second;
  out.reader_errors = reader_errors;
  out.reader_fallbacks = runtime ? static_cast<long>(runtime->reader_fallbacks.load()) : 0;
  return out;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    std::cerr << "usage: test_aux_snapshot_reads <tpch.duckdb> <queries> [seconds=3] [readers=4] "
                 "[mode=shared|private] [seed=1]\n";
    return 2;
  }
  try {
    std::vector<int> queries;
    std::stringstream ss(argv[2]);
    for (std::string item; std::getline(ss, item, ',');) {
      queries.push_back(std::stoi(item));
    }
    const int seconds = argc > 3 ? std::atoi(argv[3]) : 3;
    const int readers = argc > 4 ? std::atoi(argv[4]) : 4;
    const std::string mode = argc > 5 ? argv[5] : "shared";
    if (mode != "shared" && mode != "private") {
      throw std::runtime_error("mode must be shared or private");
    }
    const uint64_t seed = argc > 6 ? std::strtoull(argv[6], nullptr, 10) : 1;
    bool failed = false;
    for (int q : queries) {
      const auto o = RunQuery(argv[1], q, seconds, readers, mode == "shared", seed);
      // A reader older than a background rebuild answers from SQL at its own
      // snapshot (reader_fallbacks, reported): exact, so not a failure.
      const bool bad = o.first_mismatches || o.second_mismatches || o.final_mismatches || o.reader_errors ||
                       o.reads == 0 || o.commits == 0;
      failed |= bad;
      std::cout << (bad ? "FAIL" : "PASS") << " Q" << q << " mode=" << mode << " reads=" << o.reads
                << " commits=" << o.commits << " write_errors=" << o.write_errors
                << " first_call_mismatches=" << o.first_mismatches
                << " second_call_mismatches=" << o.second_mismatches << " final_mismatches=" << o.final_mismatches
                << " reader_errors=" << o.reader_errors << " reader_fallbacks=" << o.reader_fallbacks << "\n";
      for (const auto &e : o.examples) {
        std::cout << "  " << e << "\n";
      }
    }
    return failed ? 1 : 0;
  } catch (const std::exception &e) {
    std::cerr << "test_aux_snapshot_reads failed: " << e.what() << "\n";
    return 1;
  }
}
