#include "accelerator_codegen/aux_state/postgres/postgres_aux_session.hpp"

#include <cstdlib>

#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/aux_state/postgres/postgres_mutation_decoder.hpp"
#include "accelerator_codegen/aux_state/postgres/postgres_aux_runtime.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "libpq-fe.h"

namespace accelerator_codegen::aux_state {
namespace {

std::vector<int> ParsePgIntArray(const char *text) {
	std::vector<int> values;
	if (!text || text[0] != '{') {
		return values;
	}
	std::string payload(text + 1);
	if (!payload.empty() && payload.back() == '}') {
		payload.pop_back();
	}
	if (payload.empty()) {
		return values;
	}
	size_t start = 0;
	while (start < payload.size()) {
		const size_t comma = payload.find(',', start);
		const std::string token = payload.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
		if (!token.empty()) {
			values.push_back(std::stoi(token));
		}
		if (comma == std::string::npos) {
			break;
		}
		start = comma + 1;
	}
	return values;
}

std::vector<std::string> ParsePgTextArray(const char *text) {
	std::vector<std::string> values;
	if (!text || text[0] != '{') {
		return values;
	}
	std::string payload(text + 1);
	if (!payload.empty() && payload.back() == '}') {
		payload.pop_back();
	}
	if (payload.empty()) {
		return values;
	}
	size_t i = 0;
	while (i < payload.size()) {
		if (payload[i] == '"') {
			i++;
			std::string token;
			while (i < payload.size()) {
				if (payload[i] == '"') {
					if (i + 1 < payload.size() && payload[i + 1] == '"') {
						token += '"';
						i += 2;
						continue;
					}
					i++;
					break;
				}
				token += payload[i++];
			}
			values.push_back(token);
			if (i < payload.size() && payload[i] == ',') {
				i++;
			}
		} else {
			const size_t comma = payload.find(',', i);
			const std::string token = payload.substr(i, comma == std::string::npos ? std::string::npos : comma - i);
			if (token == "NULL") {
				values.emplace_back();
			} else {
				values.push_back(token);
			}
			if (comma == std::string::npos) {
				break;
			}
			i = comma + 1;
		}
	}
	return values;
}

void RequirePg(bool ok, PGconn *conn, const std::string &context) {
	if (!ok) {
		const char *error = conn ? PQerrorMessage(conn) : "null connection";
		throw std::runtime_error(context + ": " + error);
	}
}

std::string EscapeSqlLiteral(const std::string &value) {
	std::string escaped;
	for (const char ch : value) {
		if (ch == '\'') {
			escaped += "''";
		} else {
			escaped += ch;
		}
	}
	return escaped;
}

} // namespace

TableDependency MakePostgresTableDependency(PGconn &conn, const std::string &schema_name, const std::string &table_name,
                                            const std::vector<std::string> &column_names) {
	TableDependency dependency;
	dependency.table_name = table_name;

	const std::string schema_literal = EscapeSqlLiteral(schema_name);
	const std::string table_literal = EscapeSqlLiteral(table_name);
	const std::string sql =
	    "SELECT c.oid, c.relname, a.attnum, a.attname "
	    "FROM pg_class c "
	    "JOIN pg_namespace n ON n.oid = c.relnamespace "
	    "JOIN pg_attribute a ON a.attrelid = c.oid "
	    "WHERE n.nspname = '" +
	    schema_literal + "' AND c.relname = '" + table_literal + "' AND a.attnum > 0 AND NOT a.attisdropped";

	PGresult *result = PQexec(&conn, sql.c_str());
	RequirePg(result != nullptr && PQresultStatus(result) == PGRES_TUPLES_OK, &conn, "postgres catalog lookup failed");

	std::unordered_map<std::string, std::pair<int, int>> columns_by_name;
	for (int row = 0; row < PQntuples(result); row++) {
		const char *oid_text = PQgetvalue(result, row, 0);
		const char *attnum_text = PQgetvalue(result, row, 2);
		const char *attname = PQgetvalue(result, row, 3);
		if (!oid_text || !attnum_text || !attname) {
			continue;
		}
		dependency.table_oid = static_cast<duckdb::idx_t>(std::stoull(oid_text));
		columns_by_name.emplace(attname, std::make_pair(std::stoi(attnum_text), row));
	}
	PQclear(result);

	for (const auto &column_name : column_names) {
		const auto it = columns_by_name.find(column_name);
		if (it == columns_by_name.end()) {
			throw std::runtime_error("postgres column not found: " + table_name + "." + column_name);
		}
		const int attnum = it->second.first;
		dependency.watched_columns.emplace_back(dependency.table_oid, duckdb::PhysicalIndex(attnum),
		                                        duckdb::LogicalIndex(attnum), column_name);
	}
	return dependency;
}

bool PostgresTableHasColumns(PGconn &conn, const std::string &schema_name, const std::string &table_name,
                             const std::vector<std::string> &column_names) {
	try {
		MakePostgresTableDependency(conn, schema_name, table_name, column_names);
		return true;
	} catch (...) {
		return false;
	}
}

PostgresAuxSession::PostgresAuxSession(PostgresConnectionConfig config) : config_(std::move(config)) {
	if (config_.attach_conninfo.empty()) {
		config_.attach_conninfo = config_.conninfo;
	}
	conn_ = PQconnectdb(config_.conninfo.c_str());
	RequirePg(conn_ && PQstatus(conn_) == CONNECTION_OK, conn_, "postgres connect failed");
	registry_ = duckdb::make_shared_ptr<AcceleratorStateRegistry>();
	txn_buffer_.SetRegistry(registry_.get());
}

PostgresAuxSession::~PostgresAuxSession() {
	if (conn_) {
		PQfinish(conn_);
	}
}

AcceleratorStateRegistry &PostgresAuxSession::Registry() {
	return *registry_;
}

TxnInvalidationBuffer &PostgresAuxSession::TxnBuffer() {
	return txn_buffer_;
}

duckdb::ClientContext &PostgresAuxSession::MutationContext() {
	EnsureMutationContext();
	return *scan_connection_->context;
}

void PostgresAuxSession::OnTableMutation(const duckdb::TableMutationEvent &event) {
	EnsureMutationContext();
	registry_->OnTableMutation(MutationContext(), event, txn_buffer_);
}

void PostgresAuxSession::TransactionPreCommit() {
	if (txn_buffer_.CommittedPendingApply().IsEmpty() && !txn_buffer_.AnyIncrementalHasPendingChanges()) {
		return;
	}
	EnsureMutationContext();
	if (!duck_txn_active_) {
		MutationContext().transaction.BeginTransaction();
		duck_txn_active_ = true;
	}
	auto &context = MutationContext();
	auto &meta = context.transaction.ActiveTransaction();
	txn_buffer_.TransactionPreCommit(meta, context);
}

void PostgresAuxSession::TransactionCommit() {
	EnsureMutationContext();
	if (!duck_txn_active_) {
		return;
	}
	auto &context = MutationContext();
	if (!context.transaction.HasActiveTransaction()) {
		return;
	}
	auto &meta = context.transaction.ActiveTransaction();
	txn_buffer_.TransactionCommit(meta, context);
}

void PostgresAuxSession::TransactionRollback() {
	registry_->DiscardMaintenanceOnRollback(txn_buffer_);
	if (!mutation_context_ready_ || !duck_txn_active_) {
		return;
	}
	auto &context = MutationContext();
	auto &meta = context.transaction.ActiveTransaction();
	txn_buffer_.TransactionRollback(meta, context);
}

void PostgresAuxSession::EnsureMutationContext() {
	if (mutation_context_ready_) {
		return;
	}
	scan_db_ = std::make_unique<duckdb::DuckDB>(nullptr);
	scan_connection_ = std::make_unique<duckdb::Connection>(*scan_db_);
	SyncRegistryOnMutationContext();
	mutation_context_ready_ = true;
}

void PostgresAuxSession::EnsureScanBridge() {
	EnsureMutationContext();
	scan_bridge_ready_ = true;
}

void PostgresAuxSession::RefreshScanTables() {
	EnsureMutationContext();
}

void PostgresAuxSession::WithMutationContextTransaction(const std::function<void(duckdb::ClientContext &)> &fn) {
	EnsureMutationContext();
	auto &context = MutationContext();
	const bool started_transaction = !context.transaction.HasActiveTransaction();
	if (started_transaction) {
		context.transaction.BeginTransaction();
	}
	try {
		fn(context);
		if (started_transaction) {
			context.transaction.Commit();
		}
	} catch (...) {
		if (started_transaction && context.transaction.HasActiveTransaction()) {
			context.transaction.Rollback(nullptr);
		}
		throw;
	}
}

duckdb::Connection &PostgresAuxSession::ScanConnection() {
	EnsureMutationContext();
	return *scan_connection_;
}

std::unique_ptr<duckdb::Connection> PostgresAuxSession::NewScanConnection() {
	EnsureMutationContext();
	auto connection = std::make_unique<duckdb::Connection>(*scan_db_);
	return connection;
}

void PostgresAuxSession::SyncRegistryOnMutationContext() {
	auto &db = *scan_connection_->context->db;
	auto &cache = db.GetObjectCache();
	cache.Put(AcceleratorStateRegistry::CACHE_KEY, registry_);
	cache.Put(PostgresAuxRuntime::CACHE_KEY, duckdb::make_shared_ptr<PostgresAuxRuntime>(conn_));
}

void PostgresAuxSession::BeginTransaction() {
	if (in_transaction_) {
		throw std::runtime_error("postgres session already in transaction");
	}
	EnsureMutationLogTable();
	ExecOrThrow("BEGIN");
	in_transaction_ = true;
	EnsureMutationContext();
	MutationContext().transaction.BeginTransaction();
	duck_txn_active_ = true;
}

void PostgresAuxSession::Commit() {
	if (!in_transaction_) {
		throw std::runtime_error("postgres session not in transaction");
	}
	FlushCapturedMutations();
	const bool has_work = !txn_buffer_.CommittedPendingApply().IsEmpty() || txn_buffer_.AnyIncrementalHasPendingChanges();
	if (has_work) {
		TransactionPreCommit();
	}
	ExecOrThrow("COMMIT");
	in_transaction_ = false;
	if (has_work) {
		TransactionCommit();
	}
	if (duck_txn_active_) {
		auto &context = MutationContext();
		if (context.transaction.HasActiveTransaction()) {
			context.transaction.Commit();
		}
		duck_txn_active_ = false;
	}
}

void PostgresAuxSession::Rollback() {
	if (!in_transaction_) {
		throw std::runtime_error("postgres session not in transaction");
	}
	ExecOrThrow("ROLLBACK");
	in_transaction_ = false;
	TransactionRollback();
	if (duck_txn_active_) {
		auto &context = MutationContext();
		if (context.transaction.HasActiveTransaction()) {
			context.transaction.Rollback(nullptr);
		}
		duck_txn_active_ = false;
	}
}

void PostgresAuxSession::Execute(const std::string &sql) {
	if (!in_transaction_) {
		BeginTransaction();
	}
	ExecOrThrow(sql);
	FlushCapturedMutations();
}

void PostgresAuxSession::EnsureMutationLogTable() {
	ExecOrThrow(
	    "CREATE TEMP TABLE IF NOT EXISTS accel_mutation_log ("
	    "event_id bigserial PRIMARY KEY,"
	    "kind smallint NOT NULL,"
	    "table_oid oid NOT NULL,"
	    "table_name text NOT NULL,"
	    "updated_attnums int[],"
	    "col_attnums int[] NOT NULL,"
	    "col_old text[],"
	    "col_new text[]"
	    ") ON COMMIT DELETE ROWS");
}

void PostgresAuxSession::FlushCapturedMutations() {
	PGresult *result = PQexec(conn_,
	                          "SELECT kind, table_oid::text, table_name, updated_attnums, col_attnums, col_old, col_new "
	                          "FROM accel_mutation_log ORDER BY event_id");
	RequirePg(result != nullptr && PQresultStatus(result) == PGRES_TUPLES_OK, conn_, "read accel_mutation_log failed");

	std::vector<PostgresMutationLogRow> rows;
	for (int row = 0; row < PQntuples(result); row++) {
		PostgresMutationLogRow entry;
		entry.kind = std::stoi(PQgetvalue(result, row, 0));
		entry.table_oid = std::stoull(PQgetvalue(result, row, 1));
		entry.table_name = PQgetvalue(result, row, 2);
		entry.updated_column_attnums = ParsePgIntArray(PQgetvalue(result, row, 3));
		entry.col_attnums = ParsePgIntArray(PQgetvalue(result, row, 4));
		entry.col_old = ParsePgTextArray(PQgetvalue(result, row, 5));
		entry.col_new = ParsePgTextArray(PQgetvalue(result, row, 6));
		rows.push_back(std::move(entry));
	}
	PQclear(result);

	if (rows.empty()) {
		return;
	}

	const auto events = DecodePostgresMutationLog(rows, registry_->GetAllDependencies());
	for (const auto &event : events) {
		OnTableMutation(event);
	}
	PQclear(PQexec(conn_, "DELETE FROM accel_mutation_log"));
}

void PostgresAuxSession::ExecOrThrow(const std::string &sql) {
	PGresult *result = PQexec(conn_, sql.c_str());
	const bool ok = result != nullptr && (PQresultStatus(result) == PGRES_TUPLES_OK || PQresultStatus(result) == PGRES_COMMAND_OK);
	if (!ok) {
		std::string error = PQerrorMessage(conn_);
		if (result) {
			error += PQresultErrorMessage(result);
		}
		PQclear(result);
		throw std::runtime_error("postgres exec failed: " + error);
	}
	PQclear(result);
}

} // namespace accelerator_codegen::aux_state
