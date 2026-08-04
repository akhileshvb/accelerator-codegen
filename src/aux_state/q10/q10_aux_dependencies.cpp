#include "accelerator_codegen/tpch_q10_aux.hpp"
#include "accelerator_codegen/tpch_q10_incremental.hpp"
#include "accelerator_codegen/tpch_q10_maintenance_policy.hpp"

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

std::optional<Q10PartitionKey> OrderdateFromValue(const duckdb::Value &value) {
	if (value.IsNull()) {
		return std::nullopt;
	}
	return PartitionKeyFromOrderdate(value.GetValue<duckdb::date_t>());
}

std::optional<Q10PartitionKey> MonthFromMutationRow(const duckdb::TableMutationRow &row,
                                                    const aux_state::ColumnRef &orderdate_column) {
	std::vector<duckdb::Value> values;
	for (const auto &projected : row.values) {
		if (projected.column_index.index == orderdate_column.physical_index.index) {
			values.push_back(projected.value);
		}
	}
	if (values.empty()) {
		return std::nullopt;
	}
	const auto &picked = values.size() == 1 ? values.front() : values.back();
	return OrderdateFromValue(picked);
}

aux_state::InvalidationPlan InterpretLineitemMutation(const duckdb::TableMutationEvent &event,
                                                    const aux_state::TableDependency &dependency) {
	aux_state::InvalidationPlan plan;
	const auto *orderkey_column = FindColumn(dependency, "l_orderkey");
	const auto *price_column = FindColumn(dependency, "l_extendedprice");
	const auto *discount_column = FindColumn(dependency, "l_discount");
	const auto *returnflag_column = FindColumn(dependency, "l_returnflag");
	if (!orderkey_column || !price_column || !discount_column || !returnflag_column) {
		plan.scope = aux_state::InvalidationScope::AllPartitions;
		return plan;
	}
	if (event.kind == duckdb::TableMutationKind::UPDATE && !UpdateTouchesRelevantColumns(event, dependency)) {
		return plan;
	}
	return plan;
}

aux_state::InvalidationPlan InterpretOrdersMutation(const duckdb::TableMutationEvent &event,
                                                    const aux_state::TableDependency &dependency) {
	aux_state::InvalidationPlan plan;
	const auto *orderdate_column = FindColumn(dependency, "o_orderdate");
	const auto *custkey_column = FindColumn(dependency, "o_custkey");
	if (!orderdate_column || !custkey_column) {
		plan.scope = aux_state::InvalidationScope::AllPartitions;
		return plan;
	}
	if (event.kind == duckdb::TableMutationKind::UPDATE && UpdateTouchesRelevantColumns(event, dependency)) {
		plan.scope = aux_state::InvalidationScope::PartitionSet;
		for (const auto &row : event.rows) {
			if (auto month = MonthFromMutationRow(row, *orderdate_column)) {
				plan.partitions.insert(*month);
			}
			for (const auto &value : row.values) {
				if (value.column_index.index == orderdate_column->physical_index.index) {
					if (auto month = OrderdateFromValue(value.value)) {
						plan.partitions.insert(*month);
					}
				}
			}
		}
		if (plan.partitions.empty()) {
			plan.scope = aux_state::InvalidationScope::AllPartitions;
		}
	}
	return plan;
}

aux_state::InvalidationPlan InterpretCustomerMutation(const duckdb::TableMutationEvent &event,
                                                      const aux_state::TableDependency &dependency) {
	aux_state::InvalidationPlan plan;
	if (event.kind == duckdb::TableMutationKind::UPDATE && UpdateTouchesRelevantColumns(event, dependency)) {
		const auto *custkey_column = FindColumn(dependency, "c_custkey");
		if (custkey_column) {
			for (const auto &row : event.rows) {
				for (const auto &value : row.values) {
					if (value.column_index.index == custkey_column->physical_index.index) {
						plan.scope = aux_state::InvalidationScope::AllPartitions;
						plan.partitions.clear();
						return plan;
					}
				}
			}
		}
	}
	return plan;
}

aux_state::InvalidationPlan InterpretNationMutation(const duckdb::TableMutationEvent &event,
                                                    const aux_state::TableDependency &dependency) {
	aux_state::InvalidationPlan plan;
	(void)event;
	(void)dependency;
	return plan;
}

