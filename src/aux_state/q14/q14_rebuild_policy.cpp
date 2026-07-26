#include "accelerator_codegen/tpch_q14_rebuild_policy.hpp"

#include <chrono>

#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/aux_state/txn_invalidation_buffer.hpp"
#include "accelerator_codegen/tpch_q14_aux.hpp"
#include "accelerator_codegen/tpch_q14_maintenance.hpp"
#include "accelerator_codegen/tpch_q14_write_policy.hpp"
#include "duckdb/transaction/transaction_context.hpp"

namespace accelerator_codegen {
namespace {

struct Q14RebuildPolicyTiming {
	bool has_measurement = false;
	uint64_t last_inter_write_interval_us = 0;
	std::chrono::steady_clock::time_point last_precommit_time {};
	bool has_precommit_time = false;
};

Q14RebuildPolicyTiming g_rebuild_timing;

} // namespace

bool Q14RebuildMaintenanceEnabled() {
	return false;
}

uint64_t GetQ14PartitionRebuildCostUs() {
	return static_cast<uint64_t>(Q14PartitionRebuildUs());
}

uint64_t GetQ14FullRebuildCostUs() {
	return static_cast<uint64_t>(Q14FullRebuildUs());
}

bool HasQ14InterWriteMeasurement() {
	return g_rebuild_timing.has_measurement;
}

uint64_t GetQ14MeasuredInterWriteIntervalUs() {
	return g_rebuild_timing.last_inter_write_interval_us;
}

void RecordQ14RebuildPolicyPreCommit() {
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

void ResetQ14RebuildPolicyTiming() {
	g_rebuild_timing = Q14RebuildPolicyTiming {};
}

bool PrepareQ14RebuildPreCommit(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                                const aux_state::InvalidationPlan &plan, aux_state::TxnInvalidationBuffer &txn_buffer) {
	(void)context;
	(void)store;
	if (!Q14RebuildMaintenanceEnabled()) {
		return false;
	}
	if (plan.IsEmpty()) {
		return true;
	}

	RecordQ14RebuildPolicyPreCommit();

	const uint64_t rebuild_cost_us = plan.scope == aux_state::InvalidationScope::AllPartitions
	                                     ? GetQ14FullRebuildCostUs()
	                                     : GetQ14PartitionRebuildCostUs() * static_cast<uint64_t>(plan.partitions.size());
	if (g_rebuild_timing.has_measurement && rebuild_cost_us >= g_rebuild_timing.last_inter_write_interval_us) {
		RecordQ14RebuildCostModelSkip();
		return false;
	}

	txn_buffer.SetPendingQ14Rebuild(plan);
	return false;
}

void EnsurePendingQ14RebuildApplied(duckdb::ClientContext &context) {
	auto buffer = aux_state::GetOrCreateTxnInvalidationBuffer(context);
	if (!buffer->HasPendingQ14Rebuild()) {
		return;
	}
	const auto start = std::chrono::steady_clock::now();
	const auto pending = buffer->PendingQ14Rebuild();
	const bool started_transaction = !context.transaction.HasActiveTransaction();
	if (started_transaction) {
		context.transaction.BeginTransaction();
	}
	try {
		auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
		auto &store = registry->GetStore("tpch_q14_aux");
		ApplyQ14RebuildPlan(context, store, pending);
		buffer->ClearPendingQ14Rebuild();
		if (started_transaction) {
			context.transaction.Commit();
		}
		const auto end = std::chrono::steady_clock::now();
		const auto total_us = static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
		uint64_t rebuild_scan_us = 0;
		if (pending.scope == aux_state::InvalidationScope::AllPartitions) {
			rebuild_scan_us = static_cast<uint64_t>(Q14FullRebuildUs());
		} else if (pending.scope == aux_state::InvalidationScope::PartitionSet) {
			rebuild_scan_us = static_cast<uint64_t>(Q14PartitionRebuildUs() * pending.partitions.size());
		}
		RecordQ14LazyRebuildObservation(rebuild_scan_us, total_us);
	} catch (...) {
		if (started_transaction && context.transaction.HasActiveTransaction()) {
			context.transaction.Rollback(nullptr);
		}
		throw;
	}
}

void ApplyQ14RebuildPlan(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                         const aux_state::InvalidationPlan &plan) {
	(void)store;
	if (plan.IsEmpty()) {
		return;
	}

	if (plan.scope == aux_state::InvalidationScope::AllPartitions) {
		BuildQ14AuxState(context);
		RecordQ14Rebuild();
		return;
	}
	if (plan.scope == aux_state::InvalidationScope::PartitionSet) {
		for (auto partition : plan.partitions) {
			RebuildQ14AuxPartition(context, partition);
		}
		RecordQ14Rebuild();
	}
}

} // namespace accelerator_codegen
