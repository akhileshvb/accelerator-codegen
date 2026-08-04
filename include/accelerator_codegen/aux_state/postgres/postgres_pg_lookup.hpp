#pragma once

#include <cstdint>
#include <optional>

#include <libpq-fe.h>

#include "duckdb/common/types/date.hpp"

namespace accelerator_codegen::aux_state {

struct PostgresOrderSnapshot {
	int64_t custkey = 0;
	duckdb::date_t orderdate {};
};

std::optional<PostgresOrderSnapshot> LookupOrderSnapshotPostgres(PGconn &conn, int64_t orderkey);

} // namespace accelerator_codegen::aux_state
