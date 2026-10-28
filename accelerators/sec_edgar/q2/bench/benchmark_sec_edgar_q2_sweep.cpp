// SEC-EDGAR Q2 aux sweep: read-only latency and write-rate sweep, aux vs plain
// DuckDB, in one process (the aux state is built once).
//
//   benchmark_sec_edgar_q2_sweep <db_copy> [aux|aux_parallel] [reads] [csv_out]
//
// The database file is modified (probe value bumps / inserts / deletes) and the
// probe is restored at the end; always pass a work copy.
//
// The sweep runs for every parameter set (fy, uom) that eval/harness/param_sets
// draws (ACCEL_PARAM_SEED=<s> reproduces a run, ACCEL_PARAM_SETS=<k> sets their
// number); a set whose answer is empty is skipped. Base and aux run the same sets.
//
// Only the public DuckdbRuntime / catalog API is used, so the same binary can
// be run against an older libaccelerator_codegen.so (LD_LIBRARY_PATH) to
// compare versions.
//
// Writes go through DuckdbRuntime::ApplyMutationBatch with a *faithful*
// mutation stream (every batch carries the row's true old and new value);
// DuckdbRuntime::Apply re-sends the first old/new pair on every call, which a
// max-style incremental module cannot consume more than once.
//
// Env: ACCEL_BENCH_THREADS (default 1), Q2_BENCH_WRITES_PER_READ="0,1,10,100,1000"
//      Q2_BENCH_CYCLES (reads per write-rate point, default 20)
//      Q2_BENCH_PATTERN=bump|mixed|both (default both)
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "eval/harness/differential.hpp"
#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "backends/duckdb/api/register.hpp"
#include "backends/duckdb/api/sql_fallback.hpp"
#include "eval/harness/param_sets.hpp"
#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"

