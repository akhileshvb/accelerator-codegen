// GENERATED BACKEND
#pragma once

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
  bool AllowsSynchronousRebuildOnCommit() const override { return false; }
  bool ReadersCanBuildState() const override { return false; }
  std::size_t WorkerCount() const override;

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
};

} // namespace accelerator_codegen::aux_state::engine::duckdb
