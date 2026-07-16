#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "accelerator_codegen/chunk_source.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/storage/storage_index.hpp"

namespace duckdb {
class ClientContext;
class Connection;
class FunctionData;
class LogicalDependencyList;
class LogicalType;
class TableFilterSet;
}

namespace accelerator_codegen {

struct NativeColumnContract {
    std::string name;
    duckdb::LogicalType expected_logical_type;
    duckdb::PhysicalType expected_physical_type;
};

struct NativeTableSpec {
    std::string catalog_name;
    std::string schema_name;
    std::string table_name;
    duckdb::vector<duckdb::StorageIndex> projected_columns;
    duckdb::vector<duckdb::LogicalType> projected_types;
    duckdb::vector<NativeColumnContract> schema_contract;
};

duckdb::unique_ptr<duckdb::FunctionData> CreateLineitemScanBindData(
    duckdb::ClientContext &context,
    const duckdb::vector<std::string> &projected_column_names,
    duckdb::unique_ptr<duckdb::TableFilterSet> table_filters = nullptr);
void AddNativeTableDependencies(duckdb::LogicalDependencyList &dependencies, const duckdb::FunctionData *bind_data);
duckdb::unique_ptr<ChunkSource> CreateChunkSource(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data);

uint64_t CountLineitemRows(duckdb::Connection &connection);
uint64_t CountLineitemRows(duckdb::ClientContext &context);
std::vector<duckdb::date_t> CollectLineitemShipdates(duckdb::Connection &connection);
std::vector<duckdb::date_t> CollectLineitemShipdates(duckdb::ClientContext &context);

} // namespace accelerator_codegen
