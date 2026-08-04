#include "accelerator_codegen/sec_edgar_q24_aux.hpp"
#include "accelerator_codegen/sec_edgar_q24_incremental.hpp"
#include "accelerator_codegen/sec_edgar_q24_maintenance.hpp"
#include "accelerator_codegen/sec_edgar_q24_maintenance_policy.hpp"
#include "accelerator_codegen/sec_edgar_q24_finalize.hpp"

#include <cstdlib>
#include <cstring>
#include <unordered_set>

#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/aux_state/catalog_dependency.hpp"
#include "accelerator_codegen/aux_state/invalidation_plan.hpp"
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

bool UpdateTouchesRelevantColumns(const duckdb::TableMutationEvent &event,
                                  const aux_state::TableDependency &dependency,
                                  const std::unordered_set<std::string> &relevant) {
	for (const auto &updated : event.updated_columns) {
		for (const auto &column : dependency.watched_columns) {
			if (column.physical_index.index != updated.index) {
				continue;
			}
			if (relevant.count(column.column_name) > 0) {
				return true;
			}
		}
	}
	return false;
}

aux_state::InvalidationPlan InterpretNumMutation(const duckdb::TableMutationEvent &event,
                                                 const aux_state::TableDependency &dependency) {
	static const std::unordered_set<std::string> kRelevantColumns = {"adsh", "tag", "version", "uom", "value", "ddate"};
	aux_state::InvalidationPlan plan;
	switch (event.kind) {
	case duckdb::TableMutationKind::INSERT:
	case duckdb::TableMutationKind::DELETE:
		plan.scope = aux_state::InvalidationScope::PartitionSet;
		plan.partitions.insert(kSecEdgarQ24GroupAggregatesSlot);
		plan.partitions.insert(kSecEdgarQ24OutputSlot);
		return plan;
	case duckdb::TableMutationKind::UPDATE:
		if (!UpdateTouchesRelevantColumns(event, dependency, kRelevantColumns)) {
			plan.scope = aux_state::InvalidationScope::None;
			return plan;
		}
		plan.scope = aux_state::InvalidationScope::AllPartitions;
		return plan;
	default:
		break;
	}
	return plan;
}

aux_state::InvalidationPlan InterpretPreMutation(const duckdb::TableMutationEvent &event,
                                                 const aux_state::TableDependency &dependency) {
	static const std::unordered_set<std::string> kRelevantColumns = {"adsh", "tag", "version"};
	aux_state::InvalidationPlan plan;
	switch (event.kind) {
	case duckdb::TableMutationKind::INSERT:
	case duckdb::TableMutationKind::DELETE:
		plan.scope = aux_state::InvalidationScope::AllPartitions;
		return plan;
	case duckdb::TableMutationKind::UPDATE:
		if (!UpdateTouchesRelevantColumns(event, dependency, kRelevantColumns)) {
			plan.scope = aux_state::InvalidationScope::None;
			return plan;
		}
		plan.scope = aux_state::InvalidationScope::AllPartitions;
		return plan;
	default:
		break;
	}
	return plan;
}

aux_state::InvalidationPlan InterpretSecEdgarQ24MutationImpl(const duckdb::TableMutationEvent &event,
                                                            const std::vector<aux_state::TableDependency> &dependencies) {
	const aux_state::TableDependency *num = nullptr;
	const aux_state::TableDependency *pre = nullptr;
	for (const auto &dependency : dependencies) {
		if (dependency.table_name == "num") {
			num = &dependency;
		} else if (dependency.table_name == "pre") {
			pre = &dependency;
		}
	}
	if (num && event.table_oid == num->table_oid) {
		return InterpretNumMutation(event, *num);
	}
	if (pre && event.table_oid == pre->table_oid) {
		return InterpretPreMutation(event, *pre);
	}
	return {};
}

} // namespace

aux_state::InvalidationPlan InterpretSecEdgarQ24AuxTableMutation(const duckdb::TableMutationEvent &event,
                                                                 const std::vector<aux_state::TableDependency> &dependencies) {
	return InterpretSecEdgarQ24MutationImpl(event, dependencies);
}

bool SecEdgarQ24AuxEnabled() {
	return ReadBoolEnv("ACCEL_SEC_EDGAR_SecEdgarQ24_AUX_STATE", true);
}

void RegisterSecEdgarQ24AuxAccelerator(duckdb::Connection &connection) {
	LoadSecEdgarQ24CostModelParams();
	auto &context = *connection.context;
	static const std::vector<std::string> kNumColumns = {"adsh", "tag", "version", "uom", "value", "ddate"};
	static const std::vector<std::string> kPreColumns = {"adsh", "tag", "version"};
	if (!aux_state::CanMakeTableDependency(context, "num", kNumColumns) ||
	    !aux_state::CanMakeTableDependency(context, "pre", kPreColumns)) {
		return;
	}

	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	registry->RegisterAcceleratorIfAbsent(kSecEdgarQ24AuxAcceleratorId, [&]() {
		registry->RegisterAccelerator(
		    kSecEdgarQ24AuxAcceleratorId,
		    [](const duckdb::TableMutationEvent &event,
		       const std::vector<aux_state::TableDependency> &dependencies) -> aux_state::InvalidationPlan {
			    return InterpretSecEdgarQ24MutationImpl(event, dependencies);
		    });
		registry->RegisterIncrementalBufferFactory(kSecEdgarQ24AuxAcceleratorId,
		                                           []() { return std::make_unique<SecEdgarQ24IncrementalTxnBuffer>(); });
		registry->RegisterMutationAccumulator(kSecEdgarQ24AuxAcceleratorId, AccumulateSecEdgarQ24Mutation);
		registry->RegisterMaintenanceHook(kSecEdgarQ24AuxAcceleratorId, CreateSecEdgarQ24MaintenanceHook());
		registry->RegisterTableDependency(
		    kSecEdgarQ24AuxAcceleratorId, aux_state::MakeTableDependency(context, "num", kNumColumns));
		registry->RegisterTableDependency(
		    kSecEdgarQ24AuxAcceleratorId, aux_state::MakeTableDependency(context, "pre", kPreColumns));
	});
}

bool IsSecEdgarQ24DependentTableMutation(const duckdb::TableMutationEvent &event,
                                        const std::vector<aux_state::TableDependency> &dependencies) {
	for (const auto &dependency : dependencies) {
		if (event.table_oid == dependency.table_oid) {
			return true;
		}
	}
	return false;
}

} // namespace accelerator_codegen
