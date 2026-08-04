#include "accelerator_codegen/aux_state/postgres/postgres_table_import.hpp"

#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "duckdb/main/connection.hpp"
#include "libpq-fe.h"

namespace accelerator_codegen::aux_state {
namespace {

void ExecDuckOrThrow(duckdb::Connection &conn, const std::string &sql) {
	auto result = conn.Query(sql);
	if (result == nullptr || result->HasError()) {
		throw std::runtime_error("duckdb import failed: " + (result ? result->GetError() : "null") + "\nSQL: " + sql);
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

std::string SqlLiteral(const char *value) {
	if (!value) {
		return "NULL";
	}
	return "'" + EscapeSqlLiteral(value) + "'";
}

void ExecPgOrThrow(PGconn &conn, const std::string &sql) {
	PGresult *result = PQexec(&conn, sql.c_str());
	const bool ok = result != nullptr && PQresultStatus(result) == PGRES_TUPLES_OK;
	if (!ok) {
		std::string error = PQerrorMessage(&conn);
		if (result) {
			error += PQresultErrorMessage(result);
		}
		PQclear(result);
		throw std::runtime_error("postgres import query failed: " + error);
	}
	PQclear(result);
}

void ImportTable(PGconn &pg_conn, duckdb::Connection &duck_conn, const std::string &create_sql, const std::string &table_name) {
	ExecDuckOrThrow(duck_conn, "DROP TABLE IF EXISTS " + table_name);
	ExecDuckOrThrow(duck_conn, create_sql);

	PGresult *result = PQexec(&pg_conn, ("SELECT * FROM " + table_name).c_str());
	if (result == nullptr || PQresultStatus(result) != PGRES_TUPLES_OK) {
		std::string error = PQerrorMessage(&pg_conn);
		if (result) {
			error += PQresultErrorMessage(result);
		}
		PQclear(result);
		throw std::runtime_error("postgres table read failed for " + table_name + ": " + error);
	}

	const int columns = PQnfields(result);
	for (int row = 0; row < PQntuples(result); row++) {
		std::ostringstream insert;
		insert << "INSERT INTO " << table_name << " VALUES (";
		for (int col = 0; col < columns; col++) {
			if (col > 0) {
				insert << ", ";
			}
			if (PQgetisnull(result, row, col)) {
				insert << "NULL";
			} else {
				insert << SqlLiteral(PQgetvalue(result, row, col));
			}
		}
		insert << ")";
		ExecDuckOrThrow(duck_conn, insert.str());
	}
	PQclear(result);
}

} // namespace

void ImportPostgresTablesToDuckdb(PGconn &pg_conn, duckdb::Connection &duck_conn) {
	static const std::unordered_map<std::string, std::string> kCreateSql = {
	    {"nation",
	     "CREATE TABLE nation(n_nationkey INTEGER, n_name VARCHAR, n_regionkey INTEGER, n_comment VARCHAR)"},
	    {"customer",
	     "CREATE TABLE customer(c_custkey BIGINT, c_name VARCHAR, c_address VARCHAR, c_nationkey INTEGER, c_phone VARCHAR, "
	     "c_acctbal DECIMAL(15,2), c_mktsegment VARCHAR, c_comment VARCHAR)"},
	    {"orders",
	     "CREATE TABLE orders(o_orderkey BIGINT, o_custkey BIGINT, o_orderstatus VARCHAR, o_totalprice DECIMAL(15,2), "
	     "o_orderdate DATE, o_orderpriority VARCHAR, o_clerk VARCHAR, o_shippriority INTEGER, o_comment VARCHAR)"},
	    {"lineitem",
	     "CREATE TABLE lineitem(l_orderkey BIGINT, l_partkey BIGINT, l_suppkey BIGINT, l_linenumber BIGINT, "
	     "l_quantity DECIMAL(15,2), l_extendedprice DECIMAL(15,2), l_discount DECIMAL(15,2), l_tax DECIMAL(15,2), "
	     "l_returnflag VARCHAR, l_linestatus VARCHAR, l_shipdate DATE, l_commitdate DATE, l_receiptdate DATE, "
	     "l_shipinstruct VARCHAR, l_shipmode VARCHAR, l_comment VARCHAR)"},
	    {"pre", "CREATE TABLE pre(adsh VARCHAR, tag VARCHAR, version VARCHAR)"},
	    {"num", "CREATE TABLE num(adsh VARCHAR, tag VARCHAR, version VARCHAR, uom VARCHAR, value DOUBLE, ddate INTEGER)"},
	};

	PGresult *tables = PQexec(&pg_conn,
	                          "SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' AND "
	                          "table_type = 'BASE TABLE' ORDER BY table_name");
	if (tables == nullptr || PQresultStatus(tables) != PGRES_TUPLES_OK) {
		PQclear(tables);
		throw std::runtime_error("postgres table listing failed");
	}

	for (int row = 0; row < PQntuples(tables); row++) {
		const char *table_name = PQgetvalue(tables, row, 0);
		if (!table_name) {
			continue;
		}
		const auto it = kCreateSql.find(table_name);
		if (it == kCreateSql.end()) {
			continue;
		}
		ImportTable(pg_conn, duck_conn, it->second, table_name);
	}
	PQclear(tables);
}

} // namespace accelerator_codegen::aux_state
