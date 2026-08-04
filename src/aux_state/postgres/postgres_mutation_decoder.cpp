#include "accelerator_codegen/aux_state/catalog_dependency.hpp"
#include "accelerator_codegen/aux_state/postgres/postgres_mutation_decoder.hpp"

#include <cmath>
#include <optional>
#include <stdexcept>
#include <unordered_map>

#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/decimal.hpp"

namespace accelerator_codegen::aux_state {
namespace {

const TableDependency *FindDependencyByTable(const std::vector<TableDependency> &dependencies, uint64_t table_oid,
                                             const std::string &table_name) {
	for (const auto &dependency : dependencies) {
		if (dependency.table_name == table_name) {
			return &dependency;
		}
	}
	for (const auto &dependency : dependencies) {
		if (dependency.table_oid == table_oid) {
			return &dependency;
		}
	}
	return nullptr;
}

const ColumnRef *FindColumnByAttnum(const TableDependency &dependency, int attnum) {
	for (const auto &column : dependency.watched_columns) {
		if (static_cast<int>(column.physical_index.index) == attnum) {
			return &column;
		}
	}
	return nullptr;
}

duckdb::LogicalType InferLogicalType(const ColumnRef &column) {
	if (column.column_name == "ddate") {
		return duckdb::LogicalType::INTEGER;
	}
	if (column.column_name.find("date") != std::string::npos) {
		return duckdb::LogicalType::DATE;
	}
	if (column.column_name.find("key") != std::string::npos || column.column_name == "cnt") {
		return duckdb::LogicalType::BIGINT;
	}
	if (column.column_name == "value") {
		return duckdb::LogicalType::DOUBLE;
	}
	if (column.column_name.find("price") != std::string::npos || column.column_name.find("discount") != std::string::npos ||
	    column.column_name.find("bal") != std::string::npos) {
		return duckdb::LogicalType::DECIMAL(15, 2);
	}
	return duckdb::LogicalType::VARCHAR;
}

int64_t ScaleMultiplier(uint8_t scale) {
	int64_t multiplier = 1;
	for (uint8_t i = 0; i < scale; i++) {
		multiplier *= 10;
	}
	return multiplier;
}

int64_t ParseDecimalScaled(const std::string &text, uint8_t scale) {
	const double parsed = std::stod(text);
	const double scaled = parsed * static_cast<double>(ScaleMultiplier(scale));
	return static_cast<int64_t>(std::round(scaled));
}

std::optional<duckdb::Value> ParseTextValue(const std::string &text, const duckdb::LogicalType &type) {
	if (text.empty()) {
		return duckdb::Value(type);
	}
	switch (type.id()) {
	case duckdb::LogicalTypeId::BIGINT:
		if (text.find('.') != std::string::npos) {
			return duckdb::Value::BIGINT(ParseDecimalScaled(text, 0));
		}
		return duckdb::Value::BIGINT(std::stoll(text));
	case duckdb::LogicalTypeId::INTEGER:
		if (text.find('.') != std::string::npos) {
			return duckdb::Value::INTEGER(static_cast<int32_t>(std::round(std::stod(text))));
		}
		return duckdb::Value::INTEGER(std::stoi(text));
	case duckdb::LogicalTypeId::DOUBLE:
		return duckdb::Value(std::stod(text));
	case duckdb::LogicalTypeId::DATE:
		return duckdb::Value::DATE(duckdb::Date::FromString(text));
	case duckdb::LogicalTypeId::DECIMAL: {
		const auto width = duckdb::DecimalType::GetWidth(type);
		const auto scale = duckdb::DecimalType::GetScale(type);
		const int64_t scaled = ParseDecimalScaled(text, scale);
		return duckdb::Value::DECIMAL(scaled, width, scale);
	}
	case duckdb::LogicalTypeId::VARCHAR:
		return duckdb::Value(text);
	default:
		return duckdb::Value(text);
	}
}

void AppendProjectedValue(duckdb::TableMutationRow &row, duckdb::PhysicalIndex index, const duckdb::Value &value) {
	row.values.push_back(duckdb::ProjectedColumnValue {index, value});
}

duckdb::TableMutationRow BuildMutationRow(const PostgresMutationLogRow &log_row, const TableDependency &dependency) {
	duckdb::TableMutationRow row;
	const auto kind = static_cast<duckdb::TableMutationKind>(log_row.kind);
	for (size_t i = 0; i < log_row.col_attnums.size(); i++) {
		const auto *column = FindColumnByAttnum(dependency, log_row.col_attnums[i]);
		if (!column) {
			continue;
		}
		const auto logical_type = InferLogicalType(*column);
		if (kind == duckdb::TableMutationKind::INSERT) {
			if (i < log_row.col_new.size()) {
				if (auto value = ParseTextValue(log_row.col_new[i], logical_type)) {
					AppendProjectedValue(row, column->physical_index, *value);
				}
			}
		} else if (kind == duckdb::TableMutationKind::DELETE) {
			if (i < log_row.col_old.size()) {
				if (auto value = ParseTextValue(log_row.col_old[i], logical_type)) {
					AppendProjectedValue(row, column->physical_index, *value);
				}
			}
		} else {
			if (i < log_row.col_old.size()) {
				if (auto value = ParseTextValue(log_row.col_old[i], logical_type)) {
					AppendProjectedValue(row, column->physical_index, *value);
				}
			}
			if (i < log_row.col_new.size()) {
				if (auto value = ParseTextValue(log_row.col_new[i], logical_type)) {
					AppendProjectedValue(row, column->physical_index, *value);
				}
			}
		}
	}
	return row;
}

} // namespace

std::vector<duckdb::TableMutationEvent> DecodePostgresMutationLog(const std::vector<PostgresMutationLogRow> &rows,
                                                                  const std::vector<TableDependency> &dependencies) {
	std::unordered_map<std::string, duckdb::TableMutationEvent> grouped;
	for (const auto &log_row : rows) {
		const auto *dependency = FindDependencyByTable(dependencies, log_row.table_oid, log_row.table_name);
		if (!dependency) {
			continue;
		}
		const std::string group_key = log_row.table_name + ":" + std::to_string(log_row.kind);
		auto &event = grouped[group_key];
		if (event.rows.empty()) {
			event.kind = static_cast<duckdb::TableMutationKind>(log_row.kind);
			event.catalog_oid = dependency->catalog_oid;
			event.schema_oid = dependency->schema_oid;
			event.table_oid = dependency->table_oid;
			event.table_name = dependency->table_name;
			for (const auto attnum : log_row.updated_column_attnums) {
				if (const auto *column = FindColumnByAttnum(*dependency, attnum)) {
					event.updated_columns.emplace_back(column->physical_index);
				}
			}
		}
		event.rows.push_back(BuildMutationRow(log_row, *dependency));
	}
	std::vector<duckdb::TableMutationEvent> events;
	for (auto &entry : grouped) {
		events.push_back(std::move(entry.second));
	}
	return events;
}

} // namespace accelerator_codegen::aux_state
