#pragma once

#include <initializer_list>
#include <string>
#include <utility>

#include "backends/duckdb/api/native_scan_compat.hpp"

namespace accelerator_codegen {

inline NativeColumnSpec SecEdgarColumn(const char *name,
                                       duckdb::LogicalType type,
                                       duckdb::PhysicalType physical) {
	return NativeColumnSpec{name, name, std::move(type), physical};
}

inline NativeScanSpec SecEdgarTableScan(
    const std::string &table_name,
    duckdb::vector<NativeColumnSpec> columns) {
	NativeScanSpec spec;
	spec.table_name = table_name;
	spec.projected_columns = std::move(columns);
	return spec;
}

inline NativeScanSpec SecEdgarPreScan(std::initializer_list<const char *> columns) {
	duckdb::vector<NativeColumnSpec> projected;
	for (const char *name : columns) {
		if (std::string(name) == "line") {
			projected.push_back(SecEdgarColumn(
			    name, duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32));
		} else {
			projected.push_back(SecEdgarColumn(
			    name, duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR));
		}
	}
	return SecEdgarTableScan("pre", std::move(projected));
}

inline NativeScanSpec SecEdgarSubScan(std::initializer_list<const char *> columns) {
	duckdb::vector<NativeColumnSpec> projected;
	for (const char *name : columns) {
		if (std::string(name) == "fy" || std::string(name) == "cik" ||
		    std::string(name) == "sic") {
			projected.push_back(SecEdgarColumn(
			    name, duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32));
		} else {
			projected.push_back(SecEdgarColumn(
			    name, duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR));
		}
	}
	return SecEdgarTableScan("sub", std::move(projected));
}

inline NativeScanSpec SecEdgarNumScan(std::initializer_list<const char *> columns) {
	duckdb::vector<NativeColumnSpec> projected;
	for (const char *name : columns) {
		if (std::string(name) == "value") {
			projected.push_back(SecEdgarColumn(
			    name, duckdb::LogicalType::DOUBLE, duckdb::PhysicalType::DOUBLE));
		} else if (std::string(name) == "ddate") {
			projected.push_back(SecEdgarColumn(
			    name, duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32));
		} else {
			projected.push_back(SecEdgarColumn(
			    name, duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR));
		}
	}
	return SecEdgarTableScan("num", std::move(projected));
}

inline NativeScanSpec SecEdgarTagScan(std::initializer_list<const char *> columns) {
	duckdb::vector<NativeColumnSpec> projected;
	for (const char *name : columns) {
		if (std::string(name) == "abstract") {
			projected.push_back(SecEdgarColumn(
			    name, duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32));
		} else {
			projected.push_back(SecEdgarColumn(
			    name, duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR));
		}
	}
	return SecEdgarTableScan("tag", std::move(projected));
}

} // namespace accelerator_codegen
