#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "accelerator_codegen/aux_state/incremental_txn_buffer.hpp"
#include "accelerator_codegen/aux_state/invalidation_plan.hpp"
#include "accelerator_codegen/aux_state/maintenance/staged_publication.hpp"
#include "accelerator_codegen/aux_state/partitioned_state_store.hpp"
#include "accelerator_codegen/aux_state/txn_invalidation_buffer.hpp"
#include "accelerator_codegen/sec_edgar_q1_aux.hpp"
#include "accelerator_codegen/sec_edgar_q1_finalize.hpp"
#include "duckdb/accelerator/mutation_recorder.hpp"

namespace duckdb {
class ClientContext;
}

namespace accelerator_codegen::aux_state {
struct TableDependency;
}

namespace accelerator_codegen {

struct SecEdgarQ1GroupDelta {
	int64_t cnt_delta = 0;
	int64_t line_sum_delta = 0;
	std::unordered_map<std::string, int64_t> adsh_refcount_delta;
};

struct SecEdgarQ1PartitionDelta {
	SecEdgarQ1GroupDelta delta;
};

struct SecEdgarQ1ComputedIncrementalDelta {
	bool success = false;
	std::vector<aux_state::maintenance::StagedPartitionUpdate> partition_updates;
	aux_state::InvalidationPlan affected_partitions;
};

class SecEdgarQ1IncrementalTxnBuffer : public aux_state::IncrementalTxnBuffer {
public:
	SecEdgarQ1IncrementalTxnBuffer();
	~SecEdgarQ1IncrementalTxnBuffer() override;
	SecEdgarQ1IncrementalTxnBuffer(SecEdgarQ1IncrementalTxnBuffer &&) noexcept;
	SecEdgarQ1IncrementalTxnBuffer &operator=(SecEdgarQ1IncrementalTxnBuffer &&) noexcept;
	SecEdgarQ1IncrementalTxnBuffer(const SecEdgarQ1IncrementalTxnBuffer &) = delete;
	SecEdgarQ1IncrementalTxnBuffer &operator=(const SecEdgarQ1IncrementalTxnBuffer &) = delete;

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
	friend void SecEdgarQ1AccumulatePreMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
	                                         const std::vector<aux_state::TableDependency> &dependencies,
	                                         SecEdgarQ1IncrementalTxnBuffer &buffer);
	friend void MaybeMergePendingSecEdgarQ1LightOps(duckdb::ClientContext &context,
	                                        const std::vector<aux_state::TableDependency> &dependencies,
	                                        SecEdgarQ1IncrementalTxnBuffer &buffer);
	friend bool SecEdgarQ1FinalizeIncrementalCapture(duckdb::ClientContext &context,
	                                           const std::vector<aux_state::TableDependency> &dependencies,
	                                           SecEdgarQ1IncrementalTxnBuffer &buffer);
	friend bool ComputeSecEdgarQ1IncrementalDelta(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
	                                      const aux_state::InvalidationPlan &plan, SecEdgarQ1IncrementalTxnBuffer &buffer,
	                                      SecEdgarQ1ComputedIncrementalDelta &out);
	friend struct SecEdgarQ1MutationBatchFeatures BuildSecEdgarQ1MutationBatchFeatures(const aux_state::InvalidationPlan &plan,
	                                                                   const SecEdgarQ1IncrementalTxnBuffer &buffer,
	                                                                   uint64_t rows_touched);

	struct Impl;
	std::unique_ptr<Impl> impl;
};

void SecEdgarQ1AccumulatePreMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                  const std::vector<aux_state::TableDependency> &dependencies,
                                  SecEdgarQ1IncrementalTxnBuffer &buffer);

bool SecEdgarQ1FinalizeIncrementalCapture(duckdb::ClientContext &context,
                                  const std::vector<aux_state::TableDependency> &dependencies,
                                  SecEdgarQ1IncrementalTxnBuffer &buffer);

void MaybeMergePendingSecEdgarQ1LightOps(duckdb::ClientContext &context,
                                 const std::vector<aux_state::TableDependency> &dependencies,
                                 SecEdgarQ1IncrementalTxnBuffer &buffer);

void SecEdgarQ1AccumulatePreMutation(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                             const std::vector<aux_state::TableDependency> &dependencies,
                             SecEdgarQ1IncrementalTxnBuffer &buffer);

bool ComputeSecEdgarQ1IncrementalDelta(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                               const aux_state::InvalidationPlan &plan, SecEdgarQ1IncrementalTxnBuffer &buffer,
                               SecEdgarQ1ComputedIncrementalDelta &out);

void PublishSecEdgarQ1IncrementalDelta(aux_state::PartitionedStateStore &store, const SecEdgarQ1ComputedIncrementalDelta &delta,
                               uint64_t valid_through_mutation_epoch);

SecEdgarQ1IncrementalTxnBuffer &GetSecEdgarQ1IncrementalTxnBuffer(aux_state::TxnInvalidationBuffer &txn_buffer);

aux_state::InvalidationPlan AccumulateSecEdgarQ1Mutation(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                                 const std::vector<aux_state::TableDependency> &dependencies,
                                                 aux_state::TxnInvalidationBuffer &txn_buffer);

} // namespace accelerator_codegen
