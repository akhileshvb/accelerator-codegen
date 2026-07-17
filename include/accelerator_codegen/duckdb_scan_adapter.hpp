#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "accelerator_codegen/chunk_source.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/types/value.hpp"
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

struct NativeColumnSpec {
    std::string source_name;
    std::string kernel_column_id;
    duckdb::LogicalType expected_logical_type;
    duckdb::PhysicalType expected_physical_type;
};

struct NativeFilterSpec {
    std::string source_name;
    duckdb::ExpressionType comparison_type;
    duckdb::Value constant;
};

struct NativeScanSpec {
    std::string catalog_name;
    std::string schema_name;
    std::string table_name;
    duckdb::vector<NativeColumnSpec> projected_columns;
    duckdb::vector<NativeFilterSpec> filters;
};

duckdb::unique_ptr<duckdb::FunctionData> BindNativeScan(duckdb::ClientContext &context, const NativeScanSpec &scan_spec);
void AddNativeTableDependencies(duckdb::LogicalDependencyList &dependencies, const duckdb::FunctionData *bind_data);
duckdb::unique_ptr<ChunkSource> CreateNativeChunkSource(duckdb::ClientContext &context,
                                                        const duckdb::FunctionData &bind_data);

} // namespace accelerator_codegen
