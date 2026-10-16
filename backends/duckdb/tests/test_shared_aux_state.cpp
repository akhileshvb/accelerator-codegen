#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <memory>
#include <string>

#include "substrate/aux/core/aux_types.hpp"
#include "substrate/aux/core/aux_txn_buffer.hpp"
#include "eval/tests/test_support/module_runtime.hpp"
#include "duckdb/main/connection.hpp"

using namespace accelerator_codegen::aux_state::core;

namespace {

class TestModule final : public AuxQueryModule {
public:
  AcceleratorId Id() const override { return "test"; }

  std::vector<TableDependency> Dependencies() const override {
    return {{7, {1}}};
  }

  std::unique_ptr<AuxTxnBuffer> CreateTxnBuffer() const override {
    return std::make_unique<BasicAuxTxnBuffer>();
  }

  void Build(AuxEngineServices &, StateBuilder &builder) override {
    builder.Finish();
  }

  InvalidationPlan InterpretMutation(const MutationBatch &) const override {
    return {};
  }

  void AccumulateMutation(const MutationBatch &, AuxEngineServices &,
                          AuxTxnBuffer &buffer) override {
    static_cast<BasicAuxTxnBuffer &>(buffer).MarkPending();
  }

  StagedMaintenanceOutcome StageMaintenance(AuxEngineServices &services,
                                            const InvalidationPlan &,
                                            AuxTxnBuffer &) override {
    StagedMaintenanceOutcome outcome;
    outcome.action = MaintenanceAction::ApplyDelta;
    outcome.expected_version = 0;
    StateImage image;
    image.accelerator = Id();
    image.partition = 0;
    image.version = 1;
    image.payload = std::make_shared<const std::vector<uint8_t>>(
        std::initializer_list<uint8_t>{'o', 'k'});
    outcome.updates.push_back(std::move(image));
    ++stage_count;
    assert(services.CurrentVisibility().mutation_epoch == 0);
    return outcome;
  }

  bool CheckValidity(AuxEngineServices &,
                     const QueryArguments &) const override {
    return true;
  }

  void Execute(AuxEngineServices &, const QueryArguments &,
               AuxRowSink &) override {}

  void ExecuteFallback(AuxEngineServices &, const QueryArguments &,
                       AuxRowSink &) override {}

  int stage_count = 0;
};

} // namespace

int main() {
  // The DuckDB runtime: the module maintains after the SQL commit (never for
  // a rollback) and the published state reads back.
  duckdb::DuckDB db(nullptr);
  duckdb::Connection connection(db);
  connection.Query("CREATE TABLE t (a BIGINT)");
  auto duckdb_module = std::make_shared<TestModule>();
  accelerator_codegen::aux_state::test::ModuleRuntime runtime(connection, duckdb_module);
  runtime.Build();
  MutationBatch row_insert;
  row_insert.table = 7;
  row_insert.kind = MutationKind::Insert;
  row_insert.new_rows = {AuxRow{{AuxValue::Of(AuxType::Int64, int64_t(1))}}};

  connection.BeginTransaction();
  connection.Query("INSERT INTO t VALUES (1)");
  connection.Rollback();
  assert(duckdb_module->stage_count == 0);

  connection.BeginTransaction();
  connection.Query("INSERT INTO t VALUES (1)");
  assert(duckdb_module->stage_count == 0);
  connection.Commit();
  runtime.Commit({row_insert});
  assert(duckdb_module->stage_count == 1);
  auto &duckdb_services = runtime.Backend();
  const auto published =
      duckdb_services.StateStore().Read(duckdb_services.CurrentVisibility());
  assert(published.partitions.size() == 1);
  assert(*static_cast<const std::vector<uint8_t> *>(
             published.partitions[0].payload.get()) ==
         std::vector<uint8_t>({'o', 'k'}));

  return 0;
}
