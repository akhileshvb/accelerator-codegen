#include "accelerator_codegen/tpch_q6_aux.hpp"
#include "accelerator_codegen/tpch_q6_incremental.hpp"
#include "accelerator_codegen/tpch_q6_maintenance_policy.hpp"

#include <cstdlib>
#include <cstring>

#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/aux_state/catalog_dependency.hpp"
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

bool UpdateTouchesRelevantColumns(const duckdb::TableMutationEvent &event,
                                  const aux_state::TableDependency &dependency) {
	for (const auto &updated : event.updated_columns) {
		if (dependency.WatchesPhysicalIndex(updated.index)) {
			return true;
		}
	}
	return false;
}

std::optional<Q6PartitionKey> ShipdateFromValue(const duckdb::Value &value) {
	if (value.IsNull()) {
		return std::nullopt;
	}
	return Q6PartitionKeyFromShipdate(value.GetValue<duckdb::date_t>());
}

aux_state::InvalidationPlan InterpretLineitemMutation(const duckdb::TableMutationEvent &event,
                                                    const aux_state::TableDependency &dependency) {
	aux_state::InvalidationPlan plan;
	if (event.kind == duckdb::TableMutationKind::UPDATE && !UpdateTouchesRelevantColumns(event, dependency)) {
		return plan;
	}
	const auto *shipdate_column = FindColumn(dependency, "l_shipdate");
	if (!shipdate_column) {
		plan.scope = aux_state::InvalidationScope::AllPartitions;
		return plan;
	}
	plan.scope = aux_state::InvalidationScope::PartitionSet;
	for (const auto &row : event.rows) {
		for (const auto &value : row.values) {
			if (value.column_index.index != shipdate_column->physical_index.index) {
				continue;
			}
			if (auto partition = ShipdateFromValue(value.value)) {
				plan.partitions.insert(*partition);
			}
		}
	}
	if (event.kind == duckdb::TableMutationKind::UPDATE && UpdateTouchesRelevantColumns(event, dependency)) {
		for (const auto &row : event.rows) {
			for (const auto &value : row.values) {
				if (value.column_index.index != shipdate_column->physical_index.index) {
					continue;
				}
				if (auto partition = ShipdateFromValue(value.value)) {
					plan.partitions.insert(*partition);
				}
			}
		}
	}
	if (plan.partitions.empty() && event.kind != duckdb::TableMutationKind::INSERT) {
		plan.scope = aux_state::InvalidationScope::AllPartitions;
	}
	return plan;
}

} // namespace

bool Q6AuxEnabled() {
	return ReadBoolEnv("ACCEL_Q6_AUX_STATE", true);
}

void RegisterQ6AuxAccelerator(duckdb::Connection &connection) {
	LoadQ6CostModelParams();
	auto &context = *connection.context;
	static const std::vector<std::string> kLineitemColumns = {"l_shipdate", "l_discount", "l_quantity", "l_extendedprice"};
	if (!aux_state::CanMakeTableDependency(context, "lineitem", kLineitemColumns)) {
		return;
	}

	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	registry->RegisterAcceleratorIfAbsent(kQ6AuxAcceleratorId, [&]() {
		registry->RegisterAccelerator(
		    kQ6AuxAcceleratorId,
		    [](const duckdb::TableMutationEvent &event,
		       const std::vector<aux_state::TableDependency> &dependencies) -> aux_state::InvalidationPlan {
			    for (const auto &dependency : dependencies) {
				    if (dependency.table_name == "lineitem" && event.table_oid == dependency.table_oid) {
					    return InterpretLineitemMutation(event, dependency);
				    }
			    }
			    return {};
		    });
		registry->RegisterIncrementalBufferFactory(kQ6AuxAcceleratorId,
		                                           []() { return std::make_unique<Q6IncrementalTxnBuffer>(); });
		registry->RegisterMutationAccumulator(kQ6AuxAcceleratorId, AccumulateQ6Mutation);
		registry->RegisterMaintenanceHook(kQ6AuxAcceleratorId, CreateQ6MaintenanceHook());
		registry->RegisterTableDependency(kQ6AuxAcceleratorId,
		                                  aux_state::MakeTableDependency(context, "lineitem", kLineitemColumns));
	});
}

bool IsQ6DependentTableMutation(const duckdb::TableMutationEvent &event,
                                 const std::vector<aux_state::TableDependency> &dependencies) {
	for (const auto &dependency : dependencies) {
		if (event.table_oid == dependency.table_oid) {
			return true;
		}
	}
	return false;
}

} // namespace accelerator_codegen
