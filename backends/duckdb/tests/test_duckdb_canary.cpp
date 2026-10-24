// The canary suite (pipeline/canary, pipeline/prompts/engine_backend/canary_contract.md) bound to
// DuckDB: sessions are connections of one database, canary accelerators register through the
// backend's aux registry and catalog, counters are the runtime's statistics, and the build scan
// forwards ScanPartitioned and its row view as they are.
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <memory>
#include <unistd.h>

#include "backends/duckdb/api/aux_query_registry.hpp"
#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "backends/duckdb/api/register.hpp"
#include "backends/duckdb/api/sql_fallback.hpp"
#include "pipeline/canary/canary_main.hpp"

namespace {

namespace canary = accelerator_codegen::canary;
namespace engine = accelerator_codegen::aux_state::engine;
namespace core = accelerator_codegen::aux_state::core;
namespace runtime = accelerator_codegen::aux_state::runtime;
namespace scan = accelerator_codegen::aux_state::engine::duckdb::scan;
using DuckdbRuntime = engine::duckdb::DuckdbRuntime;

class DuckSession final : public canary::Session {
public:
  explicit DuckSession(duckdb::DuckDB &db) : connection_(db) {}

  canary::Rows Query(const std::string &sql) override {
    auto result = connection_.Query(sql);
    if (!result || result->HasError()) {
      throw std::runtime_error(result ? result->GetError() : "null result");
    }
    canary::Rows rows;
    for (duckdb::idx_t r = 0; r < result->RowCount(); ++r) {
      canary::Row row;
      for (duckdb::idx_t c = 0; c < result->ColumnCount(); ++c) {
        const auto value = result->GetValue(c, r);
        if (value.IsNull()) {
          row.emplace_back(std::monostate{});
        } else if (result->types[c].id() == duckdb::LogicalTypeId::VARCHAR) {
          row.emplace_back(value.GetValue<std::string>());
        } else {
          row.emplace_back(value.GetValue<int64_t>());
        }
      }
      rows.push_back(std::move(row));
    }
    return rows;
  }

private:
  duckdb::Connection connection_;
};

class DuckBatch final : public canary::ScanBatch {
public:
  explicit DuckBatch(const scan::ChunkBatch &batch) : batch_(batch) {}
  std::size_t size() const override { return batch_.size(); }
  int64_t Int(std::size_t row, int column) const override { return batch_.Row(row).Int(column); }
  std::string_view TextView(std::size_t row, int column) const override { return batch_.Row(row).TextView(column); }
  bool IsNull(std::size_t row, int column) const override { return batch_.Row(row).IsNull(column); }

private:
  const scan::ChunkBatch &batch_;
};

// The module bound to the backend: its build and reads run inside the scan-services scope.
struct CanaryBinding final : DuckdbRuntime::Binding {
  CanaryBinding(std::shared_ptr<core::AuxQueryModule> module_p, engine::duckdb::CaptureTableList tables_p)
      : module(std::move(module_p)), tables(std::move(tables_p)) {}
  core::AuxQueryModule &Module() override { return *module; }
  void Build(engine::duckdb::DuckDBAuxBackend &backend, core::StateBuilder &builder) override {
    const scan::ScanServicesScope scans(backend);
    module->Build(backend, builder);
  }
  void Execute(engine::duckdb::DuckDBAuxBackend &backend, const core::QueryArguments &arguments,
               core::AuxRowSink &sink) override {
    const scan::ScanServicesScope scans(backend);
    module->Execute(backend, arguments, sink);
  }
  engine::duckdb::CaptureTableList CaptureTables() const override { return tables; }
  std::shared_ptr<core::AuxQueryModule> module;
  engine::duckdb::CaptureTableList tables;
};

class DuckEngine final : public canary::Engine {
public:
  DuckEngine() {
    const char *tmp = std::getenv("TMPDIR");
    std::string pattern = std::string(tmp && *tmp ? tmp : "/tmp") + "/canary_duckdb_XXXXXX";
    if (!mkdtemp(pattern.data())) {
      throw std::runtime_error("mkdtemp failed");
    }
    dir_ = pattern;
    db_ = std::make_unique<duckdb::DuckDB>(dir_ + "/canary.duckdb");
    admin_ = std::make_unique<duckdb::Connection>(*db_);
  }
  ~DuckEngine() override {
    runtimes_.clear();
    admin_.reset();
    db_.reset();
    std::error_code ignored;
    std::filesystem::remove_all(dir_, ignored);
  }

