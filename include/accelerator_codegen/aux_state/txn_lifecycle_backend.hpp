#pragma once

namespace accelerator_codegen::aux_state {

class TxnSession;

//! Engine-specific installation of mutation capture and (where needed) txn-boundary hooks.
//!
//! DuckDB: no-op Install — fork MutationRecorder + ClientContextState already wired.
//! PostgreSQL: CREATE TRIGGER on each TableDependency + session-scoped mutation log table.
class TxnLifecycleBackend {
public:
	virtual ~TxnLifecycleBackend() = default;

	virtual void Install(TxnSession &session) = 0;
	virtual void Uninstall(TxnSession &session) = 0;
};

} // namespace accelerator_codegen::aux_state