namespace {

namespace catalog = accelerator_codegen::aux_state::engine;
namespace core = accelerator_codegen::aux_state::core;
namespace harness = accelerator_codegen::aux_state::harness;
using catalog::duckdb::DuckdbRuntime;
using Clock = std::chrono::steady_clock;

double ElapsedUs(const Clock::time_point &start) {
  return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}

void Require(bool ok, const std::string &msg) {
  if (!ok) {
    throw std::runtime_error(msg);
  }
}

std::string EnvStr(const char *name, const std::string &fallback) {
  const char *v = std::getenv(name);
  return v && *v ? v : fallback;
}

void Exec(duckdb::Connection &con, const std::string &sql) {
  auto r = con.Query(sql);
  Require(r && !r->HasError(), "SQL failed: " + sql + " " + (r ? r->GetError() : ""));
}

uint64_t Drain(duckdb::Connection &con, const std::string &sql) {
  auto r = con.Query(sql);
  Require(r && !r->HasError(), "SQL failed: " + sql + " " + (r ? r->GetError() : ""));
  for (duckdb::idx_t i = 0; i < r->RowCount(); ++i) {
    for (duckdb::idx_t c = 0; c < r->ColumnCount(); ++c) {
      (void)r->GetValue(c, i);
    }
  }
  return r->RowCount();
}

struct Stats {
  double mean = 0, p50 = 0, p99 = 0;
  size_t n = 0;
};

Stats Summarize(std::vector<double> v) {
  Stats s;
  if (v.empty()) {
    return s;
  }
  std::sort(v.begin(), v.end());
  s.n = v.size();
  double sum = 0;
  for (double x : v) {
    sum += x;
  }
  s.mean = sum / static_cast<double>(v.size());
  s.p50 = v[v.size() / 2];
  s.p99 = v[std::min(v.size() - 1, static_cast<size_t>(0.99 * v.size()))];
  return s;
}

core::AuxRow BumpRow(core::AuxRow row, double delta) {
  auto &cell = row.values[7];
  cell = core::AuxValue::Of(core::AuxType::Double, std::get<double>(cell.payload) + delta);
  return row;
}

std::string Num(double v) {
  std::ostringstream os;
  os << std::fixed << std::setprecision(1) << v;
  return os.str();
}

struct Config {
  std::string db, mode;
  int reads = 200;
  std::string aux_sql, base_sql;
};

// One write pattern: returns the cost of one write in microseconds.
class Writer {
public:
  virtual ~Writer() = default;
  // Total cost of one write; `aux_us` receives the aux-maintenance part.
  virtual double Write(duckdb::Connection &con, DuckdbRuntime *runtime, double &aux_us) = 0;
  virtual void Restore(duckdb::Connection &con) {}
};

// Bumps the top result row's value (the catalog probe), like the sweep harness.
class BumpWriter final : public Writer {
public:
  BumpWriter(const catalog::SharedProbe &p) : probe(p), old_row(p.old_row), new_row(p.new_row) {
    delta = p.write_delta;
  }
  double Write(duckdb::Connection &con, DuckdbRuntime *runtime, double &aux_us) override {
    const auto start = Clock::now();
    Exec(con, probe.update_sql);
    aux_us = 0;
    if (runtime) {
      const auto aux_start = Clock::now();
      core::MutationBatch m;
      m.table = probe.table;
      m.kind = core::MutationKind::Update;
      m.updated_columns = probe.updated_columns;
      m.old_rows = {old_row};
      m.new_rows = {new_row};
      runtime->ApplyMutationBatch(m);
      aux_us = ElapsedUs(aux_start);
    }
    const double us = ElapsedUs(start);
    old_row = new_row;
    new_row = BumpRow(new_row, delta);
    return us;
  }
  void Restore(duckdb::Connection &con) override { Exec(con, probe.restore_sql); }

private:
  catalog::SharedProbe probe;
  core::AuxRow old_row, new_row;
  double delta = 1.0;
};

// Mixed workload on the rows of the arguments' unit and fiscal year: inserts,
// deletes, value updates (some touching the current maxima).
class MixedWriter final : public Writer {
public:
  MixedWriter(duckdb::Connection &con, const core::QueryArguments &args)
      : rng(12345), uom(std::get<std::string>(args.values[1].payload)) {
    auto r = con.Query(catalog::BindArguments("SELECT n.adsh, n.tag FROM num n JOIN sub s ON n.adsh = s.adsh "
                                              "WHERE s.fy = $1 AND n.uom = $2 AND n.value IS NOT NULL LIMIT 300",
                                              args));
    Require(r && !r->HasError(), "mixed seed query");
    for (duckdb::idx_t i = 0; i < r->RowCount(); ++i) {
      keys.emplace_back(r->GetValue(0, i).ToString(), r->GetValue(1, i).ToString());
    }
    Require(!keys.empty(), "no rows of the arguments");
  }
  double Write(duckdb::Connection &con, DuckdbRuntime *runtime, double &aux_us) override {
    aux_us = 0;
    const auto &[adsh, tag] = keys[rng() % keys.size()];
    const double value = static_cast<double>(rng() % 1000) * 1000.0;
    const int op = static_cast<int>(rng() % 3);
        const auto start = Clock::now();
    core::MutationBatch m;
    m.table = 1;
    if (op == 0 || inserted.empty()) {
      m.kind = core::MutationKind::Insert;
      Exec(con, "INSERT INTO num VALUES ('" + adsh + "','" + tag + "','v',20220101,4,'" + uom + "',NULL," +
                    Num(value) + ",NULL)");
      m.new_rows = {Row(adsh, tag, value)};
      inserted.push_back({adsh, tag, value});
    } else if (op == 1) {
      m.kind = core::MutationKind::Delete;
      const auto [a, t, v] = inserted.back();
      inserted.pop_back();
      Exec(con, "DELETE FROM num WHERE rowid = (SELECT rowid FROM num WHERE adsh = '" + a +
                    "' AND tag = '" + t + "' AND uom = '" + uom + "' AND value = " + Num(v) + " LIMIT 1)");
      m.old_rows = {Row(a, t, v)};
    } else {
      m.kind = core::MutationKind::Update;
      m.updated_columns = {7};
      auto &[a, t, v] = inserted.back();
      const double nv = v + 1.0;
      Exec(con, "UPDATE num SET value = " + Num(nv) + " WHERE rowid = (SELECT rowid FROM num WHERE adsh = '" +
                    a + "' AND tag = '" + t + "' AND uom = '" + uom + "' AND value = " + Num(v) + " LIMIT 1)");
      m.old_rows = {Row(a, t, v)};
      m.new_rows = {Row(a, t, nv)};
      v = nv;
    }
    if (runtime) {
      const auto aux_start = Clock::now();
      runtime->ApplyMutationBatch(m);
      aux_us = ElapsedUs(aux_start);
    }
    return ElapsedUs(start);
  }
  void Restore(duckdb::Connection &con) override {
    for (const auto &[a, t, v] : inserted) {
      Exec(con, "DELETE FROM num WHERE rowid = (SELECT rowid FROM num WHERE adsh = '" + a +
                    "' AND tag = '" + t + "' AND uom = '" + uom + "' AND value = " + Num(v) + " LIMIT 1)");
    }
    inserted.clear();
  }

private:
  core::AuxRow Row(const std::string &adsh, const std::string &tag, double value) const {
    using core::AuxType;
    using core::AuxValue;
    return {{AuxValue::Of(AuxType::String, adsh), AuxValue::Of(AuxType::String, tag),
             AuxValue::Of(AuxType::String, std::string("v")), AuxValue::Of(AuxType::Int64, int64_t(20220101)),
             AuxValue::Of(AuxType::Int64, int64_t(4)), AuxValue::Of(AuxType::String, uom),
             AuxValue::Null(AuxType::String), AuxValue::Of(AuxType::Double, value),
             AuxValue::Null(AuxType::String)}};
  }
  std::mt19937_64 rng;
  std::string uom;
  std::vector<std::pair<std::string, std::string>> keys;
  std::vector<std::tuple<std::string, std::string, double>> inserted;
};

void Emit(std::ostream &csv, const std::string &line) {
  std::cout << line << std::endl;
  csv << line << '\n';
  csv.flush();
}

} // namespace

