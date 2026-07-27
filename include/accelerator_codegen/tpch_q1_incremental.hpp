#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "accelerator_codegen/aux_state/incremental_txn_buffer.hpp"
#include "accelerator_codegen/aux_state/invalidation_plan.hpp"
#include "accelerator_codegen/aux_state/maintenance/staged_publication.hpp"
#include "accelerator_codegen/aux_state/partitioned_state_store.hpp"
#include "accelerator_codegen/aux_state/txn_invalidation_buffer.hpp"
#include "accelerator_codegen/tpch_q1_aux.hpp"
#include "accelerator_codegen/tpch_q1_finalize.hpp"
#include "duckdb/accelerator/mutation_recorder.hpp"

namespace duckdb {
class ClientContext;
}

namespace accelerator_codegen::aux_state {
struct TableDependency;
}

namespace accelerator_codegen {

struct Q1GroupDelta {
	Q1DayGroupStats delta;
};

struct Q1PartitionDelta {
	std::unordered_map<Q1GroupKey, Q1GroupDelta> groups;
};

struct Q1ComputedIncrementalDelta {
	bool success = false;
	std::vector<aux_state::maintenance::StagedPartitionUpdate> partition_updates;
	aux_state::InvalidationPlan affected_partitions;
};

class Q1IncrementalTxnBuffer : public aux_state::IncrementalTxnBuffer {
public:
	Q1IncrementalTxnBuffer();
	~Q1IncrementalTxnBuffer() override;
	Q1IncrementalTxnBuffer(Q1IncrementalTxnBuffer &&) noexcept;
	Q1IncrementalTxnBuffer &operator=(Q1IncrementalTxnBuffer &&) noexcept;
	Q1IncrementalTxnBuffer(const Q1IncrementalTxnBuffer &) = delete;
	Q1IncrementalTxnBuffer &operator=(const Q1IncrementalTxnBuffer &) = delete;

	void Clear() override;
	bool HasPendingChanges() const override;
	aux_state::InvalidationPlan ExtraInvalidationPlan() const override;
	bool IsPoisoned() const;
	uint64_t RowOpCount() const;
	uint64_t PendingLightOpCount() const;
	uint64_t CheapRowsTouched() const;
	bool HeavyCaptureDone() const;
	bool BulkMutation() const;

private:
	friend void Q1AccumulateLineitemMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
	                                              const std::vector<aux_state::TableDependency> &dependencies,
	                                              Q1IncrementalTxnBuffer &buffer);
	friend void MaybeMergePendingQ1LightOps(duckdb::ClientContext &context,
	                                        const std::vector<aux_state::TableDependency> &dependencies,
	                                        Q1IncrementalTxnBuffer &buffer);
	friend bool Q1FinalizeIncrementalCapture(duckdb::ClientContext &context,
	                                           const std::vector<aux_state::TableDependency> &dependencies,
	                                           Q1IncrementalTxnBuffer &buffer);
	friend bool ComputeQ1IncrementalDelta(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
	                                      const aux_state::InvalidationPlan &plan, Q1IncrementalTxnBuffer &buffer,
	                                      Q1ComputedIncrementalDelta &out);
	friend struct Q1MutationBatchFeatures BuildQ1MutationBatchFeatures(const aux_state::InvalidationPlan &plan,
	                                                                   const Q1IncrementalTxnBuffer &buffer,
	                                                                   uint64_t rows_touched);

	struct Impl;
	std::unique_ptr<Impl> impl;
};

void Q1AccumulateLineitemMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                       const std::vector<aux_state::TableDependency> &dependencies,
                                       Q1IncrementalTxnBuffer &buffer);

bool Q1FinalizeIncrementalCapture(duckdb::ClientContext &context,
                                  const std::vector<aux_state::TableDependency> &dependencies,
                                  Q1IncrementalTxnBuffer &buffer);

void MaybeMergePendingQ1LightOps(duckdb::ClientContext &context,
                                 const std::vector<aux_state::TableDependency> &dependencies,
                                 Q1IncrementalTxnBuffer &buffer);

void Q1AccumulateLineitemMutation(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                  const std::vector<aux_state::TableDependency> &dependencies,
                                  Q1IncrementalTxnBuffer &buffer);

bool ComputeQ1IncrementalDelta(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                               const aux_state::InvalidationPlan &plan, Q1IncrementalTxnBuffer &buffer,
                               Q1ComputedIncrementalDelta &out);

void PublishQ1IncrementalDelta(aux_state::PartitionedStateStore &store, const Q1ComputedIncrementalDelta &delta);

Q1IncrementalTxnBuffer &GetQ1IncrementalTxnBuffer(aux_state::TxnInvalidationBuffer &txn_buffer);

aux_state::InvalidationPlan AccumulateQ1Mutation(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                                 const std::vector<aux_state::TableDependency> &dependencies,
                                                 aux_state::TxnInvalidationBuffer &txn_buffer);

} // namespace accelerator_codegen
