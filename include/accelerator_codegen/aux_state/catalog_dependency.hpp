#pragma once

#include <string>
#include <unordered_set>
#include <vector>

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/storage/storage_index.hpp"

namespace accelerator_codegen::aux_state {

struct ColumnRef {
	duckdb::idx_t table_oid = 0;
	duckdb::PhysicalIndex physical_index;
	duckdb::LogicalIndex logical_index;
	std::string column_name;

	ColumnRef(duckdb::idx_t table_oid_p, duckdb::PhysicalIndex physical_index_p, duckdb::LogicalIndex logical_index_p,
	          std::string column_name_p)
	    : table_oid(table_oid_p), physical_index(physical_index_p), logical_index(logical_index_p),
	      column_name(std::move(column_name_p)) {
	}
};

struct TableDependency {
	duckdb::idx_t catalog_oid = 0;
	duckdb::idx_t schema_oid = 0;
	duckdb::idx_t table_oid = 0;
	std::string table_name;
	std::vector<ColumnRef> watched_columns;

	std::unordered_set<duckdb::idx_t> WatchedPhysicalIndices() const;
	bool WatchesPhysicalIndex(duckdb::idx_t physical_index) const;
};

} // namespace accelerator_codegen::aux_state
