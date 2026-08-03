#include "accelerator_codegen/sec_edgar_q3_aux.hpp"
#include "accelerator_codegen/sec_edgar_q3_incremental.hpp"
#include "accelerator_codegen/sec_edgar_q3_maintenance.hpp"
#include "accelerator_codegen/sec_edgar_q3_maintenance_policy.hpp"
#include "accelerator_codegen/sec_edgar_q3_finalize.hpp"

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
	static const std::unordered_set<std::string> kRelevantColumns = {"adsh", "uom", "value"};
	aux_state::InvalidationPlan plan;
	switch (event.kind) {
	case duckdb::TableMutationKind::INSERT:
	case duckdb::TableMutationKind::DELETE:
		plan.scope = aux_state::InvalidationScope::PartitionSet;
		plan.partitions.insert(kSecEdgarQ3CikTotalsSlot);
		plan.partitions.insert(kSecEdgarQ3OutputSlot);
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

aux_state::InvalidationPlan InterpretSubMutation(const duckdb::TableMutationEvent &event,
                                                 const aux_state::TableDependency &dependency) {
	static const std::unordered_set<std::string> kRelevantColumns = {"adsh", "name", "cik", "fy"};
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

aux_state::InvalidationPlan InterpretSecEdgarQ3Mutation(const duckdb::TableMutationEvent &event,
                                                        const std::vector<aux_state::TableDependency> &dependencies) {
	const aux_state::TableDependency *num = nullptr;
	const aux_state::TableDependency *sub = nullptr;
	for (const auto &dependency : dependencies) {
		if (dependency.table_name == "num") {
			num = &dependency;
		} else if (dependency.table_name == "sub") {
			sub = &dependency;
		}
	}
	if (num && event.table_oid == num->table_oid) {
		return InterpretNumMutation(event, *num);
	}
	if (sub && event.table_oid == sub->table_oid) {
		return InterpretSubMutation(event, *sub);
	}
	return {};
}

} // namespace

bool SecEdgarQ3AuxEnabled() {
	return ReadBoolEnv("ACCEL_SEC_EDGAR_SecEdgarQ3_AUX_STATE", true);
}

void RegisterSecEdgarQ3AuxAccelerator(duckdb::Connection &connection) {
	LoadSecEdgarQ3CostModelParams();
	auto &context = *connection.context;
	static const std::vector<std::string> kNumColumns = {"adsh", "tag", "version", "uom", "value"};
	static const std::vector<std::string> kSubColumns = {"adsh", "name", "cik", "fy"};
	if (!aux_state::CanMakeTableDependency(context, "num", kNumColumns) ||
	    !aux_state::CanMakeTableDependency(context, "sub", kSubColumns)) {
		return;
	}

	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	registry->RegisterAcceleratorIfAbsent(kSecEdgarQ3AuxAcceleratorId, [&]() {
		registry->RegisterAccelerator(
		    kSecEdgarQ3AuxAcceleratorId,
		    [](const duckdb::TableMutationEvent &event,
		       const std::vector<aux_state::TableDependency> &dependencies) -> aux_state::InvalidationPlan {
			    return InterpretSecEdgarQ3Mutation(event, dependencies);
		    });
		registry->RegisterIncrementalBufferFactory(kSecEdgarQ3AuxAcceleratorId,
		                                           []() { return std::make_unique<SecEdgarQ3IncrementalTxnBuffer>(); });
		registry->RegisterMutationAccumulator(kSecEdgarQ3AuxAcceleratorId, AccumulateSecEdgarQ3Mutation);
		registry->RegisterMaintenanceHook(kSecEdgarQ3AuxAcceleratorId, CreateSecEdgarQ3MaintenanceHook());
		registry->RegisterTableDependency(
		    kSecEdgarQ3AuxAcceleratorId, aux_state::MakeTableDependency(context, "num", kNumColumns));
		registry->RegisterTableDependency(
		    kSecEdgarQ3AuxAcceleratorId, aux_state::MakeTableDependency(context, "sub", kSubColumns));
	});
}

bool IsSecEdgarQ3DependentTableMutation(const duckdb::TableMutationEvent &event,
                                        const std::vector<aux_state::TableDependency> &dependencies) {
	for (const auto &dependency : dependencies) {
		if (event.table_oid == dependency.table_oid) {
			return true;
		}
	}
	return false;
}

} // namespace accelerator_codegen
