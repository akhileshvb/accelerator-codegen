#include "accelerator_codegen/aux_state/catalog_dependency.hpp"

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

} // namespace accelerator_codegen::aux_state
