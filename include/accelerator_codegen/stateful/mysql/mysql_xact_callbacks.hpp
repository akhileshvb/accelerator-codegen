#pragma once

#include <cstdint>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {

class MysqlAuxBackend;

void RegisterMysqlXactCallbacks();
bool RegisterMysqlBackendForMutations();
void SetMysqlActiveBackend(MysqlAuxBackend *backend);
MysqlAuxBackend *GetMysqlActiveBackend();
void RegisterMysqlMutationBackend(MysqlAuxBackend *backend);
void UnregisterMysqlMutationBackend(MysqlAuxBackend *backend);
bool HasRegisteredMysqlMutationBackend();
// ---- Sessions and transactions ---------------------------------------------
// Captured writes are staged per session (connection) in every registered
// accelerator and committed or rolled back with that session's transaction
// only. A session is keyed by the server's THD (see CurrentMysqlSession).
//
// A trigger-captured write of the calling session (typed AuxRow batch on a
// table the accelerators depend on; the table id and column types come from
// the capture call, see mysql_accel_capture_row).
void DispatchMysqlMutation(const core::MutationBatch &mutation);
// What a finished statement was, for the transaction bookkeeping.
enum class MysqlStatementKind {
  Commit,              // COMMIT
  Rollback,            // ROLLBACK
  RollbackToSavepoint, // ROLLBACK TO SAVEPOINT
  Begin,               // START TRANSACTION / BEGIN
  Other,               // anything else (DML, DDL, SELECT, SET, ...)
};
// A statement of `session` finished (audit general-status event).
// `in_active_transaction`: the session is inside an explicit (multi-statement)
// transaction after the statement, so a plain statement's writes stay staged;
// otherwise they were committed (success) or rolled back (`failed`) with it.
void DispatchMysqlStatementEnd(std::uintptr_t session, MysqlStatementKind kind,
                               bool failed, bool in_active_transaction);
// The connection closed: its open transaction is rolled back.
void DispatchMysqlSessionEnd(std::uintptr_t session);
// Session key of the connection the calling thread serves / of a server THD
// handle (an audit event's), and whether that connection is inside an explicit
// transaction (false if the server does not export the query).
std::uintptr_t CurrentMysqlSession();
std::uintptr_t MysqlSessionFromThd(const void *thd);
bool MysqlThdInActiveTransaction(const void *thd);
// A captured write could not be decoded/dispatched: every accelerator's state
// may have missed it, so invalidate them all (the next read rebuilds).
// `transaction_open`: a write transaction is still running whose capture failed
// (builds wait for its commit/abort); false for an explicit invalidation.
void InvalidateAllMysqlAccelerators(bool transaction_open = true);
std::vector<MysqlAuxBackend *> RegisteredMysqlMutationBackends();
int MysqlObserverMutationCount();
void MysqlResetObserver();
void MysqlSetForceMaintenanceAction(const char *action_name);
void MysqlSetExpectedQueries(double expected_queries);
double MysqlExpectedQueries();
const char *MysqlLastMaintenanceAction();
void NoteMysqlBackendLastAction(MysqlAuxBackend *backend);

} // namespace accelerator_codegen::aux_state::engine::mysql
