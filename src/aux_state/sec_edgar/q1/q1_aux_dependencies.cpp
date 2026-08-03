#include "accelerator_codegen/sec_edgar_q1_aux.hpp"
#include "accelerator_codegen/sec_edgar_q1_incremental.hpp"
#include "accelerator_codegen/sec_edgar_q1_maintenance.hpp"
#include "accelerator_codegen/sec_edgar_q1_maintenance_policy.hpp"

#include <cstdlib>
#include <cstring>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/aux_state/catalog_dependency.hpp"
#include "accelerator_codegen/aux_state/invalidation_plan.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
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

std::optional<std::string> PickStringColumn(const duckdb::TableMutationRow &row, const aux_state::ColumnRef &column,
                                            bool use_new_value) {
	std::vector<duckdb::Value> values;
	for (const auto &projected : row.values) {
		if (projected.column_index.index == column.physical_index.index) {
			values.push_back(projected.value);
		}
	}
	if (values.empty()) {
		return std::nullopt;
	}
	const auto &picked = (values.size() == 1 || !use_new_value) ? values.front() : values.back();
	if (picked.IsNull()) {
		return std::nullopt;
	}
	return picked.GetValue<std::string>();
}

std::optional<SecEdgarQ1PartitionKey> PartitionFromMutationRow(const duckdb::TableMutationRow &row,
                                                               const aux_state::TableDependency &dependency,
                                                               bool use_new_value) {
	const auto *stmt_column = FindColumn(dependency, "stmt");
	const auto *rfile_column = FindColumn(dependency, "rfile");
	if (!stmt_column) {
		return std::nullopt;
	}
	auto stmt = PickStringColumn(row, *stmt_column, use_new_value);
	if (!stmt) {
		return std::nullopt;
	}
	std::optional<std::string> rfile;
	bool rfile_is_null = true;
	if (rfile_column) {
		rfile = PickStringColumn(row, *rfile_column, use_new_value);
		rfile_is_null = !rfile.has_value();
	}
	return PartitionKeyFromStmtRfileStrings(*stmt, rfile.value_or(std::string()), rfile_is_null);
}

bool UpdateTouchesRelevantColumns(const duckdb::TableMutationEvent &event,
                                  const aux_state::TableDependency &dependency) {
	static const std::unordered_set<std::string> kRelevantColumns = {"stmt", "rfile", "line", "adsh"};
	for (const auto &updated : event.updated_columns) {
		for (const auto &column : dependency.watched_columns) {
			if (column.physical_index.index != updated.index) {
				continue;
			}
			if (kRelevantColumns.count(column.column_name) > 0) {
				return true;
			}
		}
	}
	return false;
}

aux_state::InvalidationPlan InterpretPreMutation(const duckdb::TableMutationEvent &event,
                                                 const aux_state::TableDependency &dependency) {
	aux_state::InvalidationPlan plan;
	const auto *stmt_column = FindColumn(dependency, "stmt");
	if (!stmt_column) {
		plan.scope = aux_state::InvalidationScope::AllPartitions;
		return plan;
	}

	switch (event.kind) {
	case duckdb::TableMutationKind::INSERT:
	case duckdb::TableMutationKind::DELETE: {
		plan.scope = aux_state::InvalidationScope::PartitionSet;
		for (const auto &row : event.rows) {
			auto partition = PartitionFromMutationRow(row, dependency, true);
			if (!partition) {
				continue;
			}
			plan.partitions.insert(*partition);
		}
		if (plan.partitions.empty() && event.kind == duckdb::TableMutationKind::INSERT) {
			plan.scope = aux_state::InvalidationScope::None;
			plan.partitions.clear();
		}
		return plan;
	}
	case duckdb::TableMutationKind::UPDATE: {
		if (!UpdateTouchesRelevantColumns(event, dependency)) {
			plan.scope = aux_state::InvalidationScope::None;
			return plan;
		}
		plan.scope = aux_state::InvalidationScope::PartitionSet;
		for (const auto &row : event.rows) {
			if (auto old_partition = PartitionFromMutationRow(row, dependency, false)) {
				plan.partitions.insert(*old_partition);
			}
			if (auto new_partition = PartitionFromMutationRow(row, dependency, true)) {
				plan.partitions.insert(*new_partition);
			}
		}
		return plan;
	}
	default:
		break;
	}
	return plan;
}

aux_state::InvalidationPlan InterpretSecEdgarQ1Mutation(const duckdb::TableMutationEvent &event,
                                                        const std::vector<aux_state::TableDependency> &dependencies) {
	const aux_state::TableDependency *pre = nullptr;
	for (const auto &dependency : dependencies) {
		if (dependency.table_name == "pre") {
			pre = &dependency;
			break;
		}
	}
	if (!pre || event.table_oid != pre->table_oid) {
		return {};
	}
	return InterpretPreMutation(event, *pre);
}

} // namespace

bool SecEdgarQ1AuxEnabled() {
	return ReadBoolEnv("ACCEL_SEC_EDGAR_SecEdgarQ1_AUX_STATE", true);
}

void RegisterSecEdgarQ1AuxAccelerator(duckdb::Connection &connection) {
	LoadSecEdgarQ1CostModelParams();
	auto &context = *connection.context;
	static const std::vector<std::string> kPreColumns = {"stmt", "rfile", "line", "adsh"};
	if (!aux_state::CanMakeTableDependency(context, "pre", kPreColumns)) {
		return;
	}

	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	registry->RegisterAcceleratorIfAbsent(kSecEdgarQ1AuxAcceleratorId, [&]() {
		registry->RegisterAccelerator(
		    kSecEdgarQ1AuxAcceleratorId,
		    [](const duckdb::TableMutationEvent &event,
		       const std::vector<aux_state::TableDependency> &dependencies) -> aux_state::InvalidationPlan {
			    return InterpretSecEdgarQ1Mutation(event, dependencies);
		    });
		registry->RegisterIncrementalBufferFactory(kSecEdgarQ1AuxAcceleratorId,
		                                           []() { return std::make_unique<SecEdgarQ1IncrementalTxnBuffer>(); });
		registry->RegisterMutationAccumulator(kSecEdgarQ1AuxAcceleratorId, AccumulateSecEdgarQ1Mutation);
		registry->RegisterMaintenanceHook(kSecEdgarQ1AuxAcceleratorId, CreateSecEdgarQ1MaintenanceHook());
		registry->RegisterTableDependency(
		    kSecEdgarQ1AuxAcceleratorId, aux_state::MakeTableDependency(context, "pre", kPreColumns));
	});
}

bool IsSecEdgarQ1DependentTableMutation(const duckdb::TableMutationEvent &event,
                                        const std::vector<aux_state::TableDependency> &dependencies) {
	for (const auto &dependency : dependencies) {
		if (event.table_oid == dependency.table_oid) {
			return true;
		}
	}
	return false;
}

} // namespace accelerator_codegen
