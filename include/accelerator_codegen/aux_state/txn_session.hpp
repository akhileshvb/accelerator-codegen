#pragma once

#include "duckdb/accelerator/mutation_recorder.hpp"

namespace duckdb {
class ClientContext;
}

namespace accelerator_codegen::aux_state {

class AcceleratorStateRegistry;
class TxnInvalidationBuffer;

//! Minimal cross-engine contract for aux txn coordination (substrate, not payload storage).
//!
//! Mutation capture: emit TableMutationEvent for each dependent-table DML within a transaction.
//! Transaction boundaries:
//!   PreCommit  — plan maintenance / apply pre-commit invalidation
//!   Commit     — publish staged maintenance to in-memory PartitionedStateStore
//!   Rollback   — discard staged maintenance
//!
//! DuckDB implements this via fork MutationRecorder + ClientContextState hooks.
//! PostgreSQL implements this via SQL triggers (mutation log) + explicit session calls below.
struct TxnSession {
	virtual ~TxnSession() = default;

	virtual AcceleratorStateRegistry &Registry() = 0;
	virtual TxnInvalidationBuffer &TxnBuffer() = 0;
	virtual duckdb::ClientContext &MutationContext() = 0;

	virtual void OnTableMutation(const duckdb::TableMutationEvent &event) = 0;
	virtual void TransactionPreCommit() = 0;
	virtual void TransactionCommit() = 0;
	virtual void TransactionRollback() = 0;
};

void OnExternalMutation(TxnSession &session, const duckdb::TableMutationEvent &event);

} // namespace accelerator_codegen::aux_state
