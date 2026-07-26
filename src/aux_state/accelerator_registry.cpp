#include "accelerator_codegen/aux_state/accelerator_registry.hpp"

#include <algorithm>

#include "accelerator_codegen/tpch_q14_write_policy.hpp"

#include "accelerator_codegen/aux_state/txn_invalidation_buffer.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/planner/extension_callback.hpp"

namespace accelerator_codegen::aux_state {
namespace {

class AcceleratorConnectionCallback final : public duckdb::ExtensionCallback {
public:
	void OnConnectionOpened(duckdb::ClientContext &context) override {
		GetOrCreateTxnInvalidationBuffer(context);
	}
};

void EnsureConnectionCallback(duckdb::DatabaseInstance &db) {
	static std::once_flag once;
	std::call_once(once, [&db]() {
		duckdb::ExtensionCallback::Register(db.config, duckdb::make_shared_ptr<AcceleratorConnectionCallback>());
	});
}

} // namespace

void AcceleratorStateRegistry::RegisterAccelerator(const std::string &accelerator_id,
                                                   InvalidationInterpreter interpreter) {
	accelerators[accelerator_id].interpreter = std::move(interpreter);
}

void AcceleratorStateRegistry::RegisterMaintenanceHook(const std::string &accelerator_id,
                                                       maintenance::MaintenanceHookPtr hook) {
	accelerators[accelerator_id].maintenance_hook = std::move(hook);
}

void AcceleratorStateRegistry::RegisterTableDependency(const std::string &accelerator_id, TableDependency dependency) {
	accelerators[accelerator_id].dependencies.push_back(dependency);
	all_dependencies.push_back(std::move(dependency));
}

PartitionedStateStore &AcceleratorStateRegistry::GetStore(const std::string &accelerator_id) {
	return accelerators[accelerator_id].store;
}

const std::vector<TableDependency> &AcceleratorStateRegistry::GetAcceleratorDependencies(
    const std::string &accelerator_id) const {
	return accelerators.at(accelerator_id).dependencies;
}

const TableDependency *AcceleratorStateRegistry::FindDependency(duckdb::idx_t table_oid) const {
	for (const auto &dependency : all_dependencies) {
		if (dependency.table_oid == table_oid) {
			return &dependency;
		}
	}
	return nullptr;
}

InvalidationPlan AcceleratorStateRegistry::InterpretMutation(const duckdb::TableMutationEvent &event) const {
	InvalidationPlan merged;
	for (const auto &entry : accelerators) {
		if (!entry.second.interpreter) {
			continue;
		}
		MergeInvalidationPlan(merged, entry.second.interpreter(event, entry.second.dependencies));
	}
	return merged;
}

void AcceleratorStateRegistry::ApplyPreCommitInvalidation(const InvalidationPlan &plan,
                                                          const std::string &accelerator_id) {
	auto it = accelerators.find(accelerator_id);
	if (it == accelerators.end()) {
		return;
	}
	auto &store = it->second.store;
	if (plan.scope == InvalidationScope::AllPartitions) {
		store.InvalidateAll();
		if (accelerator_id == "tpch_q14_aux") {
			accelerator_codegen::RecordQ14InvalidationApplied(true, 0);
		}
		return;
	}
	uint64_t partition_count = 0;
	for (auto partition : plan.partitions) {
		store.InvalidatePartition(partition);
		partition_count++;
	}
	if (accelerator_id == "tpch_q14_aux" && partition_count > 0) {
		accelerator_codegen::RecordQ14InvalidationApplied(false, partition_count);
	}
}

void AcceleratorStateRegistry::PlanMaintenancePreCommit(const InvalidationPlan &plan, duckdb::ClientContext &context,
                                                        TxnInvalidationBuffer &txn_buffer) {
	for (auto &entry : accelerators) {
		if (!entry.second.maintenance_hook) {
			continue;
		}
		auto &staged = txn_buffer.GetOrCreateStagedOutcome(entry.first);
		entry.second.maintenance_hook->PlanMaintenance(context, entry.second.store, plan, txn_buffer, staged);
	}
}

void AcceleratorStateRegistry::PublishMaintenanceOnCommit(duckdb::ClientContext &context,
                                                          TxnInvalidationBuffer &txn_buffer) {
	for (auto &entry : accelerators) {
		if (!entry.second.maintenance_hook) {
			continue;
		}
		auto &staged = txn_buffer.GetStagedOutcome(entry.first);
		if (!staged.active) {
			continue;
		}
		entry.second.maintenance_hook->PublishMaintenance(context, entry.second.store, txn_buffer, staged);
	}
}

void AcceleratorStateRegistry::DiscardMaintenanceOnRollback(TxnInvalidationBuffer &txn_buffer) {
	for (auto &entry : accelerators) {
		if (!entry.second.maintenance_hook) {
			continue;
		}
		auto &staged = txn_buffer.GetStagedOutcome(entry.first);
		if (!staged.active) {
			continue;
		}
		entry.second.maintenance_hook->DiscardMaintenance(staged);
	}
}

void AcceleratorStateRegistry::ApplyPreCommitInvalidationAll(const InvalidationPlan &plan,
                                                           duckdb::ClientContext *context,
                                                           TxnInvalidationBuffer *txn_buffer) {
	if (context && txn_buffer) {
		PlanMaintenancePreCommit(plan, *context, *txn_buffer);
	}
	for (auto &entry : accelerators) {
		if (context && txn_buffer && entry.second.maintenance_hook) {
			continue;
		}
		ApplyPreCommitInvalidation(plan, entry.first);
	}
}

bool AcceleratorStateRegistry::TryGetWatchedColumns(duckdb::TableCatalogEntry &table,
                                                    duckdb::vector<duckdb::PhysicalIndex> &columns) {
	const auto *dependency = FindDependency(table.oid);
	if (!dependency) {
		return false;
	}
	columns.clear();
	for (const auto &column : dependency->watched_columns) {
		columns.emplace_back(column.physical_index);
	}
	return !columns.empty();
}

bool AcceleratorStateRegistry::TryGetUpdateOldColumns(
    duckdb::TableCatalogEntry &table, const duckdb::vector<duckdb::PhysicalIndex> &updated_columns,
    duckdb::vector<duckdb::PhysicalIndex> &old_columns) {
	const auto *dependency = FindDependency(table.oid);
	if (!dependency) {
		return false;
	}
	bool touches_watched = false;
	for (const auto &updated : updated_columns) {
		if (dependency->WatchesPhysicalIndex(updated.index)) {
			touches_watched = true;
			break;
		}
	}
	if (!touches_watched) {
		old_columns.clear();
		return true;
	}
	for (const auto &updated : updated_columns) {
		if (dependency->WatchesPhysicalIndex(updated.index)) {
			old_columns.emplace_back(updated);
		}
	}
	if (!old_columns.empty() && (dependency->table_name == "lineitem" || dependency->table_name == "part")) {
		for (const auto &column : dependency->watched_columns) {
			const bool already_present =
			    std::any_of(old_columns.begin(), old_columns.end(), [&](const duckdb::PhysicalIndex &existing) {
				    return existing.index == column.physical_index.index;
			    });
			if (!already_present) {
				old_columns.emplace_back(column.physical_index);
			}
		}
	}
	return !old_columns.empty();
}

duckdb::shared_ptr<AcceleratorStateRegistry> AcceleratorStateRegistry::GetOrCreate(duckdb::DatabaseInstance &db) {
	auto &cache = db.GetObjectCache();
	auto existing = cache.Get<AcceleratorStateRegistry>(CACHE_KEY);
	if (existing) {
		return existing;
	}
	auto created = duckdb::make_shared_ptr<AcceleratorStateRegistry>();
	cache.Put(CACHE_KEY, created);
	return created;
}

void InstallAcceleratorSubstrate(duckdb::Connection &connection) {
	auto &db = *connection.context->db;
	EnsureConnectionCallback(db);
	auto registry = AcceleratorStateRegistry::GetOrCreate(db);
	auto txn_buffer = GetOrCreateTxnInvalidationBuffer(*connection.context);
	txn_buffer->SetRegistry(registry.get());
}

} // namespace accelerator_codegen::aux_state
