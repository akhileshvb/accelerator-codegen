#include "accelerator_codegen/aux_state/postgres/postgres_aux_build.hpp"

#include <chrono>
#include <cstdlib>
#include <string>

#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/aux_state/partitioned_state_store.hpp"
#include "accelerator_codegen/aux_state/postgres/postgres_pg_stream.hpp"
#include "accelerator_codegen/sec_edgar_q24.hpp"
#include "accelerator_codegen/sec_edgar_q24_aux.hpp"
#include "accelerator_codegen/sec_edgar_q24_fused.hpp"
#include "accelerator_codegen/sec_edgar_q24_rebuild_policy.hpp"
#include "accelerator_codegen/sec_edgar_q24_write_policy.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/main/client_context.hpp"
#include "libpq-fe.h"

namespace accelerator_codegen::aux_state {
namespace {

constexpr duckdb::idx_t kChunkCapacity = 2048;

std::string Q24NumSelectSql() {
	const char *limit_env = std::getenv("ACCEL_POSTGRES_LOAD_NUM_LIMIT");
	std::string sql =
	    "SELECT adsh, tag, version, value, uom, ddate FROM num "
	    "WHERE uom = 'USD' AND value IS NOT NULL AND ddate BETWEEN 20230101 AND 20231231";
	if (limit_env && *limit_env) {
		const int limit = std::stoi(limit_env);
		if (limit > 0) {
			sql += " LIMIT " + std::to_string(limit);
		}
	}
	return sql;
}

std::string Q24NumCountSql() {
	const char *limit_env = std::getenv("ACCEL_POSTGRES_LOAD_NUM_LIMIT");
	if (limit_env && *limit_env) {
		const int limit = std::stoi(limit_env);
		if (limit > 0) {
			return "SELECT COUNT(*) FROM (" + Q24NumSelectSql() + ") t";
		}
	}
	return "SELECT COUNT(*) FROM num WHERE uom = 'USD' AND value IS NOT NULL AND ddate BETWEEN 20230101 AND 20231231";
}

void ResetChunkVectors(duckdb::DataChunk &chunk) {
	for (duckdb::idx_t col = 0; col < chunk.ColumnCount(); col++) {
		chunk.data[col].SetVectorType(duckdb::VectorType::FLAT_VECTOR);
	}
}

void FeedQ24NumRow(duckdb::DataChunk &chunk, duckdb::idx_t row, PGresult *result, int pg_row) {
	chunk.data[accelerator_codegen::SecEdgarQ24NumChunkLayout::ADSH_INDEX].SetValue(
	    row, duckdb::Value(PQgetvalue(result, pg_row, 0)));
	chunk.data[accelerator_codegen::SecEdgarQ24NumChunkLayout::TAG_INDEX].SetValue(
	    row, duckdb::Value(PQgetvalue(result, pg_row, 1)));
	chunk.data[accelerator_codegen::SecEdgarQ24NumChunkLayout::VERSION_INDEX].SetValue(
	    row, duckdb::Value(PQgetvalue(result, pg_row, 2)));
	chunk.data[accelerator_codegen::SecEdgarQ24NumChunkLayout::VALUE_INDEX].SetValue(
	    row, duckdb::Value(std::stod(PQgetvalue(result, pg_row, 3))));
	chunk.data[accelerator_codegen::SecEdgarQ24NumChunkLayout::UOM_INDEX].SetValue(
	    row, duckdb::Value(PQgetvalue(result, pg_row, 4)));
	chunk.data[accelerator_codegen::SecEdgarQ24NumChunkLayout::DDATE_INDEX].SetValue(
	    row, duckdb::Value::INTEGER(std::stoi(PQgetvalue(result, pg_row, 5))));
}

void FeedQ24PreRow(duckdb::DataChunk &chunk, duckdb::idx_t row, PGresult *result, int pg_row) {
	chunk.data[accelerator_codegen::SecEdgarQ24PreChunkLayout::ADSH_INDEX].SetValue(
	    row, duckdb::Value(PQgetvalue(result, pg_row, 0)));
	chunk.data[accelerator_codegen::SecEdgarQ24PreChunkLayout::TAG_INDEX].SetValue(
	    row, duckdb::Value(PQgetvalue(result, pg_row, 1)));
	chunk.data[accelerator_codegen::SecEdgarQ24PreChunkLayout::VERSION_INDEX].SetValue(
	    row, duckdb::Value(PQgetvalue(result, pg_row, 2)));
}

void StreamQ24Num(PGconn &conn, accelerator_codegen::SecEdgarQ24Kernel &kernel) {
	duckdb::DataChunk chunk;
	chunk.Initialize(duckdb::Allocator::DefaultAllocator(),
	                 {duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR,
	                  duckdb::LogicalType::DOUBLE, duckdb::LogicalType::VARCHAR, duckdb::LogicalType::INTEGER});
	ResetChunkVectors(chunk);

	duckdb::idx_t row = 0;
	StreamPostgresQuery(conn, Q24NumSelectSql(), [&](PGresult *result, int pg_row) {
		FeedQ24NumRow(chunk, row, result, pg_row);
		row++;
		if (row >= kChunkCapacity) {
			chunk.SetCardinality(row);
			kernel.FeedNumChunk(chunk);
			row = 0;
		}
	});
	if (row > 0) {
		chunk.SetCardinality(row);
		kernel.FeedNumChunk(chunk);
	}
}

void StreamQ24Pre(PGconn &conn, accelerator_codegen::SecEdgarQ24Kernel &kernel) {
	duckdb::DataChunk chunk;
	chunk.Initialize(duckdb::Allocator::DefaultAllocator(),
	                 {duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR});
	ResetChunkVectors(chunk);

	duckdb::idx_t row = 0;
	StreamPostgresQuery(conn, "SELECT adsh, tag, version FROM pre", [&](PGresult *result, int pg_row) {
		FeedQ24PreRow(chunk, row, result, pg_row);
		row++;
		if (row >= kChunkCapacity) {
			chunk.SetCardinality(row);
			kernel.FeedPreChunk(chunk);
			row = 0;
		}
	});
	if (row > 0) {
		chunk.SetCardinality(row);
		kernel.FeedPreChunk(chunk);
	}
}

aux_state::PartitionedStateStore &GetSecEdgarQ24Store(duckdb::ClientContext &context) {
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	return registry->GetStore(accelerator_codegen::kSecEdgarQ24AuxAcceleratorId);
}

void PublishQ24Partitions(duckdb::ClientContext &context,
                          const accelerator_codegen::SecEdgarQ24PreMembershipPartition &pre_membership,
                          const accelerator_codegen::SecEdgarQ24TagVersionPartition &tag_versions,
                          const accelerator_codegen::SecEdgarQ24GroupAggregatesPartition &aggregates,
                          const accelerator_codegen::SecEdgarQ24OutputPartition &output) {
	auto &store = GetSecEdgarQ24Store(context);
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	const auto publish_epoch =
	    registry->GetMaintenanceState(accelerator_codegen::kSecEdgarQ24AuxAcceleratorId).BumpCommittedMutationEpoch();
	store.PublishPartition(accelerator_codegen::kSecEdgarQ24PreMembershipSlot,
	                       std::make_shared<accelerator_codegen::SecEdgarQ24PreMembershipPartition>(pre_membership),
	                       publish_epoch);
	store.PublishPartition(accelerator_codegen::kSecEdgarQ24TagVersionSlot,
	                       std::make_shared<accelerator_codegen::SecEdgarQ24TagVersionPartition>(tag_versions),
	                       publish_epoch);
	store.PublishPartition(accelerator_codegen::kSecEdgarQ24GroupAggregatesSlot,
	                       std::make_shared<accelerator_codegen::SecEdgarQ24GroupAggregatesPartition>(aggregates),
	                       publish_epoch);
	store.PublishPartition(accelerator_codegen::kSecEdgarQ24OutputSlot,
	                       std::make_shared<accelerator_codegen::SecEdgarQ24OutputPartition>(output), publish_epoch);
}

} // namespace

void BuildSecEdgarQ24AuxStateFromPostgres(PGconn &conn, duckdb::ClientContext &context) {
	const auto start = std::chrono::steady_clock::now();

	accelerator_codegen::SecEdgarQ24Parameters parameters;
	parameters.row_estimate = static_cast<duckdb::idx_t>(PostgresCountRows(conn, Q24NumCountSql()));

	auto kernel = accelerator_codegen::CreateSecEdgarQ24FusedKernel(parameters);
	auto &fused_kernel = dynamic_cast<accelerator_codegen::SecEdgarQ24FusedKernel &>(*kernel);

	StreamQ24Num(conn, fused_kernel);
	fused_kernel.BuildAdshBloomFilterForPrescan(context);
	StreamQ24Pre(conn, fused_kernel);
	fused_kernel.FinalizeFusedPrescan(context);
	kernel->Finalize();

	accelerator_codegen::SecEdgarQ24PreMembershipPartition pre_membership;
	accelerator_codegen::SecEdgarQ24TagVersionPartition tag_versions;
	accelerator_codegen::SecEdgarQ24GroupAggregatesPartition aggregates;
	accelerator_codegen::SecEdgarQ24OutputPartition output;
	fused_kernel.ExportAuxPartitions(pre_membership, tag_versions, aggregates, output);
	PublishQ24Partitions(context, pre_membership, tag_versions, aggregates, output);

	accelerator_codegen::RecordSecEdgarQ24Rebuild();
	accelerator_codegen::SecEdgarQ24AuxDiagnostics diagnostics;
	diagnostics.build_ns =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
	accelerator_codegen::UpdateSecEdgarQ24AuxDiagnostics(diagnostics);
}

} // namespace accelerator_codegen::aux_state
