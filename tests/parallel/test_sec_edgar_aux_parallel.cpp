#ifndef ACCEL_ENABLE_PARALLEL_AUX
#error "test_sec_edgar_aux_parallel requires -DACCEL_ENABLE_PARALLEL_AUX"
#endif

#include "accelerator_codegen/harness/differential.hpp"
#include "accelerator_codegen/runtime/duckdb/register.hpp"
#include "accelerator_codegen/runtime/duckdb/duckdb_runtime.hpp"
#include "catalog/query_catalog.hpp"
#include "sec_edgar/q1/duckdb/aux/parallel/module.hpp"

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace catalog = accelerator_codegen::aux_state::engine;
namespace harness = accelerator_codegen::aux_state::harness;
using catalog::duckdb::DuckdbRuntime;
using accelerator_codegen::aux_state::core::AuxEngineServices;
using accelerator_codegen::aux_state::core::AuxRow;
using accelerator_codegen::aux_state::core::AuxRowSink;
using accelerator_codegen::aux_state::core::AuxStateStore;
using accelerator_codegen::aux_state::core::AuxType;
using accelerator_codegen::aux_state::core::AuxValue;
using accelerator_codegen::aux_state::core::OutputSchema;
using accelerator_codegen::aux_state::core::QueryKey;
using accelerator_codegen::aux_state::core::StateBuilder;
using accelerator_codegen::aux_state::core::StateImage;
using accelerator_codegen::aux_state::core::StateSnapshot;
using accelerator_codegen::aux_state::core::StagedMaintenanceOutcome;
using accelerator_codegen::aux_state::core::ValidityResult;
using accelerator_codegen::aux_state::core::Version;
using accelerator_codegen::aux_state::core::VisibilityToken;
using accelerator_codegen::aux_state::query::SecEdgarQ1Config;
using accelerator_codegen::aux_state::query::SecEdgarQ1InitialScan;
using accelerator_codegen::aux_state::query::SecEdgarQ1Module;
using accelerator_codegen::aux_state::query::SecEdgarQ1ParallelModule;
using accelerator_codegen::aux_state::query::SecEdgarQ1PreInput;

constexpr const char *kDefaultTestDb =
    "/home/akhilvb/accelerator-codegen-duckdb/sec_edgar_test.duckdb";
constexpr const char *kDefaultSf3Db =
    "/home/akhilvb/accelerator-codegen-duckdb/sec_edgar_sf3.duckdb";
constexpr int kSecQueries[] = {1, 2, 3, 4, 6, 24};

void Require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

bool FileExists(const std::string &path) {
  std::ifstream in(path);
  return in.good();
}

int HardwareThreads() {
  const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
  return static_cast<int>(std::min(hw, 8u));
}

int EnvInt(const char *name, int fallback) {
  const char *value = std::getenv(name);
  return value && *value ? std::stoi(value) : fallback;
}

void SetThreads(duckdb::Connection &connection, int threads) {
  auto result = connection.Query("PRAGMA threads=" + std::to_string(threads));
  Require(result && !result->HasError(),
          "failed to set threads=" + std::to_string(threads));
}

class ScopedEnv {
public:
  ScopedEnv(const char *name, const std::string &value) : name_(name) {
    if (const char *old = std::getenv(name)) {
      old_ = old;
      had_ = true;
    }
    setenv(name, value.c_str(), 1);
  }
  ~ScopedEnv() {
    if (had_) {
      setenv(name_.c_str(), old_.c_str(), 1);
    } else {
      unsetenv(name_.c_str());
    }
  }
  ScopedEnv(const ScopedEnv &) = delete;
  ScopedEnv &operator=(const ScopedEnv &) = delete;

private:
  std::string name_;
  std::string old_;
  bool had_ = false;
};

std::string Normalize(const duckdb::Value &value) {
  if (value.IsNull()) {
    return "NULL";
  }
  try {
    const auto source = value.ToString();
    std::size_t parsed = 0;
    const auto numeric = std::stold(source, &parsed);
    if (parsed != source.size()) {
      return source;
    }
    std::ostringstream text;
    text << std::fixed << std::setprecision(6) << numeric;
    return text.str();
  } catch (...) {
    return value.ToString();
  }
}

bool ValuesApproxEqual(const std::string &lhs, const std::string &rhs) {
  if (lhs == rhs) {
    return true;
  }
  try {
    const long double a = std::stold(lhs);
    const long double b = std::stold(rhs);
    const long double scale = std::max<long double>({1.0L, std::fabsl(a), std::fabsl(b)});
    return std::fabsl(a - b) <= scale * 1e-6L;
  } catch (...) {
    return false;
  }
}