aux_state::InvalidationPlan InterpretQ10MutationImpl(const duckdb::TableMutationEvent &event,
                                                     const std::vector<aux_state::TableDependency> &dependencies) {
	const aux_state::TableDependency *customer = nullptr;
	const aux_state::TableDependency *orders = nullptr;
	const aux_state::TableDependency *lineitem = nullptr;
	const aux_state::TableDependency *nation = nullptr;
	for (const auto &dependency : dependencies) {
		if (dependency.table_name == "customer") {
			customer = &dependency;
		} else if (dependency.table_name == "orders") {
			orders = &dependency;
		} else if (dependency.table_name == "lineitem") {
			lineitem = &dependency;
		} else if (dependency.table_name == "nation") {
			nation = &dependency;
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
	if (nation && event.table_oid == nation->table_oid) {
		return InterpretNationMutation(event, *nation);
	}
	return {};
}

} // namespace

aux_state::InvalidationPlan InterpretQ10AuxTableMutation(const duckdb::TableMutationEvent &event,
                                                         const std::vector<aux_state::TableDependency> &dependencies) {
	return InterpretQ10MutationImpl(event, dependencies);
}

bool Q10AuxEnabled() {
	return ReadBoolEnv("ACCEL_Q10_AUX_STATE", true);
}

void RegisterQ10AuxAccelerator(duckdb::Connection &connection) {
	LoadQ10CostModelParams();
	auto &context = *connection.context;
	static const std::vector<std::string> kCustomerColumns = {"c_custkey", "c_name", "c_acctbal", "c_nationkey",
	                                                          "c_address", "c_phone", "c_comment"};
	static const std::vector<std::string> kOrdersColumns = {"o_orderkey", "o_custkey", "o_orderdate"};
	static const std::vector<std::string> kLineitemColumns = {"l_orderkey", "l_extendedprice", "l_discount",
	                                                            "l_returnflag"};
	static const std::vector<std::string> kNationColumns = {"n_nationkey", "n_name"};
	if (!aux_state::CanMakeTableDependency(context, "customer", kCustomerColumns) ||
	    !aux_state::CanMakeTableDependency(context, "orders", kOrdersColumns) ||
	    !aux_state::CanMakeTableDependency(context, "lineitem", kLineitemColumns) ||
	    !aux_state::CanMakeTableDependency(context, "nation", kNationColumns)) {
		return;
	}

	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	registry->RegisterAcceleratorIfAbsent(kQ10AuxAcceleratorId, [&]() {
		registry->RegisterAccelerator(
		    kQ10AuxAcceleratorId,
		    [](const duckdb::TableMutationEvent &event,
		       const std::vector<aux_state::TableDependency> &dependencies) -> aux_state::InvalidationPlan {
			    return InterpretQ10MutationImpl(event, dependencies);
		    });
		registry->RegisterIncrementalBufferFactory(kQ10AuxAcceleratorId,
		                                           []() { return std::make_unique<Q10IncrementalTxnBuffer>(); });
		registry->RegisterMutationAccumulator(kQ10AuxAcceleratorId, AccumulateQ10Mutation);
		registry->RegisterMaintenanceHook(kQ10AuxAcceleratorId, CreateQ10MaintenanceHook());
		registry->RegisterTableDependency(kQ10AuxAcceleratorId,
		                                  aux_state::MakeTableDependency(context, "customer", kCustomerColumns));
		registry->RegisterTableDependency(kQ10AuxAcceleratorId,
		                                  aux_state::MakeTableDependency(context, "orders", kOrdersColumns));
		registry->RegisterTableDependency(kQ10AuxAcceleratorId,
		                                  aux_state::MakeTableDependency(context, "lineitem", kLineitemColumns));
		registry->RegisterTableDependency(kQ10AuxAcceleratorId,
		                                  aux_state::MakeTableDependency(context, "nation", kNationColumns));
	});
}

bool IsQ10DependentTableMutation(const duckdb::TableMutationEvent &event,
                                 const std::vector<aux_state::TableDependency> &dependencies) {
	for (const auto &dependency : dependencies) {
		if (event.table_oid == dependency.table_oid) {
			return true;
		}
	}
	return false;
}

} // namespace accelerator_codegen
