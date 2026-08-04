#pragma once

#include <libpq-fe.h>

#include <string>

namespace duckdb {
class Connection;
}

namespace accelerator_codegen::aux_state {

void ImportPostgresTablesToDuckdb(PGconn &pg_conn, duckdb::Connection &duck_conn);

} // namespace accelerator_codegen::aux_state
