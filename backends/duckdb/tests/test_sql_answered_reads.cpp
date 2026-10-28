// A read the state could serve that the module answers from the engine's SQL anyway (its Execute calls
// ExecuteFallback) is a fallback, not a hit: the runtime's counters, which the bench's hit rate reads, say so.
#include <iostream>
#include <stdexcept>
#include <string>

#include "backends/duckdb/api/declining_query.hpp"
#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "backends/duckdb/api/register.hpp"
#include "duckdb.hpp"

namespace engine = accelerator_codegen::aux_state::engine;
namespace core = accelerator_codegen::aux_state::core;
namespace detail = engine::duckdb::declining_detail;

namespace {

void Require(bool condition, const std::string &what) {
  if (!condition) throw std::runtime_error(what);
}

// Builds a valid (empty) state, then answers every read from SQL.
class SqlAnsweringModule final : public core::AuxQueryModule {
public:
  core::AcceleratorId Id() const override { return "sql_answering_q1_aux"; }
  std::vector<core::TableDependency> Dependencies() const override { return {core::TableDependency{}}; }
  std::unique_ptr<core::AuxTxnBuffer> CreateTxnBuffer() const override { return std::make_unique<detail::NoTxnBuffer>(); }
  void Build(core::AuxEngineServices &, core::StateBuilder &builder) override { builder.Finish(); }
  core::InvalidationPlan InterpretMutation(const core::MutationBatch &) const override { return {}; }
  void AccumulateMutation(const core::MutationBatch &, core::AuxEngineServices &, core::AuxTxnBuffer &) override {}
  core::StagedMaintenanceOutcome StageMaintenance(core::AuxEngineServices &, const core::InvalidationPlan &,
                                                  core::AuxTxnBuffer &) override {
    return {};
  }
  bool CheckValidity(core::AuxEngineServices &, const core::QueryArguments &) const override { return true; }
  void Execute(core::AuxEngineServices &services, const core::QueryArguments &arguments,
               core::AuxRowSink &sink) override {
    ExecuteFallback(services, arguments, sink);
  }
};

struct SqlAnsweringBinding final : engine::duckdb::DuckdbRuntime::Binding {
  core::AuxQueryModule &Module() override { return module; }
  void Build(engine::duckdb::DuckDBAuxBackend &backend, core::StateBuilder &builder) override {
    module.Build(backend, builder);
  }
  void Execute(engine::duckdb::DuckDBAuxBackend &backend, const core::QueryArguments &arguments,
               core::AuxRowSink &sink) override {
    module.Execute(backend, arguments, sink);
  }
  engine::duckdb::CaptureTableList CaptureTables() const override { return {{"t", core::TableId{0}}}; }
  SqlAnsweringModule module;
};

void Register() {
  engine::SharedQueryCatalogEntry entry;
  entry.query_id = 1;
  entry.workload_name = "sql_answering";
  entry.accelerator_id = "sql_answering_q1_aux";
  entry.base_sql = "SELECT count(*) AS n FROM t";
  entry.fallback_sql = entry.base_sql;
  entry.duckdb_aux_sql = "SELECT * FROM accel_sql_answering_q1_aux()";
  engine::RegisterSharedQuery(entry);
  engine::duckdb::AuxQuery aux;
  aux.accelerator_id = entry.accelerator_id;
  aux.table_function = "accel_sql_answering_q1_aux";
  aux.make_binding = [](::duckdb::Connection &) {
    return std::unique_ptr<engine::duckdb::DuckdbRuntime::Binding>(new SqlAnsweringBinding());
  };
  engine::duckdb::RegisterAuxQuery(std::move(aux));
}

} // namespace

int main() {
  try {
    Register();
    ::duckdb::DuckDB db(nullptr);
    ::duckdb::Connection connection(db);
    Require(!connection.Query("CREATE TABLE t AS SELECT range AS a FROM range(10)")->HasError(), "create t");
    accelerator_codegen::RegisterAcceleratorFunctions(connection);
    const auto &entry = engine::LookupSharedQuery("sql_answering", 1);
    engine::duckdb::DuckdbRuntime accelerator(connection, entry, 10.0);
    accelerator.Build();
    const auto before = accelerator.Stats();
    auto result = connection.Query(entry.duckdb_aux_sql);
    Require(result && !result->HasError(), "read failed: " + (result ? result->GetError() : std::string("null")));
    Require(result->GetValue(0, 0).GetValue<int64_t>() == 10, "the read did not answer from the SQL");
    const auto after = accelerator.Stats();
    Require(after.reads_hit == before.reads_hit && after.reads_fallback == before.reads_fallback + 1,
            "a servable read answered from SQL counted as hits " + std::to_string(after.reads_hit - before.reads_hit) +
                ", fallbacks " + std::to_string(after.reads_fallback - before.reads_fallback));
    std::cout << "a read answered from SQL is a fallback\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "sql-answered reads test failed: " << error.what() << "\n";
    return 1;
  }
}