int main(int argc, char **argv) {
  try {
    Require(argc >= 2, "usage: benchmark_sec_edgar_q2_sweep <db_copy> [aux|aux_parallel] [reads] [csv]");
    Config cfg;
    cfg.db = argv[1];
    cfg.mode = argc > 2 ? argv[2] : "aux";
    cfg.reads = argc > 3 ? std::atoi(argv[3]) : 200;
    std::ofstream csv(argc > 4 ? argv[4] : "/dev/null", std::ios::app);

    const bool parallel = cfg.mode == "aux_parallel";
#ifdef ACCEL_ENABLE_PARALLEL_AUX
    const auto &entry = parallel ? catalog::LookupSharedQueryParallel("sec_edgar", 2)
                                 : catalog::LookupSharedQuery("sec_edgar", 2);
#else
    Require(!parallel, "built without ACCEL_ENABLE_PARALLEL_AUX");
    const auto &entry = catalog::LookupSharedQuery("sec_edgar", 2);
#endif
    const int threads = std::max(1, std::atoi(EnvStr("ACCEL_BENCH_THREADS", "1").c_str()));
    duckdb::DuckDB database(cfg.db.c_str());
    duckdb::Connection con(database);
    accelerator_codegen::RegisterAcceleratorFunctions(con);
    Exec(con, "PRAGMA threads=" + std::to_string(threads));
    setenv("ACCEL_AUX_THREADS", std::to_string(threads).c_str(), 1);

    const std::string tag = cfg.mode + "/t" + std::to_string(threads);
    Emit(csv, "label,params,regime,pattern,writes_per_read,reads,writes,read_mean_us,read_p50_us,"
              "write_mean_us,write_p50_us,write_p99_us,cycle_mean_us,aux_maint_mean_us,aux_maint_p50_us");

    DuckdbRuntime runtime(con, entry, 10.0);
    runtime.UseManualCapture(); // writers hand-build each change
    const auto build_start = Clock::now();
    runtime.Build();
    const double build_us = ElapsedUs(build_start);
    Emit(csv, tag + ",-,build,-,0,0,0," + Num(build_us) + "," + Num(build_us) + ",0,0,0,0");

    std::vector<int> rates;
    {
      std::stringstream ss(EnvStr("Q2_BENCH_WRITES_PER_READ", "1,10,100,1000"));
      std::string item;
      while (std::getline(ss, item, ',')) {
        rates.push_back(std::atoi(item.c_str()));
      }
    }
    const int cycles = std::atoi(EnvStr("Q2_BENCH_CYCLES", "20").c_str());
    const std::string pattern_env = EnvStr("Q2_BENCH_PATTERN", "both");
    std::vector<std::string> patterns;
    if (pattern_env == "both" || pattern_env == "bump") {
      patterns.push_back("bump");
    }
    if (pattern_env == "both" || pattern_env == "mixed") {
      patterns.push_back("mixed");
    }

    int swept = 0;
    for (const auto &args : harness::ArgumentSets(entry)) {
      const std::string base_sql = catalog::BindArguments(entry.base_sql, args);
      const std::string aux_sql = catalog::BindArguments(entry.duckdb_aux_sql, args);
      std::string params = harness::ArgumentLiterals(args);
      params.erase(std::remove(params.begin(), params.end(), ' '), params.end());
      std::replace(params.begin(), params.end(), ',', '|');

      // -------- plain DuckDB: read-only.
      if (Drain(con, base_sql) == 0) {
        std::cerr << "skipping (" << harness::ArgumentLiterals(args) << "): empty answer\n";
        continue;
      }
      ++swept;
      Drain(con, base_sql);
      std::vector<double> base_reads;
      for (int i = 0; i < std::min(10, std::max(5, cfg.reads / 20)); ++i) {
        const auto s = Clock::now();
        Drain(con, base_sql);
        base_reads.push_back(ElapsedUs(s));
      }
      const auto base_read = Summarize(base_reads);
      Emit(csv, "duckdb/t" + std::to_string(threads) + "," + params + ",read-only,-,0," +
                    std::to_string(base_reads.size()) + ",0," + Num(base_read.mean) + "," + Num(base_read.p50) +
                    ",0,0,0," + Num(base_read.mean));

      // -------- aux read-only.
      for (int i = 0; i < 5; ++i) {
        Drain(con, aux_sql);
      }
      std::vector<double> aux_reads;
      for (int i = 0; i < cfg.reads; ++i) {
        const auto s = Clock::now();
        Drain(con, aux_sql);
        aux_reads.push_back(ElapsedUs(s));
      }
      const auto aux_read = Summarize(aux_reads);
      std::vector<double> direct_reads;
      for (int i = 0; i < cfg.reads * 5; ++i) {
        const auto s = Clock::now();
        (void)runtime.ExecuteCollect(args);
        direct_reads.push_back(ElapsedUs(s));
      }
      const auto direct = Summarize(direct_reads);
      Emit(csv, tag + "/module-only," + params + ",read-only,-,0," + std::to_string(direct_reads.size()) + ",0," +
                    Num(direct.mean) + "," + Num(direct.p50) + ",0,0,0," + Num(direct.mean));
      harness::RequireSharedMatchesBase(con, runtime, entry, args);
      Emit(csv, tag + "," + params + ",read-only,-,0," + std::to_string(aux_reads.size()) + ",0," +
                    Num(aux_read.mean) + "," + Num(aux_read.p50) + ",0,0,0," + Num(aux_read.mean));

      // -------- write sweeps.
      if (pattern_env == "none") {
        continue;
      }
      const auto probe = catalog::FindInvalidatingSharedProbe(con, entry, args);
      for (const auto &pattern : patterns) {
        for (const bool aux : {false, true}) {
          // Fresh state per series: rebuild the aux state from the current DB.
          if (aux) {
            runtime.Build();
          }
          std::unique_ptr<Writer> writer;
          if (pattern == "bump") {
            writer = std::make_unique<BumpWriter>(probe);
          } else {
            writer = std::make_unique<MixedWriter>(con, args);
          }
          for (const int rate : rates) {
            // base reads are ~seconds: fewer cycles for the plain-DuckDB series
            const int n_cycles = aux ? cycles : std::max(2, cycles / 10);
            std::vector<double> read_us, write_us, cycle_us, aux_write_us;
            for (int c = 0; c < n_cycles; ++c) {
              double cycle = 0;
              for (int w = 0; w < rate; ++w) {
                double aux_us = 0;
                const double us = writer->Write(con, aux ? &runtime : nullptr, aux_us);
                write_us.push_back(us);
                aux_write_us.push_back(aux_us);
                cycle += us;
              }
              const auto s = Clock::now();
              Drain(con, aux ? aux_sql : base_sql);
              const double r = ElapsedUs(s);
              read_us.push_back(r);
              cycle_us.push_back(cycle + r);
            }
            if (aux) {
              harness::RequireSharedMatchesBase(con, runtime, entry, args);
            }
            const auto rs = Summarize(read_us), ws = Summarize(write_us), cs = Summarize(cycle_us);
            const auto as = Summarize(aux_write_us);
            Emit(csv, (aux ? tag : "duckdb/t" + std::to_string(threads)) + "," + params + ",writes," + pattern +
                          "," + std::to_string(rate) + "," + std::to_string(rs.n) + "," + std::to_string(ws.n) +
                          "," + Num(rs.mean) + "," + Num(rs.p50) + "," + Num(ws.mean) + "," + Num(ws.p50) + "," +
                          Num(ws.p99) + "," + Num(cs.mean) + "," + Num(as.mean) + "," + Num(as.p50));
          }
          writer->Restore(con);
        }
      }
      // Restore the DB (bump series) and verify the aux state follows via rebuild.
      Exec(con, probe.restore_sql);
      runtime.Build();
      harness::RequireSharedMatchesBase(con, runtime, entry, args);
    }
    Require(swept > 0, "every drawn parameter set had an empty answer");
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "benchmark_sec_edgar_q2_sweep failed: " << e.what() << std::endl;
    return 1;
  }
}
