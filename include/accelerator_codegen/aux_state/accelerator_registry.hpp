#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "accelerator_codegen/aux_state/catalog_dependency.hpp"
#include "accelerator_codegen/aux_state/invalidation_plan.hpp"
#include "accelerator_codegen/aux_state/maintenance/maintenance_hook.hpp"
#include "accelerator_codegen/aux_state/maintenance/staged_publication.hpp"
#include "accelerator_codegen/aux_state/partitioned_state_store.hpp"
#include "duckdb/accelerator/mutation_recorder.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/object_cache.hpp"

namespace duckdb {
class Connection;
class TableCatalogEntry;
}

namespace accelerator_codegen {
class Q14IncrementalTxnBuffer;
}

namespace accelerator_codegen::aux_state {

class TxnInvalidationBuffer;

using InvalidationInterpreter =
    std::function<InvalidationPlan(const duckdb::TableMutationEvent &, const std::vector<TableDependency> &)>;

class AcceleratorStateRegistry final : public duckdb::MutationWatchProvider {
public:
	static constexpr const char *CACHE_KEY = "accel_state_registry";

	static duckdb::string ObjectType() {
		return "AcceleratorStateRegistry";
	}

	duckdb::string GetObjectType() override {
		return ObjectType();
	}
	duckdb::optional_idx GetEstimatedCacheMemory() const override {
		return duckdb::optional_idx();
	}

	void RegisterAccelerator(const std::string &accelerator_id, InvalidationInterpreter interpreter);
	void RegisterMaintenanceHook(const std::string &accelerator_id, maintenance::MaintenanceHookPtr hook);
	void RegisterTableDependency(const std::string &accelerator_id, TableDependency dependency);
	PartitionedStateStore &GetStore(const std::string &accelerator_id);
	const std::vector<TableDependency> &GetAcceleratorDependencies(const std::string &accelerator_id) const;
	void ApplyPreCommitInvalidation(const InvalidationPlan &plan, const std::string &accelerator_id);

	bool TryGetWatchedColumns(duckdb::TableCatalogEntry &table,
	                          duckdb::vector<duckdb::PhysicalIndex> &columns) override;
	bool TryGetUpdateOldColumns(duckdb::TableCatalogEntry &table,
	                            const duckdb::vector<duckdb::PhysicalIndex> &updated_columns,
	                            duckdb::vector<duckdb::PhysicalIndex> &old_columns) override;

	InvalidationPlan InterpretMutation(const duckdb::TableMutationEvent &event) const;
	void PlanMaintenancePreCommit(const InvalidationPlan &plan, duckdb::ClientContext &context,
	                                TxnInvalidationBuffer &txn_buffer);
	void PublishMaintenanceOnCommit(duckdb::ClientContext &context, TxnInvalidationBuffer &txn_buffer);
	void DiscardMaintenanceOnRollback(TxnInvalidationBuffer &txn_buffer);
	void ApplyPreCommitInvalidationAll(const InvalidationPlan &plan, duckdb::ClientContext *context = nullptr,
	                                   TxnInvalidationBuffer *txn_buffer = nullptr);

	static duckdb::shared_ptr<AcceleratorStateRegistry> GetOrCreate(duckdb::DatabaseInstance &db);

private:
	struct AcceleratorEntry {
		InvalidationInterpreter interpreter;
		maintenance::MaintenanceHookPtr maintenance_hook;
		PartitionedStateStore store;
		std::vector<TableDependency> dependencies;
	};

	std::unordered_map<std::string, AcceleratorEntry> accelerators;
	std::vector<TableDependency> all_dependencies;

	const TableDependency *FindDependency(duckdb::idx_t table_oid) const;
};

void InstallAcceleratorSubstrate(duckdb::Connection &connection);

} // namespace accelerator_codegen::aux_state
