#include "accelerator_codegen/tpch_q12_aux.hpp"
#include "accelerator_codegen/tpch_q12_incremental.hpp"
#include "accelerator_codegen/tpch_q12_maintenance_policy.hpp"

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

std::optional<Q12PartitionKey> ReceiptdateFromValue(const duckdb::Value &value) {
	if (value.IsNull()) {
		return std::nullopt;
	}
	return PartitionKeyFromReceiptdate(value.GetValue<duckdb::date_t>());
}

aux_state::InvalidationPlan InterpretLineitemMutation(const duckdb::TableMutationEvent &event,
                                                    const aux_state::TableDependency &dependency) {
	aux_state::InvalidationPlan plan;
	if (event.kind == duckdb::TableMutationKind::UPDATE && !UpdateTouchesRelevantColumns(event, dependency)) {
		return plan;
	}
	const auto *receipt_column = FindColumn(dependency, "l_receiptdate");
	if (!receipt_column) {
		plan.scope = aux_state::InvalidationScope::AllPartitions;
		return plan;
	}
	plan.scope = aux_state::InvalidationScope::PartitionSet;
	for (const auto &row : event.rows) {
		for (const auto &value : row.values) {
			if (value.column_index.index != receipt_column->physical_index.index) {
				continue;
			}
			if (auto partition = ReceiptdateFromValue(value.value)) {
				plan.partitions.insert(*partition);
			}
		}
	}
	if (event.kind == duckdb::TableMutationKind::UPDATE && UpdateTouchesRelevantColumns(event, dependency)) {
		for (const auto &row : event.rows) {
			for (const auto &value : row.values) {
				if (value.column_index.index != receipt_column->physical_index.index) {
					continue;
				}
				if (auto partition = ReceiptdateFromValue(value.value)) {
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

aux_state::InvalidationPlan InterpretOrdersMutation(const duckdb::TableMutationEvent &event,
                                                    const aux_state::TableDependency &dependency) {
	aux_state::InvalidationPlan plan;
	const auto *priority_column = FindColumn(dependency, "o_orderpriority");
	if (!priority_column) {
		return plan;
	}
	if (event.kind == duckdb::TableMutationKind::UPDATE && UpdateTouchesRelevantColumns(event, dependency)) {
		plan.scope = aux_state::InvalidationScope::AllPartitions;
	}
	return plan;
}

aux_state::InvalidationPlan InterpretQ12Mutation(const duckdb::TableMutationEvent &event,
                                                 const std::vector<aux_state::TableDependency> &dependencies) {
	const aux_state::TableDependency *orders = nullptr;
	const aux_state::TableDependency *lineitem = nullptr;
	for (const auto &dependency : dependencies) {
		if (dependency.table_name == "orders") {
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
	return {};
}

} // namespace

bool Q12AuxEnabled() {
	return ReadBoolEnv("ACCEL_Q12_AUX_STATE", true);
}

void RegisterQ12AuxAccelerator(duckdb::Connection &connection) {
	LoadQ12CostModelParams();
	auto &context = *connection.context;
	static const std::vector<std::string> kOrdersColumns = {"o_orderkey", "o_orderpriority"};
	static const std::vector<std::string> kLineitemColumns = {"l_orderkey", "l_shipmode", "l_commitdate", "l_receiptdate",
	                                                          "l_shipdate"};
	if (!aux_state::CanMakeTableDependency(context, "orders", kOrdersColumns) ||
	    !aux_state::CanMakeTableDependency(context, "lineitem", kLineitemColumns)) {
		return;
	}

	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	registry->RegisterAcceleratorIfAbsent(kQ12AuxAcceleratorId, [&]() {
		registry->RegisterAccelerator(
		    kQ12AuxAcceleratorId,
		    [](const duckdb::TableMutationEvent &event,
		       const std::vector<aux_state::TableDependency> &dependencies) -> aux_state::InvalidationPlan {
			    return InterpretQ12Mutation(event, dependencies);
		    });
		registry->RegisterIncrementalBufferFactory(kQ12AuxAcceleratorId,
		                                           []() { return std::make_unique<Q12IncrementalTxnBuffer>(); });
		registry->RegisterMutationAccumulator(kQ12AuxAcceleratorId, AccumulateQ12Mutation);
		registry->RegisterMaintenanceHook(kQ12AuxAcceleratorId, CreateQ12MaintenanceHook());
		registry->RegisterTableDependency(kQ12AuxAcceleratorId,
		                                  aux_state::MakeTableDependency(context, "orders", kOrdersColumns));
		registry->RegisterTableDependency(kQ12AuxAcceleratorId,
		                                  aux_state::MakeTableDependency(context, "lineitem", kLineitemColumns));
	});
}

bool IsQ12DependentTableMutation(const duckdb::TableMutationEvent &event,
                                 const std::vector<aux_state::TableDependency> &dependencies) {
	for (const auto &dependency : dependencies) {
		if (event.table_oid == dependency.table_oid) {
			return true;
		}
	}
	return false;
}

} // namespace accelerator_codegen
