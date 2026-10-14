// DuckDB-private scan IR (not shared substrate). Other engines define their own
 // helpers under runtime/{engine}/.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {
class DataChunk;
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

using NativeScanChunkConsumer = std::function<void(duckdb::DataChunk &)>;

} // namespace accelerator_codegen
