#pragma once

#include <libpq-fe.h>

namespace duckdb {
class ClientContext;
}

namespace accelerator_codegen::aux_state {

void BuildQ10AuxStateFromPostgres(PGconn &conn, duckdb::ClientContext &context);
void BuildSecEdgarQ24AuxStateFromPostgres(PGconn &conn, duckdb::ClientContext &context);

} // namespace accelerator_codegen::aux_state
