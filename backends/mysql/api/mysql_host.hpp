// GENERATED BACKEND
#pragma once

// One per MySQL server (the plugin's process): the runtime skeleton's host
// with MySQL's slots, the capture of every session's trigger-captured writes
// (S1), and the accelerators the per-query code registers.

#include <cstdint>
#include <memory>
#include <string>

#include "backends/mysql/engine/mysql_statement_capture.hpp"
#include "substrate/aux/runtime/runtime_host.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {

// What a finished statement was, for the transaction bookkeeping.
enum class MysqlStatementKind {
  Commit,              // COMMIT
  Rollback,            // ROLLBACK
  RollbackToSavepoint, // ROLLBACK TO SAVEPOINT
  Begin,               // START TRANSACTION / BEGIN
  Other,               // anything else (DML, DDL, SELECT, SET, ...)
};

class MysqlHost {
public:
  // A connection, keyed by the server's THD (see mysql_session.hpp).
  using Session = std::uintptr_t;

  static MysqlHost &Get();
  ~MysqlHost();
  MysqlHost(const MysqlHost &) = delete;
  MysqlHost &operator=(const MysqlHost &) = delete;

  // Registers the module's accelerator (once; not built yet). Captured rows
  // carry one table id per MySQL table (the host's); `tables` maps a module
  // that numbers its tables differently to them.
  runtime::AcceleratorRuntime &Register(std::shared_ptr<core::AuxQueryModule> module,
                                        maintenance::CostModelParams cost_model,
                                        const runtime::TableMap &tables = {});
  runtime::RuntimeHost &Runtime();

  // ---- engine callbacks (the capture UDF and the audit plugin) ----
  // A row the triggers captured in `session`'s open statement. Throws if it
  // does not decode (the caller reports OnCaptureFailure).
  void OnCapturedRow(Session session, const CapturedRow &row);
  // A captured row was lost: no accelerator's state can be trusted.
  void OnCaptureFailure(Session session);
  // `session`'s statement finished. `in_active_transaction`: the session is
  // inside an explicit transaction afterwards, so the statement's writes stay
  // with it; otherwise they were committed (or, if `failed`, rolled back).
  void OnStatementEnd(Session session, MysqlStatementKind kind, bool failed,
                      bool in_active_transaction);
  // The connection closed: its open transaction is rolled back.
  void OnSessionEnd(Session session);
  // Every accelerator's state is rebuilt before it is read again.
  void InvalidateAll();

  // ---- diagnostics ----
  uint64_t CapturedRowCount() const;
  void ResetCapturedRowCount();
  // The accelerator a query last read, whose last maintenance action
  // LastMaintenanceAction reports ("": none).
  void NoteRead(const runtime::AcceleratorRuntime &accelerator);
  std::string LastMaintenanceAction() const;
  // Forces every later maintenance decision to `action` ("": the policy chooses).
  void ForceMaintenanceAction(const std::string &action);
  // Sums over every accelerator.
  runtime::AcceleratorStats TotalStats() const;

private:
  MysqlHost();
  struct Impl;
  std::unique_ptr<Impl> impl;
};

} // namespace accelerator_codegen::aux_state::engine::mysql
