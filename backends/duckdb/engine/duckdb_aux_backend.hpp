// GENERATED BACKEND
#pragma once

#include <functional>

#include "substrate/aux/runtime/accelerator_runtime.hpp"
#include "duckdb/main/client_context.hpp"

namespace accelerator_codegen::aux_state::engine::duckdb {

// The services a DuckDB module sees: its accelerator's runtime, plus the
// connection its scans run on (or, inside a ReaderScope, the reading
// connection, whose context also runs intra-query workers).
class DuckDBAuxBackend final : public core::AuxEngineServices {
public:
  DuckDBAuxBackend(runtime::AcceleratorRuntime &runtime, ::duckdb::ClientContext &context);

  core::VisibilityToken CurrentVisibility() const override;
  core::AuxStateStore &StateStore() override;
  const maintenance::CostModelParams &MaintenanceCostModel() const override;
  double ExpectedQueriesBeforeNextWrite() const override;
  uint64_t DataScaleRows() const override;
  std::size_t WorkerCount() const override;
  void ParallelFor(std::size_t n, const std::function<void(std::size_t)> &fn) const override;
  bool AnswerFromSql(const core::QueryArguments &arguments, core::AuxRowSink &sink) override;

  // Runs the query's SQL for AnswerFromSql; false if it has none.
  using SqlAnswer = std::function<bool(const core::QueryArguments &, core::AuxRowSink &)>;
  void SetSqlAnswer(SqlAnswer answer) { sql_answer = std::move(answer); }

  ::duckdb::ClientContext *ExecutionContext() const;
  runtime::AcceleratorRuntime &Runtime() { return runtime; }

  // Marks the current thread as reading for `context` (an aux table function
  // running in another connection).
  class ReaderScope {
  public:
    explicit ReaderScope(::duckdb::ClientContext &context);
    ~ReaderScope();
    ReaderScope(const ReaderScope &) = delete;
    ReaderScope &operator=(const ReaderScope &) = delete;

  private:
    ::duckdb::ClientContext *previous;
  };
  static ::duckdb::ClientContext *CurrentReader();

  // The snapshot epoch of `context`'s transaction on its default database:
  // its start time. A commit with id c is in that snapshot iff c < start
  // time, so state published at epoch c + 1 is visible to it exactly then.
  // 0 if there is no DuckDB transaction.
  static core::Version SnapshotEpoch(::duckdb::ClientContext &context);

private:
  runtime::AcceleratorRuntime &runtime;
  ::duckdb::ClientContext &context;
  SqlAnswer sql_answer;
};

} // namespace accelerator_codegen::aux_state::engine::duckdb
