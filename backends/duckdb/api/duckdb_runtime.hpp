// GENERATED BACKEND
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "substrate/aux/core/aux_calibration.hpp"
#include "substrate/aux/core/aux_maintenance_decision_log.hpp"
#include "substrate/aux/core/aux_store_diagnostics.hpp"
#include "backends/duckdb/api/duckdb_aux_backend.hpp"
#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"

namespace accelerator_codegen::aux_state::engine::duckdb {

class DuckdbHost;
struct DuckdbAccelerator;

// One accelerator as a client of its database's DuckdbHost: builds, reads,
// and writes on `connection` (captured for every accelerator of the database,
// whatever connection writes). The state lives in the host, shared by every
// DuckdbRuntime of the accelerator on the database.
class DuckdbRuntime {
public:
  struct Binding {
    virtual ~Binding() = default;
    virtual core::AuxQueryModule &Module() = 0;
    virtual void Build(DuckDBAuxBackend &backend, core::StateBuilder &builder) = 0;
    virtual void Execute(DuckDBAuxBackend &backend, const core::QueryArguments &arguments,
                         core::AuxRowSink &sink) = 0;
    // (DuckDB table name, the module's TableId for it), for engine capture.
    virtual std::vector<std::pair<std::string, core::TableId>> CaptureTables() const {
      return {};
    }
  };

  DuckdbRuntime(::duckdb::Connection &connection, const SharedQueryCatalogEntry &entry,
                double expected_queries_before_write);
  ~DuckdbRuntime();

  // The runtime aux table functions in any connection read through
  // (registered by Build(true)).
  static DuckdbRuntime *FindActive(::duckdb::DatabaseInstance &db,
                                   const std::string &accelerator_id);

  // Builds the state afresh (synchronously). `register_active`: make this the
  // runtime other connections' aux queries use (FindActive).
  void Build(bool register_active = true);
  // Builds only if the state is not synced (shared by every runtime of the
  // accelerator on the database).
  void EnsureBuilt();
  void Execute();
  void Execute(const core::QueryArguments &arguments);
  std::vector<core::AuxRow> ExecuteCollect();
  std::vector<core::AuxRow> ExecuteCollect(const core::QueryArguments &arguments);
  // The same read, rows streamed into `sink` (the SQL table function's path).
  void ExecuteInto(const core::QueryArguments &arguments, core::AuxRowSink &sink);

  // ---- writes on this runtime's connection ----
  void Apply(SharedProbe &probe);
  void ApplyBatch(std::vector<SharedProbe> &probes);
  // One write transaction running `sql`; `mutation` describes it for tables
  // the engine does not capture.
  void ApplyBatchWrite(const std::string &sql, const core::MutationBatch &mutation);
  void BeginWrite();
  void CommitWrite();
  void RollbackWrite();
  // One write transaction running `sql`; returns the rows its INSERT /
  // UPDATE / DELETE statements touched.
  uint64_t ApplyWrite(const std::string &sql);
  // One transaction running `sql` on the first runtime's connection, and
  // where its commit's time went per runtime (all on one database).
  struct WriteAllTiming {
    double statement_us = 0;
    std::vector<double> pre_commit_us; // each runtime's maintenance of this commit
    std::vector<std::string> action;   // each runtime's maintenance action
    std::vector<core::MaintenanceDecisionLogEntry> decision;
    double engine_commit_us = 0;       // the commit, maintenance included
    std::vector<double> commit_us;     // kept for report compatibility (0)
  };
  static void ApplyWriteAll(const std::vector<DuckdbRuntime *> &runtimes, const std::string &sql,
                            WriteAllTiming *timing = nullptr);

  void SetExpectedQueriesBeforeNextWrite(double expected_queries_before_write);
  double ExpectedQueriesBeforeNextWrite() const;
  // Rows in the accelerator's dependency tables now.
  uint64_t DataScaleRows() const;
  std::vector<core::CalibrationWrite> CalibrationWrites() const;

  // ---- calibration and tests (no SQL write) ----
  void ResetCalibrationState();
  void InvalidateAll();
  // Maintains `mutation` as a commit visible to every snapshot.
  void ApplyMutationBatch(const core::MutationBatch &mutation);
  // The database's changes reach accelerators only through
  // ApplyMutationBatch (tests that hand-build them while writing SQL).
  void UseManualCapture();
  void ExecuteBaseQuery();
  void ExecuteBaseQuery(const core::QueryArguments &arguments);

  // ---- diagnostics ----
  bool UsesVerusStore() const;
  std::size_t StateVersionCount() const;
  // The Verus store's measured heap (bytes live, all versions; peak since ResetHeapPeak);
  // 0 for a store that is not Verus.
  std::size_t StateHeapBytes() const;
  std::size_t StateHeapPeakBytes() const;
  void ResetHeapPeak() const;
  std::size_t StaleInstallCount() const;
  core::AuxStoreDiagnosticsSnapshot StoreDiagnosticsSnapshot() const;
  void ResetStoreDiagnosticsProfiling();
  uint64_t AuxStateFootprintBytes() const;
  runtime::AcceleratorStats Stats() const;

  std::atomic<uint64_t> last_rows{0};
  // Reads answered by SQL instead of the state (scans redirected to the
  // reader's snapshot).
  std::atomic<uint64_t> reader_fallbacks{0};

private:
  template <typename Sink> void Read(const core::QueryArguments &arguments, Sink &sink);
  runtime::ColumnarBatchRef ToBatch(const core::MutationBatch &mutation) const;
  // Hands `mutation` to the host with the open transaction when the engine
  // does not capture its table.
  void CaptureByHand(const core::MutationBatch &mutation);

  ::duckdb::Connection &connection_;
  const SharedQueryCatalogEntry &entry_;
  DuckdbHost &host_;
  DuckdbAccelerator &accelerator_;
  // Reads the state could serve that the module answered from SQL anyway (its Execute fell back): the runtime
  // counted them as hits; Stats() reports them as the fallbacks they were.
  std::atomic<uint64_t> servable_reads_answered_from_sql_{0};
};

// The DuckDB side of core::CalibrateFromWrites: a captured write on the
// runtime's connection, maintenance forced or chosen, then one accelerator
// query, timed together.
class DuckdbCalibrationEngine final : public core::CalibrationEngine {
public:
  DuckdbCalibrationEngine(DuckdbRuntime &runtime_p, core::QueryArguments arguments_p)
      : runtime(runtime_p), arguments(std::move(arguments_p)) {}
  Timed WriteThenRead(const std::string &sql,
                      std::optional<maintenance::MaintenanceAction> forced) override;

private:
  DuckdbRuntime &runtime;
  core::QueryArguments arguments;
};

} // namespace accelerator_codegen::aux_state::engine::duckdb
