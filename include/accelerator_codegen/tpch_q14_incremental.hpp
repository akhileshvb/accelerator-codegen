#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "accelerator_codegen/aux_state/incremental_txn_buffer.hpp"
#include "accelerator_codegen/aux_state/invalidation_plan.hpp"
#include "accelerator_codegen/aux_state/maintenance/staged_publication.hpp"
#include "accelerator_codegen/aux_state/partitioned_state_store.hpp"
#include "accelerator_codegen/aux_state/txn_invalidation_buffer.hpp"
#include "accelerator_codegen/tpch_q14_finalize.hpp"
#include "accelerator_codegen/tpch_q14_aux.hpp"
#include "duckdb/accelerator/mutation_recorder.hpp"

namespace duckdb {
class ClientContext;
}

namespace accelerator_codegen::aux_state {
struct TableDependency;
}

namespace accelerator_codegen {

struct Q14PartitionDelta {
	Q14AccT total_revenue_delta = 0;
	Q14AccT promo_revenue_delta = 0;
	int64_t row_count_delta = 0;
};

struct Q14ComputedIncrementalDelta {
	bool success = false;
	std::vector<aux_state::maintenance::StagedPartitionUpdate> partition_updates;
	aux_state::InvalidationPlan affected_partitions;
};

class Q14IncrementalTxnBuffer : public aux_state::IncrementalTxnBuffer {
public:
	Q14IncrementalTxnBuffer();
	~Q14IncrementalTxnBuffer() override;
	Q14IncrementalTxnBuffer(Q14IncrementalTxnBuffer &&) noexcept;
	Q14IncrementalTxnBuffer &operator=(Q14IncrementalTxnBuffer &&) noexcept;
	Q14IncrementalTxnBuffer(const Q14IncrementalTxnBuffer &) = delete;
	Q14IncrementalTxnBuffer &operator=(const Q14IncrementalTxnBuffer &) = delete;

	void Clear() override;
	bool HasPendingChanges() const override;
	aux_state::InvalidationPlan ExtraInvalidationPlan() const override;
	bool IsPoisoned() const;
	uint64_t RowOpCount() const;
	uint64_t PromoOpCount() const;
	uint64_t PendingLightOpCount() const;
	uint64_t CheapRowsTouched() const;
	uint64_t CheapPartFanout() const;
	uint64_t EstimatedLineitemScanRows() const;
	bool HeavyCaptureDone() const;
	bool BulkMutation() const;

private:
	friend void Q14AccumulateLineitemMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
	                                               const std::vector<aux_state::TableDependency> &dependencies,
	                                               Q14IncrementalTxnBuffer &buffer);
	friend void Q14AccumulatePartMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
	                                           const std::vector<aux_state::TableDependency> &dependencies,
	                                           Q14IncrementalTxnBuffer &buffer);
	friend void MaybeMergePendingLightOps(duckdb::ClientContext &context,
	                                      const std::vector<aux_state::TableDependency> &dependencies,
	                                      Q14IncrementalTxnBuffer &buffer);
	friend void MaybeEagerFinalizeCapture(duckdb::ClientContext &context,
	                                      const std::vector<aux_state::TableDependency> &dependencies,
	                                      Q14IncrementalTxnBuffer &buffer);
	friend bool Q14FinalizeIncrementalCapture(duckdb::ClientContext &context,
	                                          const std::vector<aux_state::TableDependency> &dependencies,
	                                          Q14IncrementalTxnBuffer &buffer);
	friend bool ComputeQ14IncrementalDelta(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
	                                       const aux_state::InvalidationPlan &plan, Q14IncrementalTxnBuffer &buffer,
	                                       Q14ComputedIncrementalDelta &out);
	friend struct Q14MutationBatchFeatures BuildQ14MutationBatchFeatures(const aux_state::InvalidationPlan &plan,
	                                                                     const Q14IncrementalTxnBuffer &buffer,
	                                                                     uint64_t rows_touched);

	struct Impl;
	std::unique_ptr<Impl> impl;
};

void Q14AccumulateLineitemMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                        const std::vector<aux_state::TableDependency> &dependencies,
                                        Q14IncrementalTxnBuffer &buffer);

void Q14AccumulatePartMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                    const std::vector<aux_state::TableDependency> &dependencies,
                                    Q14IncrementalTxnBuffer &buffer);

bool Q14FinalizeIncrementalCapture(duckdb::ClientContext &context,
                                   const std::vector<aux_state::TableDependency> &dependencies,
                                   Q14IncrementalTxnBuffer &buffer);

void MaybeMergePendingLightOps(duckdb::ClientContext &context,
                               const std::vector<aux_state::TableDependency> &dependencies,
                               Q14IncrementalTxnBuffer &buffer);

void Q14AccumulateLineitemMutation(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                   const std::vector<aux_state::TableDependency> &dependencies,
                                   Q14IncrementalTxnBuffer &buffer);

void Q14AccumulatePartMutation(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                               const std::vector<aux_state::TableDependency> &dependencies,
                               Q14IncrementalTxnBuffer &buffer);

bool ComputeQ14IncrementalDelta(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                                const aux_state::InvalidationPlan &plan, Q14IncrementalTxnBuffer &buffer,
                                Q14ComputedIncrementalDelta &out);

void PublishQ14IncrementalDelta(aux_state::PartitionedStateStore &store, const Q14ComputedIncrementalDelta &delta,
                                uint64_t valid_through_mutation_epoch = 0);

bool TryApplyQ14IncrementalPreCommit(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                                    const aux_state::InvalidationPlan &plan, Q14IncrementalTxnBuffer &buffer);

Q14IncrementalTxnBuffer &GetQ14IncrementalTxnBuffer(aux_state::TxnInvalidationBuffer &txn_buffer);

aux_state::InvalidationPlan AccumulateQ14Mutation(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                                  const std::vector<aux_state::TableDependency> &dependencies,
                                                  aux_state::TxnInvalidationBuffer &txn_buffer);

} // namespace accelerator_codegen
