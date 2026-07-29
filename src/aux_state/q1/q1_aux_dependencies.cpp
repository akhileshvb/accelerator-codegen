#include "accelerator_codegen/tpch_q1_aux.hpp"
#include "accelerator_codegen/tpch_q1_incremental.hpp"
#include "accelerator_codegen/tpch_q1_maintenance.hpp"
#include "accelerator_codegen/tpch_q1_maintenance_policy.hpp"

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

const aux_state::ColumnRef *FindColumn(const aux_state::TableDependency &dependency, const std::string &name) {
	for (const auto &column : dependency.watched_columns) {
		if (column.column_name == name) {
			return &column;
		}
	}
	return nullptr;
}

std::optional<Q1PartitionKey> DayFromMutationRow(const duckdb::TableMutationRow &row,
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
	return PartitionKeyFromShipdate(picked.GetValue<duckdb::date_t>());
}

std::optional<Q1PartitionKey> DayFromEventRow(const duckdb::TableMutationEvent &event, idx_t row_idx,
                                                const aux_state::ColumnRef &shipdate_column) {
	if (row_idx >= event.rows.size()) {
		return std::nullopt;
	}
	return DayFromMutationRow(event.rows[row_idx], shipdate_column);
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
	if (!shipdate_column) {
		plan.scope = aux_state::InvalidationScope::AllPartitions;
		return plan;
	}

	switch (event.kind) {
	case duckdb::TableMutationKind::INSERT:
	case duckdb::TableMutationKind::DELETE: {
		plan.scope = aux_state::InvalidationScope::PartitionSet;
		for (idx_t row = 0; row < event.rows.size(); row++) {
			auto day = DayFromEventRow(event, row, *shipdate_column);
			if (!day) {
				plan.scope = aux_state::InvalidationScope::AllPartitions;
				plan.partitions.clear();
				return plan;
			}
			plan.partitions.insert(*day);
		}
		return plan;
	}
	case duckdb::TableMutationKind::UPDATE: {
		if (!UpdateTouchesRelevantColumns(event, dependency)) {
			return plan;
		}
		plan.scope = aux_state::InvalidationScope::PartitionSet;
		for (const auto &row : event.rows) {
			std::vector<Q1PartitionKey> shipdate_days;
			for (const auto &value : row.values) {
				if (value.column_index.index != shipdate_column->physical_index.index) {
					continue;
				}
				if (value.value.IsNull()) {
					plan.scope = aux_state::InvalidationScope::AllPartitions;
					plan.partitions.clear();
					return plan;
				}
				shipdate_days.push_back(PartitionKeyFromShipdate(value.value.GetValue<duckdb::date_t>()));
			}
			if (shipdate_days.empty()) {
				if (auto day = DayFromMutationRow(row, *shipdate_column)) {
					plan.partitions.insert(*day);
				}
				continue;
			}
			plan.partitions.insert(shipdate_days.front());
			if (shipdate_days.size() > 1) {
				plan.partitions.insert(shipdate_days.back());
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

aux_state::InvalidationPlan InterpretQ1Mutation(const duckdb::TableMutationEvent &event,
                                                const std::vector<aux_state::TableDependency> &dependencies) {
	const aux_state::TableDependency *lineitem = nullptr;
	for (const auto &dependency : dependencies) {
		if (dependency.table_name == "lineitem") {
			lineitem = &dependency;
			break;
		}
	}
	if (!lineitem || event.table_oid != lineitem->table_oid) {
		return {};
	}
	return InterpretLineitemMutation(event, *lineitem);
}

} // namespace

bool Q1AuxEnabled() {
	return ReadBoolEnv("ACCEL_Q1_AUX_STATE", true);
}

Q1PartitionKey PartitionKeyFromShipdate(duckdb::date_t shipdate) {
	return static_cast<Q1PartitionKey>(shipdate.days);
}

Q1PartitionKey PartitionKeyFromCutoff(const Q1Parameters &parameters) {
	return static_cast<Q1PartitionKey>(parameters.cutoff_date.days);
}

void RegisterQ1AuxAccelerator(duckdb::Connection &connection) {
	LoadQ1CostModelParams();
	auto &context = *connection.context;
	static const std::vector<std::string> kLineitemColumns = {
	    "l_shipdate", "l_quantity", "l_extendedprice", "l_discount",
	    "l_tax", "l_returnflag", "l_linestatus",
	};
	if (!aux_state::CanMakeTableDependency(context, "lineitem", kLineitemColumns)) {
		return;
	}

	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	registry->RegisterAcceleratorIfAbsent(kQ1AuxAcceleratorId, [&]() {
		registry->RegisterAccelerator(
		    kQ1AuxAcceleratorId,
		    [](const duckdb::TableMutationEvent &event,
		       const std::vector<aux_state::TableDependency> &dependencies) -> aux_state::InvalidationPlan {
			    return InterpretQ1Mutation(event, dependencies);
		    });
		registry->RegisterIncrementalBufferFactory(kQ1AuxAcceleratorId,
		                                           []() { return std::make_unique<Q1IncrementalTxnBuffer>(); });
		registry->RegisterMutationAccumulator(kQ1AuxAcceleratorId, AccumulateQ1Mutation);
		registry->RegisterMaintenanceHook(kQ1AuxAcceleratorId, CreateQ1MaintenanceHook());
		registry->RegisterTableDependency(
		    kQ1AuxAcceleratorId, aux_state::MakeTableDependency(context, "lineitem", kLineitemColumns));
	});
}

bool IsQ1DependentTableMutation(const duckdb::TableMutationEvent &event,
                                const std::vector<aux_state::TableDependency> &dependencies) {
	for (const auto &dependency : dependencies) {
		if (event.table_oid == dependency.table_oid) {
			return true;
		}
	}
	return false;
}

} // namespace accelerator_codegen
