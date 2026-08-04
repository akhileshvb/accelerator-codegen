#include "accelerator_codegen/aux_state/postgres/postgres_aux_runtime.hpp"

namespace accelerator_codegen::aux_state {

PostgresAuxRuntime::PostgresAuxRuntime(PGconn *conn) : conn_(conn) {
}

duckdb::string PostgresAuxRuntime::GetObjectType() {
	return ObjectType();
}

duckdb::optional_idx PostgresAuxRuntime::GetEstimatedCacheMemory() const {
	return duckdb::optional_idx();
}

PostgresAuxRuntime *PostgresAuxRuntime::TryGet(duckdb::ClientContext &context) {
	auto &cache = context.db->GetObjectCache();
	auto runtime = cache.Get<PostgresAuxRuntime>(CACHE_KEY);
	return runtime ? runtime.get() : nullptr;
}

PGconn *PostgresAuxRuntime::TryGetConnection(duckdb::ClientContext &context) {
	auto *runtime = TryGet(context);
	return runtime ? &runtime->Connection() : nullptr;
}

} // namespace accelerator_codegen::aux_state
