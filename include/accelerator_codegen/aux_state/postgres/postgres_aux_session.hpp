#pragma once

#include <libpq-fe.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "accelerator_codegen/aux_state/catalog_dependency.hpp"
#include "accelerator_codegen/aux_state/txn_invalidation_buffer.hpp"
#include "accelerator_codegen/aux_state/txn_session.hpp"
#include "duckdb/common/shared_ptr.hpp"

namespace duckdb {
class Connection;
class DuckDB;
}

namespace accelerator_codegen::aux_state {

struct PostgresConnectionConfig {
	std::string conninfo;
	std::string attach_conninfo; // DuckDB postgres attach string (defaults to conninfo)
};

TableDependency MakePostgresTableDependency(PGconn &conn, const std::string &schema_name, const std::string &table_name,
                                            const std::vector<std::string> &column_names);

bool PostgresTableHasColumns(PGconn &conn, const std::string &schema_name, const std::string &table_name,
                             const std::vector<std::string> &column_names);

//! PostgreSQL-backed txn session: libpq writes + SQL triggers → in-memory aux substrate.
class PostgresAuxSession final : public TxnSession {
public:
	explicit PostgresAuxSession(PostgresConnectionConfig config);
	~PostgresAuxSession() override;

	PGconn &Connection() {
		return *conn_;
	}

	AcceleratorStateRegistry &Registry() override;
	TxnInvalidationBuffer &TxnBuffer() override;
	duckdb::ClientContext &MutationContext() override;

	void OnTableMutation(const duckdb::TableMutationEvent &event) override;
	void TransactionPreCommit() override;
	void TransactionCommit() override;
	void TransactionRollback() override;

	void EnsureScanBridge();
	duckdb::Connection &ScanConnection();
	std::unique_ptr<duckdb::Connection> NewScanConnection();
	void BeginTransaction();
	void Commit();
	void Rollback();
	void Execute(const std::string &sql);

	void EnsureMutationLogTable();
	void FlushCapturedMutations();
	void WithMutationContextTransaction(const std::function<void(duckdb::ClientContext &)> &fn);

private:
	PostgresConnectionConfig config_;
	PGconn *conn_ = nullptr;
	duckdb::shared_ptr<AcceleratorStateRegistry> registry_;
	TxnInvalidationBuffer txn_buffer_;
	std::unique_ptr<duckdb::DuckDB> scan_db_;
	std::unique_ptr<duckdb::Connection> scan_connection_;
	bool in_transaction_ = false;
	bool mutation_context_ready_ = false;
	bool scan_bridge_ready_ = false;
	bool duck_txn_active_ = false;

	void EnsureMutationContext();
	void RefreshScanTables();
	void ExecOrThrow(const std::string &sql);
	void SyncRegistryOnMutationContext();
};

} // namespace accelerator_codegen::aux_state