void RequireEqualResults(duckdb::MaterializedQueryResult &lhs,
                         duckdb::MaterializedQueryResult &rhs,
                         const std::string &label, bool unordered) {
  Require(!lhs.HasError(), label + ": lhs error: " + lhs.GetError());
  Require(!rhs.HasError(), label + ": rhs error: " + rhs.GetError());
  Require(lhs.RowCount() == rhs.RowCount(),
          label + ": row count " + std::to_string(lhs.RowCount()) + " vs " +
              std::to_string(rhs.RowCount()));
  Require(lhs.ColumnCount() == rhs.ColumnCount(), label + ": column count differs");
  auto row_key = [&](duckdb::MaterializedQueryResult &result, duckdb::idx_t row) {
    std::ostringstream key;
    for (duckdb::idx_t col = 0; col < result.ColumnCount(); ++col) {
      if (col != 0) {
        key << '\t';
      }
      key << Normalize(result.GetValue(col, row));
    }
    return key.str();
  };
  if (unordered) {
    std::vector<std::pair<std::string, duckdb::idx_t>> left;
    std::vector<std::pair<std::string, duckdb::idx_t>> right;
    for (duckdb::idx_t row = 0; row < lhs.RowCount(); ++row) {
      left.push_back({row_key(lhs, row), row});
      right.push_back({row_key(rhs, row), row});
    }
    std::sort(left.begin(), left.end());
    std::sort(right.begin(), right.end());
    for (std::size_t i = 0; i < left.size(); ++i) {
      for (duckdb::idx_t col = 0; col < lhs.ColumnCount(); ++col) {
        const auto lv = Normalize(lhs.GetValue(col, left[i].second));
        const auto rv = Normalize(rhs.GetValue(col, right[i].second));
        Require(ValuesApproxEqual(lv, rv),
                label + ": unordered mismatch at " + std::to_string(i) +
                    " col " + std::to_string(col) + " " + lv + " vs " + rv);
      }
    }
    return;
  }
  for (duckdb::idx_t row = 0; row < lhs.RowCount(); ++row) {
    for (duckdb::idx_t col = 0; col < lhs.ColumnCount(); ++col) {
      const auto lv = Normalize(lhs.GetValue(col, row));
      const auto rv = Normalize(rhs.GetValue(col, row));
      Require(ValuesApproxEqual(lv, rv),
              label + ": mismatch row " + std::to_string(row) + " col " +
                  std::to_string(col) + " " + lv + " vs " + rv);
    }
  }
}

void LoadQuery(duckdb::Connection &connection, const std::string &sql,
               const std::string &label, duckdb::unique_ptr<duckdb::QueryResult> &out) {
  out = connection.Query(sql);
  Require(out != nullptr, label + ": null result");
  Require(!out->HasError(), label + ": " + out->GetError());
}

void CompareTriple(duckdb::Connection &connection, const catalog::SharedQueryCatalogEntry &st,
                   const catalog::SharedQueryCatalogEntry &parallel, int threads) {
  ScopedEnv aux("ACCEL_AUX_THREADS", std::to_string(threads));
  SetThreads(connection, 1);
  const std::string label =
      "Q" + std::to_string(st.query_id) + " t=" + std::to_string(threads);
  duckdb::unique_ptr<duckdb::QueryResult> base;
  duckdb::unique_ptr<duckdb::QueryResult> st_aux;
  duckdb::unique_ptr<duckdb::QueryResult> mt_aux;
  duckdb::unique_ptr<duckdb::QueryResult> mt_repeat;
  LoadQuery(connection, st.base_sql, label + " base", base);
  LoadQuery(connection, st.duckdb_aux_sql, label + " st", st_aux);
  LoadQuery(connection, parallel.duckdb_aux_sql, label + " mt", mt_aux);
  LoadQuery(connection, parallel.duckdb_aux_sql, label + " mt-repeat", mt_repeat);
  auto &b = base->Cast<duckdb::MaterializedQueryResult>();
  auto &s = st_aux->Cast<duckdb::MaterializedQueryResult>();
  auto &p = mt_aux->Cast<duckdb::MaterializedQueryResult>();
  auto &p2 = mt_repeat->Cast<duckdb::MaterializedQueryResult>();
  RequireEqualResults(b, s, label + " base vs st", true);
  RequireEqualResults(s, p, label + " st vs mt", true);
  RequireEqualResults(p, p2, label + " mt determinism", true);
}

