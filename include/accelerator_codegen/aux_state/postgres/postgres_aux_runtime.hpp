#pragma once

#include <libpq-fe.h>

#include "duckdb/common/string.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/object_cache.hpp"

namespace accelerator_codegen::aux_state {

//! Non-owning handle to the active PostgreSQL connection for postgres-backed aux sessions.
class PostgresAuxRuntime final : public duckdb::ObjectCacheEntry {
public:
	static constexpr const char *CACHE_KEY = "accel_postgres_runtime";

	static duckdb::string ObjectType() {
		return "PostgresAuxRuntime";
	}

	explicit PostgresAuxRuntime(PGconn *conn);

	duckdb::string GetObjectType() override;
	duckdb::optional_idx GetEstimatedCacheMemory() const override;

	PGconn &Connection() {
		return *conn_;
	}

	static PostgresAuxRuntime *TryGet(duckdb::ClientContext &context);
	static PGconn *TryGetConnection(duckdb::ClientContext &context);

private:
	PGconn *conn_ = nullptr;
};

} // namespace accelerator_codegen::aux_state
