#include "accelerator_codegen/tpch_q1_rebuild_policy.hpp"

#include <chrono>

#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/aux_state/txn_invalidation_buffer.hpp"
#include "accelerator_codegen/tpch_q1_aux.hpp"
#include "accelerator_codegen/tpch_q1_maintenance.hpp"
#include "accelerator_codegen/tpch_q1_write_policy.hpp"
#include "duckdb/transaction/transaction_context.hpp"

namespace accelerator_codegen {
namespace {

struct Q1RebuildPolicyTiming {
	bool has_measurement = false;
	uint64_t last_inter_write_interval_us = 0;
	std::chrono::steady_clock::time_point last_precommit_time {};
	bool has_precommit_time = false;
};

Q1RebuildPolicyTiming g_rebuild_timing;

} // namespace

bool Q1RebuildMaintenanceEnabled() {
	return false;
}

uint64_t GetQ1PartitionRebuildCostUs() {
	return static_cast<uint64_t>(Q1PartitionRebuildUs());
}

uint64_t GetQ1FullRebuildCostUs() {
	return static_cast<uint64_t>(Q1FullRebuildUs());
}

bool HasQ1InterWriteMeasurement() {
	return g_rebuild_timing.has_measurement;
}

uint64_t GetQ1MeasuredInterWriteIntervalUs() {
	return g_rebuild_timing.last_inter_write_interval_us;
}

void RecordQ1RebuildPolicyPreCommit() {
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

void ResetQ1RebuildPolicyTiming() {
	g_rebuild_timing = Q1RebuildPolicyTiming {};
}

bool PrepareQ1RebuildPreCommit(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                                const aux_state::InvalidationPlan &plan, aux_state::TxnInvalidationBuffer &txn_buffer) {
	(void)context;
	(void)store;
	if (!Q1RebuildMaintenanceEnabled()) {
		return false;
	}
	if (plan.IsEmpty()) {
		return true;
	}

	RecordQ1RebuildPolicyPreCommit();

	const uint64_t rebuild_cost_us = plan.scope == aux_state::InvalidationScope::AllPartitions
	                                     ? GetQ1FullRebuildCostUs()
	                                     : GetQ1PartitionRebuildCostUs() * static_cast<uint64_t>(plan.partitions.size());
	if (g_rebuild_timing.has_measurement && rebuild_cost_us >= g_rebuild_timing.last_inter_write_interval_us) {
		RecordQ1RebuildCostModelSkip();
		return false;
	}

	txn_buffer.SetPendingLazyRebuild(kQ1AuxAcceleratorId, plan);
	return false;
}

void EnsurePendingQ1RebuildApplied(duckdb::ClientContext &context) {
	auto buffer = aux_state::GetOrCreateTxnInvalidationBuffer(context);
	if (!buffer->HasPendingLazyRebuild(kQ1AuxAcceleratorId)) {
		return;
	}
	const auto start = std::chrono::steady_clock::now();
	const auto pending = buffer->PendingLazyRebuild(kQ1AuxAcceleratorId);
	const bool started_transaction = !context.transaction.HasActiveTransaction();
	if (started_transaction) {
		context.transaction.BeginTransaction();
	}
	try {
		auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
		auto &store = registry->GetStore(kQ1AuxAcceleratorId);
		ApplyQ1RebuildPlan(context, store, pending);
		buffer->ClearPendingLazyRebuild(kQ1AuxAcceleratorId);
		if (started_transaction) {
			context.transaction.Commit();
		}
		const auto end = std::chrono::steady_clock::now();
		const auto total_us = static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
		uint64_t rebuild_scan_us = 0;
		if (pending.scope == aux_state::InvalidationScope::AllPartitions) {
			rebuild_scan_us = static_cast<uint64_t>(Q1FullRebuildUs());
		} else if (pending.scope == aux_state::InvalidationScope::PartitionSet) {
			rebuild_scan_us = static_cast<uint64_t>(Q1PartitionRebuildUs() * pending.partitions.size());
		}
		RecordQ1LazyRebuildObservation(rebuild_scan_us, total_us);
	} catch (...) {
		if (started_transaction && context.transaction.HasActiveTransaction()) {
			context.transaction.Rollback(nullptr);
		}
		throw;
	}
}

void ApplyQ1RebuildPlan(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                         const aux_state::InvalidationPlan &plan) {
	(void)store;
	if (plan.IsEmpty()) {
		return;
	}

	if (plan.scope == aux_state::InvalidationScope::AllPartitions) {
		BuildQ1AuxState(context);
		RecordQ1Rebuild();
		return;
	}
	if (plan.scope == aux_state::InvalidationScope::PartitionSet) {
		for (auto partition : plan.partitions) {
			RebuildQ1AuxPartition(context, partition);
		}
		RecordQ1Rebuild();
	}
}

} // namespace accelerator_codegen
