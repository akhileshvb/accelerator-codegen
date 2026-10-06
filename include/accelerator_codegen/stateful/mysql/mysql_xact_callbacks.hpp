#pragma once

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
void DispatchMysqlMutation(const char *relname, const core::MutationBatch &mutation);
void DispatchMysqlTxnEvent(bool is_commit, bool is_rollback);
// A captured write could not be decoded/dispatched: every accelerator's state
// may have missed it, so invalidate them all (the next read rebuilds).
// `transaction_open`: a write transaction is still running whose capture failed
// (builds wait for its commit/abort); false for an explicit invalidation.
void InvalidateAllMysqlAccelerators(bool transaction_open = true);
std::vector<MysqlAuxBackend *> RegisteredMysqlMutationBackends();
void RegisterTpchCaptureSchemas();
int MysqlObserverMutationCount();
void MysqlResetObserver();
void MysqlSetForceMaintenanceAction(const char *action_name);
void MysqlSetExpectedQueries(double expected_queries);
double MysqlExpectedQueries();
const char *MysqlLastMaintenanceAction();
void NoteMysqlBackendLastAction(MysqlAuxBackend *backend);

} // namespace accelerator_codegen::aux_state::engine::mysql
