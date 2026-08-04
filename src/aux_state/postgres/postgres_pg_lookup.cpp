#include "accelerator_codegen/aux_state/postgres/postgres_pg_lookup.hpp"

#include <stdexcept>
#include <string>

#include "libpq-fe.h"

namespace accelerator_codegen::aux_state {

std::optional<PostgresOrderSnapshot> LookupOrderSnapshotPostgres(PGconn &conn, int64_t orderkey) {
	const std::string orderkey_text = std::to_string(orderkey);
	const char *params[1] = {orderkey_text.c_str()};
	PGresult *result =
	    PQexecParams(&conn, "SELECT o_custkey, o_orderdate FROM orders WHERE o_orderkey = $1::bigint", 1, nullptr,
	                 params, nullptr, nullptr, 0);
	if (result == nullptr || PQresultStatus(result) != PGRES_TUPLES_OK) {
		std::string error = PQerrorMessage(&conn);
		if (result) {
			error += PQresultErrorMessage(result);
		}
		PQclear(result);
		throw std::runtime_error("postgres order lookup failed: " + error);
	}
	if (PQntuples(result) == 0) {
		PQclear(result);
		return std::nullopt;
	}
	PostgresOrderSnapshot snapshot;
	snapshot.custkey = std::stoll(PQgetvalue(result, 0, 0));
	snapshot.orderdate = duckdb::Date::FromString(PQgetvalue(result, 0, 1));
	PQclear(result);
	return snapshot;
}

} // namespace accelerator_codegen::aux_state
