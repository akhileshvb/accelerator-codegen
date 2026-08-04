#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <libpq-fe.h>

namespace accelerator_codegen::aux_state {

void RequirePgResult(bool ok, PGconn *conn, const std::string &context);

int64_t PostgresCountRows(PGconn &conn, const std::string &sql);

void StreamPostgresQuery(PGconn &conn, const std::string &sql,
                         const std::function<void(PGresult *result, int row)> &on_row);

int64_t ParseDecimalScaled(const std::string &text, uint8_t scale);

} // namespace accelerator_codegen::aux_state
