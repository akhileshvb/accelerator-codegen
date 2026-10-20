// Bind-time parameter resolution for the Q8 accelerators.  nation/region are tiny, so they are read
// through the native storage scan (no nested SQL planning/execution at bind time).
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "backends/duckdb/api/duckdb_scan_adapter.hpp"
#include "tpch/q8/duckdb/base/query.hpp"

namespace accelerator_codegen {

inline void ResolveQ8BindParameters(duckdb::ClientContext &context, Q8Parameters &parameters) {
	NativeScanSpec nation_spec;
	nation_spec.table_name = "nation";
	nation_spec.projected_columns = {
	    {"n_nationkey", "nationkey", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
	    {"n_name", "name", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"n_regionkey", "regionkey", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
	};
	NativeScanSpec region_spec;
	region_spec.table_name = "region";
	region_spec.projected_columns = {
	    {"r_regionkey", "regionkey", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
	    {"r_name", "name", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};

	std::vector<int32_t> matching_regions;
	ScanNativeBaseTable(context, region_spec, [&](duckdb::DataChunk &chunk) {
		duckdb::UnifiedVectorFormat key_format;
		duckdb::UnifiedVectorFormat name_format;
		chunk.data[0].ToUnifiedFormat(chunk.size(), key_format);
		chunk.data[1].ToUnifiedFormat(chunk.size(), name_format);
		const auto keys = duckdb::UnifiedVectorFormat::GetData<int32_t>(key_format);
		const auto names = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(name_format);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto key_index = key_format.sel->get_index(row);
			const auto name_index = name_format.sel->get_index(row);
			if (key_format.validity.RowIsValid(key_index) && name_format.validity.RowIsValid(name_index) &&
			    names[name_index].GetString() == parameters.region_name) {
				matching_regions.push_back(keys[key_index]);
			}
		}
	});

	parameters.region_nations.fill(false);
	std::vector<int32_t> target_nations;
	ScanNativeBaseTable(context, nation_spec, [&](duckdb::DataChunk &chunk) {
		duckdb::UnifiedVectorFormat key_format;
		duckdb::UnifiedVectorFormat name_format;
		duckdb::UnifiedVectorFormat region_format;
		chunk.data[0].ToUnifiedFormat(chunk.size(), key_format);
		chunk.data[1].ToUnifiedFormat(chunk.size(), name_format);
		chunk.data[2].ToUnifiedFormat(chunk.size(), region_format);
		const auto keys = duckdb::UnifiedVectorFormat::GetData<int32_t>(key_format);
		const auto names = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(name_format);
		const auto regions = duckdb::UnifiedVectorFormat::GetData<int32_t>(region_format);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto key_index = key_format.sel->get_index(row);
			const auto name_index = name_format.sel->get_index(row);
			const auto region_index = region_format.sel->get_index(row);
			if (!key_format.validity.RowIsValid(key_index)) {
				continue;
			}
			const auto nationkey = keys[key_index];
			if (name_format.validity.RowIsValid(name_index) && names[name_index].GetString() == parameters.nation_name &&
			    std::find(target_nations.begin(), target_nations.end(), nationkey) == target_nations.end()) {
				target_nations.push_back(nationkey);
			}
			if (region_format.validity.RowIsValid(region_index) && nationkey >= 0 &&
			    nationkey < static_cast<int32_t>(parameters.region_nations.size()) &&
			    std::find(matching_regions.begin(), matching_regions.end(), regions[region_index]) !=
			        matching_regions.end()) {
				parameters.region_nations[static_cast<size_t>(nationkey)] = true;
			}
		}
	});
	if (target_nations.size() != 1) {
		throw duckdb::BinderException("nation name \"%s\" did not resolve to exactly one nation key",
		                              parameters.nation_name);
	}
	parameters.target_nationkey = target_nations[0];
	// Qualifying parts are derived from the part scan itself (no bind-time pre-resolution).
	parameters.bind_part_keys.clear();
}

} // namespace accelerator_codegen
