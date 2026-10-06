// GENERATED BACKEND
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_calibration.hpp"
#include "accelerator_codegen/stateful/core/aux_store_diagnostics.hpp"
#include "accelerator_codegen/stateful/core/aux_registry.hpp"
#include "accelerator_codegen/stateful/core/aux_txn_buffer.hpp"
#include "accelerator_codegen/stateful/duckdb/duckdb_aux_backend.hpp"
#include "accelerator_codegen/stateful/duckdb/duckdb_state_store.hpp"
#include "accelerator_codegen/stateful/state/verus_aux_store.hpp"
#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"

namespace accelerator_codegen::aux_state::engine::duckdb {

class DuckdbRuntime {
public:
  struct Binding {
    virtual ~Binding() = default;
    virtual core::AuxQueryModule &Module() = 0;
    virtual void Build(DuckDBAuxBackend &backend,
                       core::StateBuilder &builder) = 0;
    virtual void Execute(DuckDBAuxBackend &backend,
                         const core::QueryArguments &arguments,
                         core::AuxRowSink &sink) = 0;
    // (DuckDB table name, the module's TableId for it), for engine capture.
    virtual std::vector<std::pair<std::string, core::TableId>>
    CaptureTables() const {
      return {};
    }
  };

  DuckdbRuntime(::duckdb::Connection &connection,
                const SharedQueryCatalogEntry &entry,
                double expected_queries_before_write);
  ~DuckdbRuntime();

  // Active session runtimes for SQL table-function hits. Build() registers;
  // destructor unregisters. Aux TF looks up by (DatabaseInstance, accelerator_id)
  // so SELECT * FROM accel_*_aux(...) reuses state instead of rebuilding.
  static DuckdbRuntime *FindActive(::duckdb::DatabaseInstance &db,
                                   const std::string &accelerator_id);

  // Build the state. `register_active`: make this runtime the one other
  // connections' aux queries use (FindActive). A short-lived runtime must
  // not register: readers in other connections would use it after it is
  // destroyed.
  void Build(bool register_active = true);
  void Execute();
  void Execute(const core::QueryArguments &arguments);
  std::vector<core::AuxRow> ExecuteCollect();
  std::vector<core::AuxRow> ExecuteCollect(const core::QueryArguments &arguments);
  // The same read, rows streamed into `sink` as the module writes them (no
  // intermediate row vector).  The SQL table function uses it for C++ stores.
  void ExecuteInto(const core::QueryArguments &arguments, core::AuxRowSink &sink);
  void Apply(SharedProbe &probe);

  // Apply multiple point probes in one transaction so StageMaintenance sees
  // bulk row fanout (used by phased cost-model experiments).
  void ApplyBatch(std::vector<SharedProbe> &probes);

  // Mini-chunk TPC-H RF1/RF2 (orders+lineitem insert/delete). Alternating
  // chunks keep DBs from unbounded growth while exercising I/D maintenance.
  // Point-update Apply(SharedProbe) remains the default sweep write path.
  void ApplyRfChunk(int write_seq, int order_count = 1);

  void SetExpectedQueriesBeforeNextWrite(double expected_queries_before_write);
  double ExpectedQueriesBeforeNextWrite() const;

  // Calibration helpers (no SQL mutation; exercises coordinator + state store).
  void ResetCalibrationState();
  void InvalidateAll();
  void ApplyMutationBatch(const core::MutationBatch &mutation);
  void ExecuteBaseQuery();

  // Whether the accelerator's state lives in its Verus store.
  bool UsesVerusStore() const { return verus_store_ != nullptr; }
  // Versions kept by the active store, and (Verus store) stale installs.
  std::size_t StateVersionCount() const;
  std::size_t StaleInstallCount() const;

  core::AuxStoreDiagnosticsSnapshot StoreDiagnosticsSnapshot() const;
  void ResetStoreDiagnosticsProfiling();
  uint64_t AuxStateFootprintBytes() const;

