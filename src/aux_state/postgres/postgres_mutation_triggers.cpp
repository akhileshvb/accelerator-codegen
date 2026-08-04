#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/aux_state/catalog_dependency.hpp"
#include "accelerator_codegen/aux_state/postgres/postgres_mutation_triggers.hpp"

#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "accelerator_codegen/aux_state/postgres/postgres_aux_session.hpp"
#include "libpq-fe.h"

namespace accelerator_codegen::aux_state {
namespace {

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

std::string SqlTextExpr(const std::string &sql_expr) {
	return sql_expr + "::text";
}

std::string BuildCaptureFunction(const std::string &function_name, const TableDependency &dependency,
                                 const std::vector<std::pair<std::string, std::string>> &columns) {
	std::ostringstream attnums;
	std::ostringstream old_exprs;
	std::ostringstream new_exprs;
	for (size_t i = 0; i < columns.size(); i++) {
		if (i > 0) {
			attnums << ", ";
			old_exprs << ", ";
			new_exprs << ", ";
		}
		const auto &column = dependency.watched_columns[i];
		attnums << column.physical_index.index;
		old_exprs << SqlTextExpr("OLD." + columns[i].first);
		new_exprs << SqlTextExpr("NEW." + columns[i].first);
	}

	std::ostringstream null_text_array;
	null_text_array << "ARRAY[";
	for (size_t i = 0; i < columns.size(); i++) {
		if (i > 0) {
			null_text_array << ", ";
		}
		null_text_array << "NULL::text";
	}
	null_text_array << "]";

	std::ostringstream sql;
	sql << "CREATE OR REPLACE FUNCTION " << function_name << "() RETURNS trigger AS $$\n"
	    << "DECLARE updated_attnums int[] := '{}'; BEGIN\n"
	    << "  IF TG_OP = 'INSERT' THEN\n"
	    << "    INSERT INTO accel_mutation_log (kind, table_oid, table_name, updated_attnums, col_attnums, col_old, col_new)\n"
	    << "    VALUES (0, TG_RELID, TG_TABLE_NAME, updated_attnums, ARRAY[" << attnums.str() << "], "
	    << null_text_array.str() << ", ARRAY[" << new_exprs.str() << "]);\n"
	    << "    RETURN NEW;\n"
	    << "  ELSIF TG_OP = 'DELETE' THEN\n"
	    << "    INSERT INTO accel_mutation_log (kind, table_oid, table_name, updated_attnums, col_attnums, col_old, col_new)\n"
	    << "    VALUES (1, TG_RELID, TG_TABLE_NAME, updated_attnums, ARRAY[" << attnums.str() << "], "
	    << "ARRAY[" << old_exprs.str() << "], " << null_text_array.str() << ");\n"
	    << "    RETURN OLD;\n"
	    << "  ELSIF TG_OP = 'UPDATE' THEN\n"
	    << "    updated_attnums := ARRAY[]::int[];\n";
	for (size_t i = 0; i < columns.size(); i++) {
		const auto &name = columns[i].first;
		sql << "    IF OLD." << name << " IS DISTINCT FROM NEW." << name << " THEN updated_attnums := "
		    << "array_append(updated_attnums, " << dependency.watched_columns[i].physical_index.index << "); END IF;\n";
	}
	sql << "    INSERT INTO accel_mutation_log (kind, table_oid, table_name, updated_attnums, col_attnums, col_old, col_new)\n"
	    << "    VALUES (2, TG_RELID, TG_TABLE_NAME, updated_attnums, ARRAY[" << attnums.str() << "], "
	    << "ARRAY[" << old_exprs.str() << "], ARRAY[" << new_exprs.str() << "]);\n"
	    << "    RETURN NEW;\n"
	    << "  END IF;\n"
	    << "  RETURN NULL;\n"
	    << "END;\n"
	    << "$$ LANGUAGE plpgsql;";
	return sql.str();
}

void ExecSql(PGconn &conn, const std::string &sql) {
	PGresult *result = PQexec(&conn, sql.c_str());
	const bool ok = result != nullptr && (PQresultStatus(result) == PGRES_TUPLES_OK || PQresultStatus(result) == PGRES_COMMAND_OK);
	if (!ok) {
		std::string error = PQerrorMessage(&conn);
		if (result) {
			error += PQresultErrorMessage(result);
		}
		PQclear(result);
		throw std::runtime_error("postgres trigger sql failed: " + error + "\nSQL: " + sql);
	}
	PQclear(result);
}

void InstallTableTrigger(PGconn &conn, const std::string &table_name, const std::string &function_name,
                         const std::string &trigger_name) {
	ExecSql(conn, "DROP TRIGGER IF EXISTS " + trigger_name + " ON " + table_name);
	ExecSql(conn, "CREATE TRIGGER " + trigger_name + " AFTER INSERT OR UPDATE OR DELETE ON " + table_name +
	                      " FOR EACH ROW EXECUTE FUNCTION " + function_name + "()");
}

void UninstallTableTrigger(PGconn &conn, const std::string &table_name, const std::string &trigger_name,
                           const std::string &function_name) {
	ExecSql(conn, "DROP TRIGGER IF EXISTS " + trigger_name + " ON " + table_name);
	ExecSql(conn, "DROP FUNCTION IF EXISTS " + function_name + "()");
}

} // namespace

void InstallPostgresQ10Triggers(PostgresAuxSession &session) {
	auto &conn = session.Connection();
	session.EnsureMutationLogTable();

	const auto &deps = session.Registry().GetAcceleratorDependencies("tpch_q10_aux");
	for (const auto &dependency : deps) {
		std::vector<std::pair<std::string, std::string>> columns;
		if (dependency.table_name == "customer") {
			columns = {{"c_custkey", "c_custkey"}, {"c_name", "c_name"}, {"c_acctbal", "c_acctbal"},
			           {"c_nationkey", "c_nationkey"}, {"c_address", "c_address"}, {"c_phone", "c_phone"},
			           {"c_comment", "c_comment"}};
		} else if (dependency.table_name == "orders") {
			columns = {{"o_orderkey", "o_orderkey"}, {"o_custkey", "o_custkey"}, {"o_orderdate", "o_orderdate"}};
		} else if (dependency.table_name == "lineitem") {
			columns = {{"l_orderkey", "l_orderkey"}, {"l_extendedprice", "l_extendedprice"}, {"l_discount", "l_discount"},
			           {"l_returnflag", "l_returnflag"}};
		} else if (dependency.table_name == "nation") {
			columns = {{"n_nationkey", "n_nationkey"}, {"n_name", "n_name"}};
		} else {
			continue;
		}
		const std::string function_name = "accel_q10_" + dependency.table_name + "_mutation";
		const std::string trigger_name = "accel_q10_" + dependency.table_name + "_mutation_trg";
		ExecSql(conn, BuildCaptureFunction(function_name, dependency, columns));
		InstallTableTrigger(conn, dependency.table_name, function_name, trigger_name);
	}
}

void UninstallPostgresQ10Triggers(PostgresAuxSession &session) {
	auto &conn = session.Connection();
	const std::vector<std::string> tables = {"customer", "orders", "lineitem", "nation"};
	for (const auto &table : tables) {
		const std::string function_name = "accel_q10_" + table + "_mutation";
		const std::string trigger_name = "accel_q10_" + table + "_mutation_trg";
		UninstallTableTrigger(conn, table, trigger_name, function_name);
	}
}

void InstallPostgresQ24Triggers(PostgresAuxSession &session) {
	auto &conn = session.Connection();
	session.EnsureMutationLogTable();

	const auto &deps = session.Registry().GetAcceleratorDependencies("sec_edgar_q24_aux");
	for (const auto &dependency : deps) {
		std::vector<std::pair<std::string, std::string>> columns;
		if (dependency.table_name == "num") {
			columns = {{"adsh", "adsh"}, {"tag", "tag"}, {"version", "version"}, {"uom", "uom"}, {"value", "value"},
			           {"ddate", "ddate"}};
		} else if (dependency.table_name == "pre") {
			columns = {{"adsh", "adsh"}, {"tag", "tag"}, {"version", "version"}};
		} else {
			continue;
		}
		const std::string function_name = "accel_q24_" + dependency.table_name + "_mutation";
		const std::string trigger_name = "accel_q24_" + dependency.table_name + "_mutation_trg";
		ExecSql(conn, BuildCaptureFunction(function_name, dependency, columns));
		InstallTableTrigger(conn, dependency.table_name, function_name, trigger_name);
	}
}

void UninstallPostgresQ24Triggers(PostgresAuxSession &session) {
	auto &conn = session.Connection();
	for (const auto &table : {"num", "pre"}) {
		const std::string function_name = "accel_q24_" + std::string(table) + "_mutation";
		const std::string trigger_name = "accel_q24_" + std::string(table) + "_mutation_trg";
		UninstallTableTrigger(conn, table, trigger_name, function_name);
	}
}

void PostgresQ10TxnLifecycleBackend::Install(TxnSession &session) {
	auto &pg_session = dynamic_cast<PostgresAuxSession &>(session);
	InstallPostgresQ10Triggers(pg_session);
}

void PostgresQ10TxnLifecycleBackend::Uninstall(TxnSession &session) {
	auto &pg_session = dynamic_cast<PostgresAuxSession &>(session);
	UninstallPostgresQ10Triggers(pg_session);
}

void PostgresQ24TxnLifecycleBackend::Install(TxnSession &session) {
	auto &pg_session = dynamic_cast<PostgresAuxSession &>(session);
	InstallPostgresQ24Triggers(pg_session);
}

void PostgresQ24TxnLifecycleBackend::Uninstall(TxnSession &session) {
	auto &pg_session = dynamic_cast<PostgresAuxSession &>(session);
	UninstallPostgresQ24Triggers(pg_session);
}

} // namespace accelerator_codegen::aux_state