void CompareRuntimes(duckdb::Connection &connection,
                     const catalog::SharedQueryCatalogEntry &st,
                     const catalog::SharedQueryCatalogEntry &parallel, int threads) {
  ScopedEnv aux("ACCEL_AUX_THREADS", std::to_string(threads));
  SetThreads(connection, threads);
  DuckdbRuntime st_runtime(connection, st, 10.0);
  DuckdbRuntime mt_runtime(connection, parallel, 10.0);
  st_runtime.Build();
  mt_runtime.Build();
  harness::RequireSharedMatchesBase(connection, st_runtime, st);
  harness::RequireSharedMatchesBase(connection, mt_runtime, parallel);
  const auto st_rows = st_runtime.ExecuteCollect();
  const auto mt_rows = mt_runtime.ExecuteCollect();
  Require(st_rows.size() == mt_rows.size(),
          "Q" + std::to_string(st.query_id) + " runtime row count mismatch");
}

void CopyDb(const std::filesystem::path &src, const std::filesystem::path &dst) {
  Require(std::filesystem::exists(src), "missing source DB: " + src.string());
  Require(src != dst, "work DB path must differ from source DB");
  if (dst.has_parent_path() && !dst.parent_path().empty()) {
    std::filesystem::create_directories(dst.parent_path());
  }
  std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing);
}

std::filesystem::path WorkCopyPath(int query_id, int threads, const char *tag) {
  return std::filesystem::temp_directory_path() /
         ("accel_parallel_aux_" + std::string(tag) + "_q" + std::to_string(query_id) +
          "_t" + std::to_string(threads) + ".duckdb");
}

void SeedSyntheticSecEdgar(duckdb::Connection &connection) {
  auto exec = [&](const std::string &sql) {
    auto result = connection.Query(sql);
    Require(result && !result->HasError(), result ? result->GetError() : sql);
  };
  exec(R"SQL(
    CREATE TABLE sub (
      adsh VARCHAR, cik INTEGER, name VARCHAR, sic INTEGER, fy INTEGER, fye INTEGER,
      form VARCHAR, period INTEGER, filed INTEGER
    );
    CREATE TABLE tag (
      tag VARCHAR, version VARCHAR, custom INTEGER, abstract INTEGER,
      datatype VARCHAR, iord VARCHAR, crdr VARCHAR, tlabel VARCHAR, doc VARCHAR
    );
    CREATE TABLE pre (
      adsh VARCHAR, report INTEGER, line INTEGER, stmt VARCHAR, inpth INTEGER,
      rfile VARCHAR, tag VARCHAR, version VARCHAR, plabel VARCHAR, negating INTEGER
    );
    CREATE TABLE num (
      adsh VARCHAR, tag VARCHAR, version VARCHAR, ddate INTEGER, qtrs INTEGER,
      uom VARCHAR, coreg VARCHAR, value DOUBLE, footnote VARCHAR
    );
  )SQL");

  for (int i = 0; i < 64; ++i) {
    const std::string adsh = "adsh-" + std::to_string(i);
    const int fy = (i % 2 == 0) ? 2022 : 2023;
    const int sic = 4000 + (i % 8);
    const int cik = 1000 + (i % 5);
    exec("INSERT INTO sub VALUES ('" + adsh + "', " + std::to_string(cik) + ", 'Co" +
         std::to_string(cik) + "', " + std::to_string(sic) + ", " + std::to_string(fy) +
         ", 1231, '10-K', 20221231, 20230301)");
    exec("INSERT INTO pre VALUES ('" + adsh + "', 1, " + std::to_string(10 + i) +
         ", 'BS', 0, 'A', 'Assets', 'us-gaap', 'Assets label', 0)");
    if (i < 40) {
      exec("INSERT INTO pre VALUES ('" + adsh + "', 1, " + std::to_string(20 + i) +
           ", 'IS', 0, 'B', 'Revenue', 'us-gaap', 'Revenue label', 0)");
    }
    if (i < 20) {
      exec("INSERT INTO pre VALUES ('" + adsh + "', 1, " + std::to_string(30 + i) +
           ", 'EQ', 0, 'C', 'Equity', 'us-gaap', 'Equity label', 0)");
    }
    exec("INSERT INTO num VALUES ('" + adsh +
         "', 'Assets', 'us-gaap', 20221231, 4, 'pure', NULL, " +
         std::to_string(1.5 + i) + ", NULL)");
    exec("INSERT INTO num VALUES ('" + adsh +
         "', 'Revenue', 'us-gaap', 20230630, 4, 'USD', NULL, " +
         std::to_string(100.0 + i) + ", NULL)");
    exec("INSERT INTO num VALUES ('" + adsh +
         "', 'Equity', 'us-gaap', 20221231, 4, 'USD', NULL, " +
         std::to_string(50.0 + i) + ", NULL)");
  }
  exec("INSERT INTO tag VALUES ('Assets', 'us-gaap', 0, 0, 'monetary', 'I', 'D', "
       "'Assets tlabel', '')");
  exec("INSERT INTO tag VALUES ('Revenue', 'us-gaap', 0, 0, 'monetary', 'I', 'C', "
       "'Revenue tlabel', '')");
  exec("INSERT INTO tag VALUES ('Equity', 'us-gaap', 0, 0, 'monetary', 'I', 'C', "
       "'Equity tlabel', '')");

  // Q24 needs >10 unmatched USD 2023 facts for one tag/version.
  for (int i = 0; i < 16; ++i) {
    exec("INSERT INTO num VALUES ('orphan-" + std::to_string(i) +
         "', 'Orphan', 'us-gaap', 20230615, 1, 'USD', NULL, " + std::to_string(3.0 + i) +
         ", NULL)");
  }
}

