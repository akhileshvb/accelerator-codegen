#include "accelerator_codegen/aux_state/catalog_dependency.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/constants.hpp"

namespace accelerator_codegen::aux_state {

std::unordered_set<duckdb::idx_t> TableDependency::WatchedPhysicalIndices() const {
	std::unordered_set<duckdb::idx_t> indices;
	for (const auto &column : watched_columns) {
		indices.insert(column.physical_index.index);
	}
	return indices;
}

bool TableDependency::WatchesPhysicalIndex(duckdb::idx_t physical_index) const {
	for (const auto &column : watched_columns) {
		if (column.physical_index.index == physical_index) {
			return true;
		}
	}
	return false;
}

bool TableHasAllColumns(duckdb::TableCatalogEntry &table, const std::vector<std::string> &column_names) {
	for (const auto &column_name : column_names) {
		if (!table.ColumnExists(column_name)) {
			return false;
		}
	}
	return true;
}

bool CanMakeTableDependency(duckdb::ClientContext &context, const std::string &table_name,
                            const std::vector<std::string> &column_names) {
	auto entry = duckdb::Catalog::GetEntry<duckdb::TableCatalogEntry>(context, INVALID_CATALOG, DEFAULT_SCHEMA, table_name,
	                                                                duckdb::OnEntryNotFound::RETURN_NULL);
	if (!entry) {
		return false;
	}
	return TableHasAllColumns(*entry, column_names);
}

TableDependency MakeTableDependency(duckdb::ClientContext &context, const std::string &table_name,
                                    const std::vector<std::string> &column_names) {
	auto &table = duckdb::Catalog::GetEntry<duckdb::TableCatalogEntry>(context, INVALID_CATALOG, DEFAULT_SCHEMA, table_name);
	TableDependency dependency;
	dependency.catalog_oid = table.catalog.GetOid();
	dependency.schema_oid = table.schema.oid;
	dependency.table_oid = table.oid;
	dependency.table_name = table_name;
	for (const auto &column_name : column_names) {
		const auto &column = table.GetColumn(column_name);
		dependency.watched_columns.emplace_back(table.oid, column.Physical(), column.Logical(), column_name);
	}
	return dependency;
}

} // namespace accelerator_codegen::aux_state
