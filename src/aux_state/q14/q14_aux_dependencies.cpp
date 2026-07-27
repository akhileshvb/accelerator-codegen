#include "accelerator_codegen/tpch_q14_aux.hpp"
#include "accelerator_codegen/tpch_q14_incremental.hpp"
#include "accelerator_codegen/tpch_q14_maintenance.hpp"
#include "accelerator_codegen/tpch_q14_maintenance_policy.hpp"

#include <cstdlib>
#include <cstring>
#include <optional>
#include <unordered_map>

#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/aux_state/catalog_dependency.hpp"
#include "accelerator_codegen/aux_state/invalidation_plan.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"

namespace accelerator_codegen {
namespace {

bool ReadBoolEnv(const char *name, bool fallback) {
	const char *value = std::getenv(name);
	if (!value || !*value) {
		return fallback;
	}
	return std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 || std::strcmp(value, "TRUE") == 0;
}

aux_state::ColumnRef MakeColumnRef(duckdb::TableCatalogEntry &table, const std::string &name) {
	const auto &column = table.GetColumn(name);
	return aux_state::ColumnRef {table.oid, column.Physical(), column.Logical(), name};
}

aux_state::TableDependency MakeTableDependency(duckdb::ClientContext &context, const std::string &table_name,
                                               const std::vector<std::string> &columns) {
	auto &table = duckdb::Catalog::GetEntry<duckdb::TableCatalogEntry>(context, INVALID_CATALOG, DEFAULT_SCHEMA, table_name);
	aux_state::TableDependency dependency;
	dependency.catalog_oid = table.catalog.GetOid();
	dependency.schema_oid = table.schema.oid;
	dependency.table_oid = table.oid;
	dependency.table_name = table_name;
	for (const auto &column_name : columns) {
		dependency.watched_columns.push_back(MakeColumnRef(table, column_name));
	}
	return dependency;
}

const aux_state::ColumnRef *FindColumn(const aux_state::TableDependency &dependency, const std::string &name) {
	for (const auto &column : dependency.watched_columns) {
		if (column.column_name == name) {
			return &column;
		}
	}
	return nullptr;
}

std::optional<duckdb::date_t> FindDateValue(const duckdb::TableMutationEvent &event,
                                            const aux_state::ColumnRef &column_ref, bool old_value) {
	for (const auto &row : event.rows) {
		for (const auto &value : row.values) {
			if (value.column_index.index != column_ref.physical_index.index) {
				continue;
			}
			(void)old_value;
			if (value.value.IsNull()) {
				return std::nullopt;
			}
			return value.value.GetValue<duckdb::date_t>();
		}
	}
	return std::nullopt;
}

std::optional<Q14PartitionKey> MonthFromMutationRow(const duckdb::TableMutationRow &row,
                                                  const aux_state::ColumnRef &shipdate_column) {
	std::vector<duckdb::Value> values;
	for (const auto &projected : row.values) {
		if (projected.column_index.index == shipdate_column.physical_index.index) {
			values.push_back(projected.value);
		}
	}
	if (values.empty()) {
		return std::nullopt;
	}
	const auto &picked = values.size() == 1 ? values.front() : values.back();
	if (picked.IsNull()) {
		return std::nullopt;
	}
	return PartitionKeyFromDate(picked.GetValue<duckdb::date_t>());
}

std::optional<Q14PartitionKey> MonthFromEventRow(const duckdb::TableMutationEvent &event, idx_t row_idx,
                                                 const aux_state::ColumnRef &shipdate_column) {
	if (row_idx >= event.rows.size()) {
		return std::nullopt;
	}
	return MonthFromMutationRow(event.rows[row_idx], shipdate_column);
}

bool UpdateTouchesRelevantColumns(const duckdb::TableMutationEvent &event,
                                  const aux_state::TableDependency &dependency) {
	for (const auto &updated : event.updated_columns) {
		if (dependency.WatchesPhysicalIndex(updated.index)) {
			return true;
		}
	}
	return false;
}

aux_state::InvalidationPlan InterpretLineitemMutation(const duckdb::TableMutationEvent &event,
                                                      const aux_state::TableDependency &dependency) {
	aux_state::InvalidationPlan plan;
	const auto *shipdate_column = FindColumn(dependency, "l_shipdate");
	const auto *partkey_column = FindColumn(dependency, "l_partkey");
	const auto *price_column = FindColumn(dependency, "l_extendedprice");
	const auto *discount_column = FindColumn(dependency, "l_discount");
	if (!shipdate_column || !partkey_column || !price_column || !discount_column) {
		plan.scope = aux_state::InvalidationScope::AllPartitions;
		return plan;
	}

	switch (event.kind) {
	case duckdb::TableMutationKind::INSERT:
	case duckdb::TableMutationKind::DELETE: {
		plan.scope = aux_state::InvalidationScope::PartitionSet;
		for (idx_t row = 0; row < event.rows.size(); row++) {
			auto month = MonthFromEventRow(event, row, *shipdate_column);
			if (!month) {
				plan.scope = aux_state::InvalidationScope::AllPartitions;
				plan.partitions.clear();
				return plan;
			}
			plan.partitions.insert(*month);
		}
		return plan;
	}
	case duckdb::TableMutationKind::UPDATE: {
		if (!UpdateTouchesRelevantColumns(event, dependency)) {
			return plan;
		}
		plan.scope = aux_state::InvalidationScope::PartitionSet;
		for (const auto &row : event.rows) {
			std::vector<Q14PartitionKey> shipdate_months;
			for (const auto &value : row.values) {
				if (value.column_index.index != shipdate_column->physical_index.index) {
					continue;
				}
				if (value.value.IsNull()) {
					plan.scope = aux_state::InvalidationScope::AllPartitions;
					plan.partitions.clear();
					return plan;
				}
				shipdate_months.push_back(PartitionKeyFromDate(value.value.GetValue<duckdb::date_t>()));
			}
			if (shipdate_months.empty()) {
				if (auto month = MonthFromMutationRow(row, *shipdate_column)) {
					plan.partitions.insert(*month);
				}
				continue;
			}
			plan.partitions.insert(shipdate_months.front());
			if (shipdate_months.size() > 1) {
				plan.partitions.insert(shipdate_months.back());
			}
		}
		if (plan.partitions.empty()) {
			plan.scope = aux_state::InvalidationScope::AllPartitions;
		}
		return plan;
	}
	default:
		break;
	}
	return plan;
}

aux_state::InvalidationPlan InterpretPartMutation(const duckdb::TableMutationEvent &event,
                                                  const aux_state::TableDependency &dependency) {
	aux_state::InvalidationPlan plan;
	const auto *partkey_column = FindColumn(dependency, "p_partkey");
	const auto *type_column = FindColumn(dependency, "p_type");
	if (!partkey_column || !type_column) {
		plan.scope = aux_state::InvalidationScope::AllPartitions;
		return plan;
	}

	switch (event.kind) {
	case duckdb::TableMutationKind::INSERT:
	case duckdb::TableMutationKind::DELETE:
		return plan;
	case duckdb::TableMutationKind::UPDATE:
		if (!UpdateTouchesRelevantColumns(event, dependency)) {
			return plan;
		}
		return plan;
	default:
		return plan;
	}
}

aux_state::InvalidationPlan InterpretQ14Mutation(const duckdb::TableMutationEvent &event,
                                                 const std::vector<aux_state::TableDependency> &dependencies) {
	const aux_state::TableDependency *lineitem = nullptr;
	const aux_state::TableDependency *part = nullptr;
	for (const auto &dependency : dependencies) {
		if (dependency.table_name == "lineitem") {
			lineitem = &dependency;
		} else if (dependency.table_name == "part") {
			part = &dependency;
		}
	}
	if (event.table_oid == lineitem->table_oid) {
		return InterpretLineitemMutation(event, *lineitem);
	}
	if (event.table_oid == part->table_oid) {
		return InterpretPartMutation(event, *part);
	}
	return {};
}

} // namespace

bool Q14AuxEnabled() {
	return ReadBoolEnv("ACCEL_Q14_AUX_STATE", true);
}

Q14PartitionKey PartitionKeyFromDate(duckdb::date_t shipdate) {
	const auto year = duckdb::Date::ExtractYear(shipdate);
	const auto month = duckdb::Date::ExtractMonth(shipdate);
	return static_cast<Q14PartitionKey>(year * 100 + month);
}

Q14PartitionKey PartitionKeyFromStartDate(duckdb::date_t start_date) {
	return PartitionKeyFromDate(start_date);
}

void RegisterQ14AuxAccelerator(duckdb::Connection &connection) {
	LoadQ14CostModelParams();
	auto &context = *connection.context;
	auto &catalog = duckdb::Catalog::GetSystemCatalog(context);
	auto lineitem = catalog.GetEntry<duckdb::TableCatalogEntry>(context, INVALID_CATALOG, DEFAULT_SCHEMA, "lineitem",
	                                                            duckdb::OnEntryNotFound::RETURN_NULL);
	auto part = catalog.GetEntry<duckdb::TableCatalogEntry>(context, INVALID_CATALOG, DEFAULT_SCHEMA, "part",
	                                                        duckdb::OnEntryNotFound::RETURN_NULL);
	if (!lineitem || !part) {
		return;
	}

	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	registry->RegisterAccelerator(
	    kQ14AuxAcceleratorId,
	    [](const duckdb::TableMutationEvent &event,
	       const std::vector<aux_state::TableDependency> &dependencies) -> aux_state::InvalidationPlan {
		    return InterpretQ14Mutation(event, dependencies);
	    });
	registry->RegisterIncrementalBufferFactory(kQ14AuxAcceleratorId,
	                                           []() { return std::make_unique<Q14IncrementalTxnBuffer>(); });
	registry->RegisterMutationAccumulator(kQ14AuxAcceleratorId, AccumulateQ14Mutation);
	registry->RegisterMaintenanceHook(kQ14AuxAcceleratorId, CreateQ14MaintenanceHook());
	registry->RegisterTableDependency(kQ14AuxAcceleratorId,
	                                  MakeTableDependency(context, "lineitem",
	                                                      {"l_shipdate", "l_partkey", "l_extendedprice", "l_discount"}));
	registry->RegisterTableDependency(kQ14AuxAcceleratorId, MakeTableDependency(context, "part", {"p_partkey", "p_type"}));
}

bool IsQ14DependentTableMutation(const duckdb::TableMutationEvent &event,
                               const std::vector<aux_state::TableDependency> &dependencies) {
	for (const auto &dependency : dependencies) {
		if (event.table_oid == dependency.table_oid) {
			return true;
		}
	}
	return false;
}

} // namespace accelerator_codegen