  // Written by concurrent readers (aux table functions in other connections).
  std::atomic<uint64_t> last_rows{0};
  // Reader queries (other connections) that ran SQL instead of reading the
  // state. Should stay 0: commits publish valid state (ReadersCanBuildState),
  // and such a fallback reads the newest data, not the reader's snapshot.
  std::atomic<uint64_t> reader_fallbacks{0};

  // ---- Writes captured by the engine (SQL on this runtime's connection) ----
  // Capture every insert / update / delete this runtime's connection makes on
  // the tables the module depends on (the DuckDB fork's MutationRecorder),
  // projected as the module's dependencies declare. Use with BeginWrite /
  // CommitWrite / RollbackWrite.
  void EnableMutationCapture();
  void BeginWrite();
  // PRE_COMMIT, the engine commit, then COMMIT of the coordinator, with the
  // aux epochs of DuckDB's snapshot order: pending from the writer's start
  // time + 1, published at the commit id + 1. Aborts the aux commit if the
  // engine commit fails.
  void CommitWrite();
  void RollbackWrite();
  // One captured write transaction running `sql` (rolled back on error).
  void ApplyWrite(const std::string &sql);
  // One engine transaction running `sql` that maintains every runtime in
  // `runtimes` (all on the same connection, capture enabled): each one's
  // PRE_COMMIT, the engine commit, then each one's COMMIT -- the commit of a
  // database with several live accelerators.  Rolled back (every aux commit
  // aborted) on error.  `timing`, if given, receives where the commit's time
  // went, per runtime in `runtimes` order.
  struct WriteAllTiming {
    double statement_us = 0;               // the write's statements
    std::vector<double> pre_commit_us;     // each runtime's PRE_COMMIT (its maintenance)
    std::vector<std::string> action;       // each runtime's staged maintenance action
    double engine_commit_us = 0;           // the engine's commit
    std::vector<double> commit_us;         // each runtime's COMMIT (its install)
  };
  static void ApplyWriteAll(const std::vector<DuckdbRuntime *> &runtimes, const std::string &sql,
                            WriteAllTiming *timing = nullptr);
  // The module's calibration writes (core::CalibrationWrite).
  std::vector<core::CalibrationWrite> CalibrationWrites() const;
  // A mutation event from the capture hook (called during the statement).
  void OnCapturedMutation(const ::duckdb::TableMutationEvent &event);

private:
  void DisableMutationCapture();
  // The store the backend uses: verus_store_ if set, else state_store_.
  core::AuxStateStore &ActiveStore();

  ::duckdb::Connection &connection_;
  const SharedQueryCatalogEntry &entry_;
  ::duckdb::shared_ptr<AuxCommitClock> clock_;
  DuckDBStateStore state_store_;
  // The accelerator's verified Verus store, if ACCEL_AUX_STORE=verus and it
  // has one (stateful/state/verus_aux_store.hpp); then the backend uses it
  // instead of state_store_.
  std::unique_ptr<VerusAuxStore> verus_store_;
  core::AuxTransactionCoordinator coordinator_;
  std::shared_ptr<double> expected_queries_before_write_;
  DuckDBAuxBackend backend_;
  std::unique_ptr<Binding> binding_;
  bool capture_enabled_ = false;
};

// The DuckDB side of core::CalibrateFromWrites: a captured write on the
// runtime's connection (mutation capture enabled), maintenance forced or
// chosen, then one accelerator query, timed together.
class DuckdbCalibrationEngine final : public core::CalibrationEngine {
public:
  explicit DuckdbCalibrationEngine(DuckdbRuntime &runtime_p) : runtime(runtime_p) {}
  Timed WriteThenRead(const std::string &sql,
                      std::optional<maintenance::MaintenanceAction> forced) override;

private:
  DuckdbRuntime &runtime;
};

} // namespace accelerator_codegen::aux_state::engine::duckdb
