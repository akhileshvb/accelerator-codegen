#include "accelerator_codegen/tpch_q3_aux.hpp"
#include "accelerator_codegen/tpch_q3_incremental.hpp"
#include "accelerator_codegen/tpch_q3_maintenance_policy.hpp"

#include <cstdlib>
#include <cstring>
#include <optional>

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

std::optional<Q3PartitionKey> SegmentFromValue(const duckdb::Value &value) {
	if (value.IsNull()) {
		return std::nullopt;
	}
	const auto key = PartitionKeyFromMktsegment(value.GetValue<std::string>());
	if (key == 0) {
		return std::nullopt;
	}
	return key;
}

aux_state::InvalidationPlan InterpretLineitemMutation(const duckdb::TableMutationEvent &event,
                                                      const aux_state::TableDependency &dependency) {
	aux_state::InvalidationPlan plan;
	(void)event;
	(void)dependency;
	return plan;
}

aux_state::InvalidationPlan InterpretOrdersMutation(const duckdb::TableMutationEvent &event,
                                                    const aux_state::TableDependency &dependency) {
	aux_state::InvalidationPlan plan;
	(void)event;
	(void)dependency;
	return plan;
}

aux_state::InvalidationPlan InterpretCustomerMutation(const duckdb::TableMutationEvent &event,
                                                      const aux_state::TableDependency &dependency) {
	aux_state::InvalidationPlan plan;
	const auto *segment_column = FindColumn(dependency, "c_mktsegment");
	if (!segment_column) {
		plan.scope = aux_state::InvalidationScope::AllPartitions;
		return plan;
	}
	if (event.kind != duckdb::TableMutationKind::UPDATE || !UpdateTouchesRelevantColumns(event, dependency)) {
		return plan;
	}
	plan.scope = aux_state::InvalidationScope::PartitionSet;
	for (const auto &row : event.rows) {
		for (const auto &value : row.values) {
			if (value.column_index.index != segment_column->physical_index.index) {
				continue;
			}
			if (auto segment = SegmentFromValue(value.value)) {
				plan.partitions.insert(*segment);
			}
		}
	}
	if (plan.partitions.empty()) {
		plan.scope = aux_state::InvalidationScope::AllPartitions;
	}
	return plan;
}

aux_state::InvalidationPlan InterpretQ3Mutation(const duckdb::TableMutationEvent &event,
                                                  const std::vector<aux_state::TableDependency> &dependencies) {
	const aux_state::TableDependency *customer = nullptr;
	const aux_state::TableDependency *orders = nullptr;
	const aux_state::TableDependency *lineitem = nullptr;
	for (const auto &dependency : dependencies) {
		if (dependency.table_name == "customer") {
			customer = &dependency;
		} else if (dependency.table_name == "orders") {
			orders = &dependency;
		} else if (dependency.table_name == "lineitem") {
			lineitem = &dependency;
		}
	}
	if (lineitem && event.table_oid == lineitem->table_oid) {
		return InterpretLineitemMutation(event, *lineitem);
	}
	if (orders && event.table_oid == orders->table_oid) {
		return InterpretOrdersMutation(event, *orders);
	}
	if (customer && event.table_oid == customer->table_oid) {
		return InterpretCustomerMutation(event, *customer);
	}
	return {};
}

} // namespace

bool Q3AuxEnabled() {
	return ReadBoolEnv("ACCEL_Q3_AUX_STATE", true);
}

void RegisterQ3AuxAccelerator(duckdb::Connection &connection) {
	LoadQ3CostModelParams();
	auto &context = *connection.context;
	static const std::vector<std::string> kCustomerColumns = {"c_custkey", "c_mktsegment"};
	static const std::vector<std::string> kOrdersColumns = {"o_orderkey", "o_orderdate", "o_custkey", "o_orderstatus",
	                                                        "o_shippriority"};
	static const std::vector<std::string> kLineitemColumns = {"l_orderkey", "l_extendedprice", "l_discount", "l_shipdate"};
	if (!aux_state::CanMakeTableDependency(context, "customer", kCustomerColumns) ||
	    !aux_state::CanMakeTableDependency(context, "orders", kOrdersColumns) ||
	    !aux_state::CanMakeTableDependency(context, "lineitem", kLineitemColumns)) {
		return;
	}

	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	registry->RegisterAcceleratorIfAbsent(kQ3AuxAcceleratorId, [&]() {
		registry->RegisterAccelerator(
		    kQ3AuxAcceleratorId,
		    [](const duckdb::TableMutationEvent &event,
		       const std::vector<aux_state::TableDependency> &dependencies) -> aux_state::InvalidationPlan {
			    return InterpretQ3Mutation(event, dependencies);
		    });
		registry->RegisterIncrementalBufferFactory(kQ3AuxAcceleratorId,
		                                           []() { return std::make_unique<Q3IncrementalTxnBuffer>(); });
		registry->RegisterMutationAccumulator(kQ3AuxAcceleratorId, AccumulateQ3Mutation);
		registry->RegisterMaintenanceHook(kQ3AuxAcceleratorId, CreateQ3MaintenanceHook());
		registry->RegisterTableDependency(kQ3AuxAcceleratorId,
		                                  aux_state::MakeTableDependency(context, "customer", kCustomerColumns));
		registry->RegisterTableDependency(kQ3AuxAcceleratorId,
		                                  aux_state::MakeTableDependency(context, "orders", kOrdersColumns));
		registry->RegisterTableDependency(kQ3AuxAcceleratorId,
		                                  aux_state::MakeTableDependency(context, "lineitem", kLineitemColumns));
	});
}

bool IsQ3DependentTableMutation(const duckdb::TableMutationEvent &event,
                                const std::vector<aux_state::TableDependency> &dependencies) {
	for (const auto &dependency : dependencies) {
		if (event.table_oid == dependency.table_oid) {
			return true;
		}
	}
	return false;
}

} // namespace accelerator_codegen
