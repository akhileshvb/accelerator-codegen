// The backend-generic slot gates (eval/harness/slot_gates.hpp) on DuckDB: the
// workload's accelerators built on one database, every client a plain
// connection of it.
//
// Usage: test_slot_gates_duckdb [scenario|all] [seconds=4] [seed=1] [readers=4]
// Env:   ACCEL_TPCH_DB (copied into a scratch directory), ACCEL_SLOT_GATE_QUERIES
//        (TPC-H queries under test, default 1,6,14: Q14 numbers lineitem
//        differently from Q1 and Q6, so routing by host table id is checked)
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>

#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "backends/duckdb/api/register.hpp"
#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"
#include "eval/harness/slot_gates.hpp"
#include "eval/tests/test_support/db_paths.hpp"
#include "eval/tests/tpch_fuzz_writes.hpp"

namespace {

namespace gates = accelerator_codegen::slot_gates;
namespace ts = accelerator_codegen::test_support;
namespace engine = accelerator_codegen::aux_state::engine;
using engine::duckdb::DuckdbRuntime;

std::unique_ptr<duckdb::MaterializedQueryResult> Run(duckdb::Connection &c, const std::string &sql) {
  auto r = c.Query(sql);
  if (!r || r->HasError()) {
    throw std::runtime_error(sql.substr(0, 200) + ": " + (r ? r->GetError() : std::string("null result")));
  }
  return r;
}

// Numbers at two decimals (the accelerators' fixed point and DECIMAL agree
// there), the rest as text; rows sorted.
gates::Rows Canonical(duckdb::MaterializedQueryResult &result) {
  gates::Rows rows;
  for (duckdb::idx_t i = 0; i < result.RowCount(); ++i) {
    std::string row;
    for (duckdb::idx_t j = 0; j < result.ColumnCount(); ++j) {
      const auto v = result.GetValue(j, i);
      row += j ? "|" : "";
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

class DuckdbSession final : public gates::Session {
public:
  explicit DuckdbSession(duckdb::DuckDB &db) : connection(db) {
    // Registering creates catalog entries: concurrent registrations conflict.
    static std::mutex registration;
    std::lock_guard<std::mutex> lock(registration);
    accelerator_codegen::RegisterAcceleratorFunctions(connection);
  }
  void Exec(const std::string &sql) override { Run(connection, sql); }
  gates::Rows Query(const std::string &sql) override { return Canonical(*Run(connection, sql)); }
  std::function<gates::Rows()> Prepare(const std::string &sql) override {
    std::shared_ptr<duckdb::PreparedStatement> statement = connection.Prepare(sql);
    if (statement->HasError()) {
      throw std::runtime_error(sql.substr(0, 200) + ": " + statement->GetError());
    }
    return [statement] {
      duckdb::vector<duckdb::Value> no_values;
      auto result = statement->Execute(no_values, /*allow_stream_result=*/false);
      if (result->HasError()) {
        throw std::runtime_error("prepared statement: " + result->GetError());
      }
      return Canonical(result->Cast<duckdb::MaterializedQueryResult>());
    };
  }

private:
  duckdb::Connection connection;
};

class TpchWrites final : public gates::WriteSource {
public:
  TpchWrites(duckdb::DuckDB &db, uint64_t seed)
      : connection(db), fuzz(ts::FuzzDialect::DuckDB, seed, [this](const std::string &sql) {
          auto r = Run(connection, sql);
          auto v = r->RowCount() ? r->GetValue(0, 0) : duckdb::Value();
          return v.IsNull() ? int64_t{0} : v.GetValue<int64_t>();
        }) {}

  // Writes on lineitem, the table the workload's queries read.
  std::vector<std::string> Next() override {
    const auto sql = fuzz.Next({1, 0, 0, 0, 0, 0, 0, 0});
    std::vector<std::string> statements;
    for (std::size_t pos = 0;;) {
      const auto end = sql.find(";\n", pos);
      statements.push_back(sql.substr(pos, end == std::string::npos ? end : end - pos));
      if (end == std::string::npos) {
        return statements;
      }
      pos = end + 2;
    }
  }

private:
  duckdb::Connection connection;
  ts::TpchFuzzWrites fuzz;
};

class DuckdbEngine final : public gates::Engine {
public:
  DuckdbEngine(const std::string &db_path, const std::vector<int> &queries) : db(db_path), setup(db) {
    for (int q : queries) {
      const auto &entry = engine::LookupSharedQuery("tpch", q);
      auto runtime = std::make_unique<DuckdbRuntime>(setup, entry, 10.0);
      runtime->Build();
      cases.push_back({"Q" + std::to_string(q), entry.duckdb_aux_sql, entry.base_sql});
      runtimes.push_back(std::move(runtime));
    }
  }

  std::string Name() const override { return "duckdb"; }
  std::unique_ptr<gates::Session> Connect() override { return std::make_unique<DuckdbSession>(db); }
  std::vector<gates::ReadCase> ReadCases() override { return cases; }
  std::vector<gates::ReaderMode> ReaderModes() override { return {{"snapshot", "BEGIN TRANSACTION", true}}; }
  std::unique_ptr<gates::WriteSource> NewWriteSource(uint64_t seed) override {
    return std::make_unique<TpchWrites>(db, seed);
  }
  gates::ReadStats Stats() override {
    gates::ReadStats total;
    for (const auto &runtime : runtimes) {
      const auto s = runtime->Stats();
      total.hits += s.reads_hit;
      total.fallbacks += s.reads_fallback;
      total.builds += s.builds;
    }
    return total;
  }
  std::vector<std::string> TruncateAndRestore() override {
    return {"CREATE TEMP TABLE slot_gate_copy AS SELECT * FROM lineitem", "TRUNCATE lineitem",
            "INSERT INTO lineitem SELECT * FROM slot_gate_copy", "DROP TABLE slot_gate_copy"};
  }
  // lineitem replaced by a subset of its rows (the queries must follow the new table), then restored.
  std::vector<gates::DdlCase> DdlCases() override {
    const std::string subset = " AS SELECT * FROM slot_gate_orig WHERE l_orderkey % 5 <> 0";
    return {{"DROP + CREATE TABLE AS + RENAME",
             {"CREATE TABLE slot_gate_orig AS SELECT * FROM lineitem", "CREATE TABLE slot_gate_new" + subset,
              "DROP TABLE lineitem", "ALTER TABLE slot_gate_new RENAME TO lineitem"},
             {"DROP TABLE lineitem", "ALTER TABLE slot_gate_orig RENAME TO lineitem"}},
            {"RENAME aside + CREATE TABLE AS",
             {"ALTER TABLE lineitem RENAME TO slot_gate_orig", "CREATE TABLE lineitem" + subset},
             {"DROP TABLE lineitem", "ALTER TABLE slot_gate_orig RENAME TO lineitem"}}};
  }
  bool SupportsSavepoints() const override { return false; }
  bool RestartAccelerators() override { return false; } // in-process: nothing separate to restart

private:
  duckdb::DuckDB db;
  duckdb::Connection setup;
  std::vector<std::unique_ptr<DuckdbRuntime>> runtimes;
  std::vector<gates::ReadCase> cases;
};

} // namespace

int main(int argc, char **argv) {
  try {
    const std::string scenario = argc > 1 ? argv[1] : "all";
    gates::Options options;
    options.seconds = argc > 2 ? std::atoi(argv[2]) : options.seconds;
    options.seed = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : options.seed;
    options.readers = argc > 4 ? std::atoi(argv[4]) : options.readers;
    std::vector<int> queries;
    std::stringstream list(ts::EnvOr("ACCEL_SLOT_GATE_QUERIES", "1,6,14"));
    for (std::string item; std::getline(list, item, ',');) {
      queries.push_back(std::stoi(item));
    }
    ts::ScratchDir scratch("slot_gates_duckdb");
    DuckdbEngine duckdb_engine(ts::PrivateDbCopy(ts::TpchDbPath(), scratch), queries);
    return gates::Run(duckdb_engine, scenario, options);
  } catch (const std::exception &e) {
    std::cerr << "test_slot_gates_duckdb failed: " << e.what() << std::endl;
    return 1;
  }
}
