#include "accelerator_codegen/sec_edgar_q1_rebuild_policy.hpp"

#include <chrono>

#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/aux_state/execution_load.hpp"
#include "accelerator_codegen/sec_edgar_q1_aux.hpp"
#include "accelerator_codegen/sec_edgar_q1_maintenance.hpp"
#include "accelerator_codegen/sec_edgar_q1_write_policy.hpp"
#include "duckdb/transaction/transaction_context.hpp"

namespace accelerator_codegen {
namespace {

struct SecEdgarQ1RebuildPolicyTiming {
	bool has_measurement = false;
	uint64_t last_inter_write_interval_us = 0;
	std::chrono::steady_clock::time_point last_precommit_time {};
	bool has_precommit_time = false;
};

SecEdgarQ1RebuildPolicyTiming g_rebuild_timing;

aux_state::InvalidationPlan CompletedPlanFromApplied(const aux_state::InvalidationPlan &applied) {
	return applied;
}

} // namespace

bool SecEdgarQ1RebuildMaintenanceEnabled() {
	return false;
}

uint64_t GetSecEdgarQ1PartitionRebuildCostUs() {
	return static_cast<uint64_t>(SecEdgarQ1PartitionRebuildUs());
}

uint64_t GetSecEdgarQ1FullRebuildCostUs() {
	return static_cast<uint64_t>(SecEdgarQ1FullRebuildUs());
}

bool HasSecEdgarQ1InterWriteMeasurement() {
	return g_rebuild_timing.has_measurement;
}

uint64_t GetSecEdgarQ1MeasuredInterWriteIntervalUs() {
	return g_rebuild_timing.last_inter_write_interval_us;
}

void RecordSecEdgarQ1RebuildPolicyPreCommit() {
	const auto now = std::chrono::steady_clock::now();
	if (g_rebuild_timing.has_precommit_time) {
		g_rebuild_timing.last_inter_write_interval_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
		                                                                 now - g_rebuild_timing.last_precommit_time)
		                                                                 .count());
		g_rebuild_timing.has_measurement = true;
	}
	g_rebuild_timing.last_precommit_time = now;
	g_rebuild_timing.has_precommit_time = true;
}

void ResetSecEdgarQ1RebuildPolicyTiming() {
	g_rebuild_timing = SecEdgarQ1RebuildPolicyTiming {};
}

bool PrepareSecEdgarQ1RebuildPreCommit(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                                const aux_state::InvalidationPlan &plan, aux_state::TxnInvalidationBuffer &txn_buffer) {
	(void)context;
	(void)store;
	(void)txn_buffer;
	if (!SecEdgarQ1RebuildMaintenanceEnabled()) {
		return false;
	}
	if (plan.IsEmpty()) {
		return true;
	}

	RecordSecEdgarQ1RebuildPolicyPreCommit();

	const uint64_t rebuild_cost_us = plan.scope == aux_state::InvalidationScope::AllPartitions
	                                     ? GetSecEdgarQ1FullRebuildCostUs()
	                                     : GetSecEdgarQ1PartitionRebuildCostUs() * static_cast<uint64_t>(plan.partitions.size());
	if (g_rebuild_timing.has_measurement && rebuild_cost_us >= g_rebuild_timing.last_inter_write_interval_us) {
		RecordSecEdgarQ1RebuildCostModelSkip();
		return false;
	}

	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	registry->GetMaintenanceState(kSecEdgarQ1AuxAcceleratorId).MergeGlobalPendingLazyRebuild(plan);
	return false;
}

void EnsurePendingSecEdgarQ1RebuildApplied(duckdb::ClientContext &context) {
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	auto &maintenance = registry->GetMaintenanceState(kSecEdgarQ1AuxAcceleratorId);
	maintenance.WithRebuildLock([&]() {
		auto claim = maintenance.BeginRebuild();
		if (!claim.HasWork()) {
			return;
		}

		const auto start = std::chrono::steady_clock::now();
		aux_state::ActiveExecutionScope rebuild_scope(aux_state::ExecutionKind::LazyRebuild, kSecEdgarQ1AuxAcceleratorId,
		                                              static_cast<double>(SecEdgarQ1PartitionRebuildUs()));
		const bool started_transaction = !context.transaction.HasActiveTransaction();
		if (started_transaction) {
			context.transaction.BeginTransaction();
		}
		try {
			auto &store = registry->GetStore(kSecEdgarQ1AuxAcceleratorId);
			ApplySecEdgarQ1RebuildPlan(context, store, claim.captured_plan);
			maintenance.CompleteRebuild(claim, CompletedPlanFromApplied(claim.captured_plan));
			if (started_transaction) {
				context.transaction.Commit();
			}
			const auto end = std::chrono::steady_clock::now();
			const auto total_us = static_cast<uint64_t>(
			    std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
			uint64_t rebuild_scan_us = 0;
			if (claim.captured_plan.scope == aux_state::InvalidationScope::AllPartitions) {
				rebuild_scan_us = static_cast<uint64_t>(SecEdgarQ1FullRebuildUs());
			} else if (claim.captured_plan.scope == aux_state::InvalidationScope::PartitionSet) {
				rebuild_scan_us =
				    static_cast<uint64_t>(SecEdgarQ1PartitionRebuildUs() * claim.captured_plan.partitions.size());
			}
			RecordSecEdgarQ1LazyRebuildObservation(rebuild_scan_us, total_us);
		} catch (...) {
			if (started_transaction && context.transaction.HasActiveTransaction()) {
				context.transaction.Rollback(nullptr);
			}
			throw;
		}
	});
}

void ApplySecEdgarQ1RebuildPlan(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                         const aux_state::InvalidationPlan &plan) {
	(void)store;
	if (plan.IsEmpty()) {
		return;
	}

	if (plan.scope == aux_state::InvalidationScope::AllPartitions) {
		BuildSecEdgarQ1AuxState(context);
		RecordSecEdgarQ1Rebuild();
		return;
	}
	if (plan.scope == aux_state::InvalidationScope::PartitionSet) {
		for (auto partition : plan.partitions) {
			RebuildSecEdgarQ1AuxPartition(context, partition);
		}
		RecordSecEdgarQ1Rebuild();
	}
}

} // namespace accelerator_codegen