// Enough rows for DuckDB to form 2+ row groups so in-kernel workers actually
// run concurrently (NextParallelScan assigns work by ~122880-row groups).
void SeedLargeSyntheticSecEdgar(duckdb::Connection &connection) {
  SeedSyntheticSecEdgar(connection);
  auto exec = [&](const std::string &sql) {
    auto result = connection.Query(sql);
    Require(result && !result->HasError(), result ? result->GetError() : sql);
  };
  constexpr const char *kRows = "160000";
  exec(std::string(R"SQL(
    INSERT INTO pre
    SELECT 'adsh-' || (i % 64)::VARCHAR, 1, 1000 + i::INTEGER,
           CASE WHEN i % 3 = 0 THEN 'BS' WHEN i % 3 = 1 THEN 'IS' ELSE 'EQ' END,
           0, 'A',
           CASE WHEN i % 3 = 0 THEN 'Assets' WHEN i % 3 = 1 THEN 'Revenue' ELSE 'Equity' END,
           'us-gaap', 'label', 0
    FROM range()SQL") + kRows + ") t(i)");
  exec(std::string(R"SQL(
    INSERT INTO num
    SELECT 'adsh-' || (i % 64)::VARCHAR,
           CASE WHEN i % 3 = 0 THEN 'Assets' WHEN i % 3 = 1 THEN 'Revenue' ELSE 'Equity' END,
           'us-gaap',
           CASE WHEN i % 2 = 0 THEN 20221231 ELSE 20230630 END,
           4,
           CASE WHEN i % 3 = 0 THEN 'pure' ELSE 'USD' END,
           NULL, 1.0 + (i % 100), NULL
    FROM range()SQL") + kRows + ") t(i)");
  exec(std::string(R"SQL(
    INSERT INTO num
    SELECT 'orphan-' || (i % 16)::VARCHAR, 'Orphan', 'us-gaap', 20230615, 1,
           'USD', NULL, 3.0, NULL
    FROM range()SQL") + kRows + ") t(i)");
}

void ApplyInvalidatingWrite(duckdb::Connection &connection,
                            const catalog::SharedQueryCatalogEntry &parallel,
                            int threads) {
  SetThreads(connection, threads);
  ScopedEnv aux("ACCEL_AUX_THREADS", std::to_string(threads));
  DuckdbRuntime runtime(connection, parallel, 10.0);
  runtime.Build();
  harness::RequireSharedMatchesBase(connection, runtime, parallel);
  auto probe = catalog::FindInvalidatingSharedProbe(connection, parallel);
  runtime.Apply(probe);
  harness::RequireSharedMatchesBase(connection, runtime, parallel);
  auto restore = connection.Query(probe.restore_sql);
  Require(restore && !restore->HasError(),
          restore ? restore->GetError() : "synthetic restore failed");
}

void RunCatalogChecks() {
  for (int query_id : kSecQueries) {
    const auto &st = catalog::LookupSharedQuery(catalog::SharedWorkloadKind::SecEdgar, query_id);
    const auto &parallel =
        catalog::LookupSharedQueryParallel(catalog::SharedWorkloadKind::SecEdgar, query_id);
    Require(std::string(st.accelerator_id).find("_aux_parallel") == std::string::npos,
            "ST accelerator id must not be parallel");
    Require(catalog::IsParallelAuxAccelerator(parallel.accelerator_id),
            "parallel catalog id must end in _aux_parallel");
    Require(std::string(parallel.duckdb_aux_sql).find("_aux_parallel()") != std::string::npos,
            "parallel DuckDB SQL name");
    Require(std::string(st.duckdb_aux_sql) != std::string(parallel.duckdb_aux_sql),
            "ST and parallel SQL names must differ");
    Require(std::string(st.base_sql) == std::string(parallel.base_sql),
            "parallel catalog must reuse ST base SQL");
  }
}

class UnitStore final : public AuxStateStore {
public:
  StateSnapshot Read(const VisibilityToken &visibility) const override {
    StateSnapshot snapshot;
    snapshot.accelerator = accelerator;
    snapshot.visibility = visibility;
    if (!valid) {
      return snapshot;
    }
    for (const auto &entry : images) {
      snapshot.partitions.push_back(entry.second);
    }
    snapshot.available = images.size() == 2;
    return snapshot;
  }
  ValidityResult CheckValidity(const QueryKey &key, const VisibilityToken &) const override {
    if (key.accelerator != accelerator) {
      return {accelerator_codegen::aux_state::core::ValidityCode::Missing, 0};
    }
    return {valid ? accelerator_codegen::aux_state::core::ValidityCode::Valid
                  : accelerator_codegen::aux_state::core::ValidityCode::Invalidated,
            valid ? version : 0};
  }
  void Invalidate(const accelerator_codegen::aux_state::core::InvalidationPlan &) override {
    valid = false;
  }
  bool TryPublish(const StagedMaintenanceOutcome &outcome, Version) override {
    for (const auto &image : outcome.updates) {
      images[image.partition] = image;
    }
    version++;
    valid = images.size() == 2;
    return true;
  }
  void Install(std::vector<StateImage> built) {
    images.clear();
    for (auto &image : built) {
      images.emplace(image.partition, std::move(image));
    }
    version = 1;
    valid = images.size() == 2;
  }
  std::string accelerator = "sec_edgar_q1_aux_parallel";
  std::map<accelerator_codegen::aux_state::core::PartitionId, StateImage> images;
  Version version = 0;
  bool valid = false;
};

class UnitServices final : public AuxEngineServices, public SecEdgarQ1InitialScan {
public:
  void ScanPre(const std::function<void(const SecEdgarQ1PreInput &)> &consumer) const override {
    for (int i = 0; i < 32; ++i) {
      consumer({"BS", "A", false, false, "adsh-" + std::to_string(i),
                static_cast<int32_t>(10 + i)});
    }
    consumer({"IS", "B", false, false, "other", 5});
  }
  VisibilityToken CurrentVisibility() const override { return {}; }
  AuxStateStore &StateStore() override { return store; }
  std::size_t WorkerCount() const override { return workers; }
  UnitStore store;
  std::size_t workers = 4;
};

class UnitBuilder final : public StateBuilder {
public:
  void Write(StateImage image) override { images.push_back(std::move(image)); }
  void Finish() override { finished = true; }
  std::vector<StateImage> images;
  bool finished = false;
};

class UnitSink final : public AuxRowSink {
public:
  void Begin(const OutputSchema &) override {}
  void WriteRow(const AuxRow &row) override { rows.push_back(row); }
  void End() override { ended = true; }
  std::vector<AuxRow> rows;
  bool ended = false;
};

void RunUnitFallbackMerge() {
  UnitServices services;
  SecEdgarQ1ParallelModule parallel(SecEdgarQ1Config{1}, &services);
  SecEdgarQ1Module serial(SecEdgarQ1Config{1}, &services);
  UnitBuilder pb;
  UnitBuilder sb;
  parallel.Build(services, pb);
  serial.Build(services, sb);
  Require(pb.finished && sb.finished, "unit build did not finish");
  services.store.Install(pb.images);
  UnitSink ps;
  UnitSink ss;
  parallel.Execute(services, {}, ps);
  services.store.accelerator = "sec_edgar_q1";
  services.store.Install(sb.images);
  serial.Execute(services, {}, ss);
  Require(ps.ended && ss.ended, "unit execute did not end");
  Require(ps.rows.size() == ss.rows.size(), "unit row count mismatch");
  Require(ps.rows.size() == 2, "expected two Q1 groups");
}

void RunSyntheticSuite() {
  duckdb::DuckDB database(nullptr);
  duckdb::Connection connection(database);
  accelerator_codegen::RegisterAcceleratorFunctions(connection);
  SeedSyntheticSecEdgar(connection);
  const int n = HardwareThreads();
  for (int query_id : kSecQueries) {
    const auto &st =
        catalog::LookupSharedQuery(catalog::SharedWorkloadKind::SecEdgar, query_id);
    const auto &parallel =
        catalog::LookupSharedQueryParallel(catalog::SharedWorkloadKind::SecEdgar, query_id);
    CompareRuntimes(connection, st, parallel, 1);
    CompareRuntimes(connection, st, parallel, n);
    CompareTriple(connection, st, parallel, 1);
    CompareTriple(connection, st, parallel, n);
  }
  for (int query_id : kSecQueries) {
    const auto &parallel =
        catalog::LookupSharedQueryParallel(catalog::SharedWorkloadKind::SecEdgar, query_id);
    std::cerr << "synthetic invalidating write Q" << query_id << " t=1\n";
    ApplyInvalidatingWrite(connection, parallel, 1);
    std::cerr << "synthetic invalidating write Q" << query_id << " t=" << n << '\n';
    ApplyInvalidatingWrite(connection, parallel, n);
  }
}

void RunLargeSyntheticSuite() {
  duckdb::DuckDB database(nullptr);
  duckdb::Connection connection(database);
  accelerator_codegen::RegisterAcceleratorFunctions(connection);
  SeedLargeSyntheticSecEdgar(connection);
  const int n = HardwareThreads();
  std::cerr << "large synthetic MT scans threads=" << n << '\n';
  ScopedEnv aux("ACCEL_AUX_THREADS", std::to_string(n));
  for (int query_id : kSecQueries) {
    const auto &st =
        catalog::LookupSharedQuery(catalog::SharedWorkloadKind::SecEdgar, query_id);
    const auto &parallel =
        catalog::LookupSharedQueryParallel(catalog::SharedWorkloadKind::SecEdgar, query_id);
    CompareTriple(connection, st, parallel, n);
    std::cerr << "large synthetic Q" << query_id << " OK\n";
  }
}

std::string FileDbPath() {
  if (const char *env = std::getenv("ACCEL_PARALLEL_AUX_DB"); env && *env) {
    Require(std::string(env).find("sec_edgar_sf3") == std::string::npos ||
                EnvInt("ACCEL_PARALLEL_AUX_SF3", 0),
            "refusing to open SF3 without ACCEL_PARALLEL_AUX_SF3=1");
    return env;
  }
  if (FileExists(kDefaultTestDb)) {
    return kDefaultTestDb;
  }
  if (EnvInt("ACCEL_PARALLEL_AUX_SF3", 0) && FileExists(kDefaultSf3Db)) {
    return kDefaultSf3Db;
  }
  return {};
}

void RunFileReadOnlySuite(const std::string &path) {
  duckdb::DuckDB database(path.c_str());
  duckdb::Connection connection(database);
  accelerator_codegen::RegisterAcceleratorFunctions(connection);
  const int n = HardwareThreads();
  std::cerr << "file read-only suite db=" << path << " threads=" << n << '\n';
  for (int query_id : kSecQueries) {
    const auto &st =
        catalog::LookupSharedQuery(catalog::SharedWorkloadKind::SecEdgar, query_id);
    const auto &parallel =
        catalog::LookupSharedQueryParallel(catalog::SharedWorkloadKind::SecEdgar, query_id);
    CompareRuntimes(connection, st, parallel, 1);
    CompareRuntimes(connection, st, parallel, n);
    CompareTriple(connection, st, parallel, 1);
    CompareTriple(connection, st, parallel, n);
  }
}

void ApplyProbeAndCheck(duckdb::Connection &connection, DuckdbRuntime &runtime,
                        const catalog::SharedQueryCatalogEntry &parallel,
                        catalog::SharedProbe probe) {
  const auto before_fp = catalog::FingerprintSharedBaseSql(connection, parallel);
  Require(catalog::SharedProbeChangesBaseResult(connection, parallel, probe, before_fp),
          "forced probe does not change DuckDB Q" + std::to_string(parallel.query_id));
  try {
    runtime.Apply(probe);
    Require(catalog::FingerprintSharedBaseSql(connection, parallel) != before_fp,
            "Apply() did not invalidate DuckDB Q" + std::to_string(parallel.query_id));
    harness::RequireSharedMatchesBase(connection, runtime, parallel);
  } catch (...) {
    auto restore = connection.Query(probe.restore_sql);
    (void)restore;
    throw;
  }
  auto restore = connection.Query(probe.restore_sql);
  Require(restore && !restore->HasError(),
          restore ? restore->GetError() : "restore failed");
  runtime.Build();
  harness::RequireSharedMatchesBase(connection, runtime, parallel);
}

std::vector<catalog::SharedProbe>
CollectWriteProbes(duckdb::Connection &connection,
                   const catalog::SharedQueryCatalogEntry &parallel) {
  try {
    return catalog::CollectInvalidatingSharedProbes(connection, parallel, 4096, 5);
  } catch (const std::exception &) {
  }
  const std::string probe_sql =
      catalog::ExpandProbeSelectLimit(parallel.probe_select_sql, 64);
  auto result = connection.Query(probe_sql);
  Require(result && !result->HasError() && result->RowCount() >= 1,
          result ? ("forced probe select failed for Q" +
                    std::to_string(parallel.query_id) + ": " + result->GetError())
                 : "forced probe select returned null");
  auto &materialized = result->Cast<duckdb::MaterializedQueryResult>();
  const auto before_fp = catalog::FingerprintSharedBaseSql(connection, parallel);
  std::vector<catalog::SharedProbe> probes;
  const bool num_probe = parallel.query_id != 1;
  for (duckdb::idx_t row = 0; row < materialized.RowCount() && probes.size() < 5;
       ++row) {
    auto probe = parallel.build_probe(materialized, row);
    if (num_probe) {
      const auto rowid = materialized.GetValue(0, row).ToString();
      const auto value = materialized.GetValue(8, row).ToString();
      const double original = std::stod(value);
      probe.update_sql =
          "UPDATE num SET value = value + 1000000 WHERE rowid = " + rowid;
      probe.restore_sql =
          "UPDATE num SET value = " + value + " WHERE rowid = " + rowid;
      probe.write_delta = 1000000.0;
      probe.new_row = probe.old_row;
      probe.new_row.values[7] =
          accelerator_codegen::aux_state::core::AuxValue::Of(
              accelerator_codegen::aux_state::core::AuxType::Double,
              original + 1000000.0);
    }
    if (catalog::SharedProbeChangesBaseResult(connection, parallel, probe, before_fp)) {
      probes.push_back(std::move(probe));
    }
  }
  if (probes.empty() && parallel.query_id == 4) {
    auto q4 = connection.Query(R"SQL(
      SELECT n.rowid, n.adsh, n.tag, n.version, n.ddate, n.qtrs, n.uom, n.coreg, n.value
      FROM num n
      JOIN sub s ON n.adsh = s.adsh
      JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version
      JOIN tag t ON n.tag = t.tag AND n.version = t.version
      WHERE n.uom = 'USD' AND p.stmt = 'EQ' AND s.sic BETWEEN 4000 AND 4999
        AND n.value IS NOT NULL AND t.abstract = 0
        AND (s.sic, t.tlabel) IN (
          SELECT s2.sic, t2.tlabel
          FROM num n2
          JOIN sub s2 ON n2.adsh = s2.adsh
          JOIN pre p2 ON n2.adsh = p2.adsh AND n2.tag = p2.tag AND n2.version = p2.version
          JOIN tag t2 ON n2.tag = t2.tag AND n2.version = t2.version
          WHERE n2.uom = 'USD' AND p2.stmt = 'EQ' AND s2.sic BETWEEN 4000 AND 4999
            AND n2.value IS NOT NULL AND t2.abstract = 0
          GROUP BY s2.sic, t2.tlabel
          HAVING COUNT(DISTINCT s2.cik) >= 2
        )
      ORDER BY n.value DESC NULLS LAST
      LIMIT 16
    )SQL");
    Require(q4 && !q4->HasError() && q4->RowCount() >= 1,
            q4 && q4->HasError() ? q4->GetError()
                                 : "Q4 qualifying-group probe select returned no rows");
    auto &q4mat = q4->Cast<duckdb::MaterializedQueryResult>();
    for (duckdb::idx_t row = 0; row < q4mat.RowCount() && probes.size() < 5; ++row) {
      auto probe = parallel.build_probe(q4mat, row);
      const auto rowid = q4mat.GetValue(0, row).ToString();
      const auto value = q4mat.GetValue(8, row).ToString();
      const double original = std::stod(value);
      probe.update_sql =
          "UPDATE num SET value = value + 1000000 WHERE rowid = " + rowid;
      probe.restore_sql =
          "UPDATE num SET value = " + value + " WHERE rowid = " + rowid;
      probe.write_delta = 1000000.0;
      probe.new_row = probe.old_row;
      probe.new_row.values[7] =
          accelerator_codegen::aux_state::core::AuxValue::Of(
              accelerator_codegen::aux_state::core::AuxType::Double,
              original + 1000000.0);
      if (catalog::SharedProbeChangesBaseResult(connection, parallel, probe, before_fp)) {
        probes.push_back(std::move(probe));
      }
    }
  }
  Require(!probes.empty(),
          "could not construct an invalidating write probe for Q" +
              std::to_string(parallel.query_id));
  return probes;
}

void RunCopiedWriteDifferential(const std::string &work,
                                const catalog::SharedQueryCatalogEntry &parallel,
                                int threads) {
  ScopedEnv aux("ACCEL_AUX_THREADS", std::to_string(threads));
  duckdb::DuckDB database(work.c_str());
  duckdb::Connection connection(database);
  SetThreads(connection, threads);
  DuckdbRuntime runtime(connection, parallel, 10.0);
  runtime.Build();
  harness::RequireSharedMatchesBase(connection, runtime, parallel);
  std::vector<catalog::SharedProbe> probes;
  try {
    probes = CollectWriteProbes(connection, parallel);
  } catch (const std::exception &error) {
    auto base = connection.Query(parallel.base_sql);
    const bool empty =
        base && !base->HasError() && base->RowCount() == 0;
    if (empty) {
      std::cerr << "skip writes Q" << parallel.query_id
                << " (empty base result; covered by synthetic writes)\n";
      return;
    }
    throw std::runtime_error(std::string("Q") + std::to_string(parallel.query_id) +
                             " write probes: " + error.what());
  }
  ApplyProbeAndCheck(connection, runtime, parallel, probes.front());
  const int nprobes = std::min(3, static_cast<int>(probes.size()));
  for (int i = 0; i < nprobes; ++i) {
    ApplyProbeAndCheck(connection, runtime, parallel, probes[static_cast<std::size_t>(i)]);
  }
  std::vector<catalog::SharedProbe> applied;
  try {
    for (int i = 0; i < nprobes; ++i) {
      runtime.Apply(probes[static_cast<std::size_t>(i)]);
      applied.push_back(probes[static_cast<std::size_t>(i)]);
      harness::RequireSharedMatchesBase(connection, runtime, parallel);
    }
  } catch (...) {
    for (auto it = applied.rbegin(); it != applied.rend(); ++it) {
      (void)connection.Query(it->restore_sql);
    }
    throw;
  }
  for (auto it = applied.rbegin(); it != applied.rend(); ++it) {
    auto restore = connection.Query(it->restore_sql);
    Require(restore && !restore->HasError(),
            restore ? restore->GetError() : "sequence restore failed");
  }
  runtime.Build();
  harness::RequireSharedMatchesBase(connection, runtime, parallel);
}

void RunFileWriteSuite(const std::string &source) {
  const int n = HardwareThreads();
  const int thread_counts[] = {1, n};
  for (int query_id : kSecQueries) {
    const auto &parallel =
        catalog::LookupSharedQueryParallel(catalog::SharedWorkloadKind::SecEdgar, query_id);
    for (int threads : thread_counts) {
      const auto work = WorkCopyPath(query_id, threads, "write");
      CopyDb(source, work);
      std::cerr << "file write differential Q" << query_id << " t=" << threads
                << " work=" << work << '\n';
      RunCopiedWriteDifferential(work.string(), parallel, threads);
      std::filesystem::remove(work);
    }
  }
}

} // namespace

int main() {
  try {
    RunCatalogChecks();
    RunUnitFallbackMerge();
    RunSyntheticSuite();
    RunLargeSyntheticSuite();
    const auto file_db = FileDbPath();
    if (!file_db.empty()) {
      RunFileReadOnlySuite(file_db);
      RunFileWriteSuite(file_db);
    } else {
      std::cerr << "file suite skipped (no sec_edgar_test.duckdb / ACCEL_PARALLEL_AUX_DB)\n";
    }
    std::cout << "parallel SEC EDGAR aux correctness OK\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "parallel SEC EDGAR aux test failed: " << error.what() << '\n';
    return 1;
  }
}
