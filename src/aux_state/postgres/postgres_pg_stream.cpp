#include "accelerator_codegen/aux_state/postgres/postgres_pg_stream.hpp"

#include <cmath>
#include <stdexcept>

#include "libpq-fe.h"

namespace accelerator_codegen::aux_state {

void RequirePgResult(bool ok, PGconn *conn, const std::string &context) {
	if (!ok) {
		const char *error = conn ? PQerrorMessage(conn) : "null connection";
		throw std::runtime_error(context + ": " + error);
	}
}

int64_t ScaleMultiplier(uint8_t scale) {
	int64_t multiplier = 1;
	for (uint8_t i = 0; i < scale; i++) {
		multiplier *= 10;
	}
	return multiplier;
}

int64_t ParseDecimalScaled(const std::string &text, uint8_t scale) {
	const double parsed = std::stod(text);
	const double scaled = parsed * static_cast<double>(ScaleMultiplier(scale));
	return static_cast<int64_t>(std::round(scaled));
}

int64_t PostgresCountRows(PGconn &conn, const std::string &sql) {
	PGresult *result = PQexec(&conn, sql.c_str());
	RequirePgResult(result != nullptr && PQresultStatus(result) == PGRES_TUPLES_OK, &conn, "postgres count failed");
	const int64_t count = std::stoll(PQgetvalue(result, 0, 0));
	PQclear(result);
	return count;
}

void StreamPostgresQuery(PGconn &conn, const std::string &sql,
                         const std::function<void(PGresult *result, int row)> &on_row) {
	PGresult *result = PQexec(&conn, sql.c_str());
	RequirePgResult(result != nullptr && PQresultStatus(result) == PGRES_TUPLES_OK, &conn, "postgres query failed");
	const int rows = PQntuples(result);
	for (int row = 0; row < rows; row++) {
		on_row(result, row);
	}
	PQclear(result);
}

} // namespace accelerator_codegen::aux_state