  std::unique_ptr<canary::Session> Connect() override { return std::make_unique<DuckSession>(*db_); }

  void Register(const canary::AcceleratorSpec &spec) override {
    keep_.push_back(spec.name);
    const char *id = keep_.back().c_str();
    keep_.push_back("SELECT * FROM accel_canary_" + spec.name + "()");
    const char *aux_sql = keep_.back().c_str();
    keep_.push_back(spec.sql);
    const char *sql = keep_.back().c_str();
    engine::SharedQueryCatalogEntry entry;
    entry.query_id = 9000 + static_cast<int>(runtimes_.size());
    entry.workload = engine::SharedWorkloadKind::Tpch;
    entry.workload_name = "canary";
    entry.accelerator_id = id;
    entry.base_sql = sql;
    entry.duckdb_aux_sql = aux_sql;
    entry.fallback_sql = sql;
    engine::RegisterSharedQuery(entry);

    const auto scan_fn = [](const core::AuxEngineServices &services, const std::string &table,
                            const std::vector<canary::CanaryColumn> &columns, std::size_t partitions,
                            const canary::BatchSink &sink, ::duckdb::Connection &connection) {
      std::vector<scan::BuildColumn> specs;
      for (const auto &column : columns) {
        scan::BuildColumn spec{column.name, column.is_text ? scan::BuildColumn::Kind::Text : scan::BuildColumn::Kind::Int};
        spec.nullable = column.nullable;
        specs.push_back(spec);
      }
      scan::ScanPartitioned(services, connection, table, specs, partitions,
                            [&](std::size_t partition, const scan::ChunkBatch &batch) { sink(partition, DuckBatch(batch)); });
    };
    engine::duckdb::AuxQuery query;
    query.accelerator_id = spec.name;
    query.table_function = "accel_canary_" + spec.name;
    query.make_binding = [spec, scan_fn](::duckdb::Connection &connection) {
      engine::duckdb::CaptureTableList tables;
      for (std::size_t i = 0; i < spec.tables.size(); ++i) {
        tables.emplace_back(spec.tables[i], static_cast<core::TableId>(i));
      }
      auto module = spec.make_module([scan_fn, &connection](const core::AuxEngineServices &services, const std::string &table,
                                                            const std::vector<canary::CanaryColumn> &columns,
                                                            std::size_t partitions, const canary::BatchSink &sink) {
        scan_fn(services, table, columns, partitions, sink, connection);
      });
      return std::unique_ptr<DuckdbRuntime::Binding>(new CanaryBinding(std::move(module), std::move(tables)));
    };
    engine::duckdb::RegisterAuxQuery(std::move(query));
    accelerator_codegen::RegisterAcceleratorFunctions(*admin_);
    runtimes_[spec.name] = std::make_unique<DuckdbRuntime>(*admin_, *engine::FindSharedQueryByAccelerator(spec.name), 10.0);
  }

  std::string ReadSql(const std::string &name) override { return "SELECT * FROM accel_canary_" + name + "()"; }

  canary::Counters ReadCounters(const std::string &name) override {
    const auto stats = runtimes_.at(name)->Stats();
    return {stats.reads_hit, stats.reads_fallback, stats.builds, stats.commits_stale};
  }

  void Invalidate(const std::string &name) override { runtimes_.at(name)->InvalidateAll(); }
  void ForceFallback(bool forced) override { runtime::SetForcedFallback(forced); }
  void TrapFallback(bool trapped) override { engine::SetFallbackTrap(trapped); }
  void Settle() override { admin_->Query("CHECKPOINT"); }

private:
  std::string dir_;
  std::unique_ptr<duckdb::DuckDB> db_;
  std::unique_ptr<duckdb::Connection> admin_;
  std::deque<std::string> keep_;
  std::map<std::string, std::unique_ptr<DuckdbRuntime>> runtimes_;
};

} // namespace

int main(int argc, char **argv) {
  return canary::Main([] { return std::make_unique<DuckEngine>(); }, argc, argv);
}
