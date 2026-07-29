#include "accelerator_codegen/tpch_q6_rebuild_policy.hpp"

#include <chrono>

#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/aux_state/execution_load.hpp"
#include "accelerator_codegen/tpch_q6_aux.hpp"
#include "accelerator_codegen/tpch_q6_incremental.hpp"
#include "accelerator_codegen/tpch_q6_write_policy.hpp"
#include "duckdb/transaction/transaction_context.hpp"

namespace accelerator_codegen {
namespace {

struct Q6RebuildPolicyTiming {
	bool has_measurement = false;
	uint64_t last_inter_write_interval_us = 0;
	std::chrono::steady_clock::time_point last_precommit_time {};
	bool has_precommit_time = false;
};

Q6RebuildPolicyTiming g_rebuild_timing;

} // namespace

bool Q6RebuildMaintenanceEnabled() {
	return false;
}

uint64_t GetQ6PartitionRebuildCostUs() {
	return static_cast<uint64_t>(Q6PartitionRebuildUs());
}

uint64_t GetQ6FullRebuildCostUs() {
	return static_cast<uint64_t>(Q6FullRebuildUs());
}

bool HasQ6InterWriteMeasurement() {
	return g_rebuild_timing.has_measurement;
}

uint64_t GetQ6MeasuredInterWriteIntervalUs() {
	return g_rebuild_timing.last_inter_write_interval_us;
}

void RecordQ6RebuildPolicyPreCommit() {
	const auto now = std::chrono::steady_clock::now();
	if (g_rebuild_timing.has_precommit_time) {
		g_rebuild_timing.last_inter_write_interval_us = static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::microseconds>(now - g_rebuild_timing.last_precommit_time).count());
		g_rebuild_timing.has_measurement = true;
	}
	g_rebuild_timing.last_precommit_time = now;
	g_rebuild_timing.has_precommit_time = true;
}

void ResetQ6RebuildPolicyTiming() {
	g_rebuild_timing = Q6RebuildPolicyTiming {};
}

bool PrepareQ6RebuildPreCommit(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                               const aux_state::InvalidationPlan &plan, aux_state::TxnInvalidationBuffer &txn_buffer) {
	(void)context;
	(void)store;
	(void)txn_buffer;
	if (!Q6RebuildMaintenanceEnabled()) {
		return false;
	}
	if (plan.IsEmpty()) {
		return true;
	}
	RecordQ6RebuildPolicyPreCommit();
	return false;
}

void EnsurePendingQ6RebuildApplied(duckdb::ClientContext &context) {
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	auto &maintenance = registry->GetMaintenanceState(kQ6AuxAcceleratorId);
	maintenance.WithRebuildLock([&]() {
		auto claim = maintenance.BeginRebuild();
		if (!claim.HasWork()) {
			return;
		}
		const auto start = std::chrono::steady_clock::now();
		aux_state::ActiveExecutionScope rebuild_scope(aux_state::ExecutionKind::LazyRebuild, kQ6AuxAcceleratorId,
		                                              static_cast<double>(Q6PartitionRebuildUs()));
		const bool started_transaction = !context.transaction.HasActiveTransaction();
		if (started_transaction) {
			context.transaction.BeginTransaction();
		}
		try {
			auto &store = registry->GetStore(kQ6AuxAcceleratorId);
			ApplyQ6RebuildPlan(context, store, claim.captured_plan);
			maintenance.CompleteRebuild(claim, claim.captured_plan);
			if (started_transaction) {
				context.transaction.Commit();
			}
			const auto end = std::chrono::steady_clock::now();
			const auto total_us =
			    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
			uint64_t rebuild_scan_us = 0;
			if (claim.captured_plan.scope == aux_state::InvalidationScope::AllPartitions) {
				rebuild_scan_us = static_cast<uint64_t>(Q6FullRebuildUs());
			} else if (claim.captured_plan.scope == aux_state::InvalidationScope::PartitionSet) {
				rebuild_scan_us =
				    static_cast<uint64_t>(Q6PartitionRebuildUs() * claim.captured_plan.partitions.size());
			}
			RecordQ6LazyRebuildObservation(rebuild_scan_us, total_us);
		} catch (...) {
			if (started_transaction && context.transaction.HasActiveTransaction()) {
				context.transaction.Rollback(nullptr);
			}
			throw;
		}
	});
}

void ApplyQ6RebuildPlan(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                        const aux_state::InvalidationPlan &plan) {
	(void)store;
	if (plan.IsEmpty()) {
		return;
	}
	if (plan.scope == aux_state::InvalidationScope::AllPartitions) {
		BuildQ6AuxState(context);
		RecordQ6Rebuild();
		return;
	}
	if (plan.scope == aux_state::InvalidationScope::PartitionSet) {
		for (auto partition : plan.partitions) {
			RebuildQ6AuxPartition(context, partition);
		}
		RecordQ6Rebuild();
	}
}

} // namespace accelerator_codegen
