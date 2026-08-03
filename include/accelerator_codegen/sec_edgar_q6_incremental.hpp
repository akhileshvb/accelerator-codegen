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
#include "accelerator_codegen/sec_edgar_q6_aux.hpp"
#include "accelerator_codegen/sec_edgar_q6_finalize.hpp"
#include "duckdb/accelerator/mutation_recorder.hpp"

namespace duckdb {
class ClientContext;
}

namespace accelerator_codegen::aux_state {
struct TableDependency;
}

namespace accelerator_codegen {

struct SecEdgarQ6GroupDelta {
	int64_t cnt_delta = 0;
	int64_t line_sum_delta = 0;
	std::unordered_map<std::string, int64_t> adsh_refcount_delta;
};

struct SecEdgarQ6PartitionDelta {
	SecEdgarQ6GroupDelta delta;
};

struct SecEdgarQ6ComputedIncrementalDelta {
	bool success = false;
	std::vector<aux_state::maintenance::StagedPartitionUpdate> partition_updates;
	aux_state::InvalidationPlan affected_partitions;
};

class SecEdgarQ6IncrementalTxnBuffer : public aux_state::IncrementalTxnBuffer {
public:
	SecEdgarQ6IncrementalTxnBuffer();
	~SecEdgarQ6IncrementalTxnBuffer() override;
	SecEdgarQ6IncrementalTxnBuffer(SecEdgarQ6IncrementalTxnBuffer &&) noexcept;
	SecEdgarQ6IncrementalTxnBuffer &operator=(SecEdgarQ6IncrementalTxnBuffer &&) noexcept;
	SecEdgarQ6IncrementalTxnBuffer(const SecEdgarQ6IncrementalTxnBuffer &) = delete;
	SecEdgarQ6IncrementalTxnBuffer &operator=(const SecEdgarQ6IncrementalTxnBuffer &) = delete;

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
	friend void SecEdgarQ6AccumulatePreMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
	                                         const std::vector<aux_state::TableDependency> &dependencies,
	                                         SecEdgarQ6IncrementalTxnBuffer &buffer);
	friend void MaybeMergePendingSecEdgarQ6LightOps(duckdb::ClientContext &context,
	                                        const std::vector<aux_state::TableDependency> &dependencies,
	                                        SecEdgarQ6IncrementalTxnBuffer &buffer);
	friend bool SecEdgarQ6FinalizeIncrementalCapture(duckdb::ClientContext &context,
	                                           const std::vector<aux_state::TableDependency> &dependencies,
	                                           SecEdgarQ6IncrementalTxnBuffer &buffer);
	friend bool ComputeSecEdgarQ6IncrementalDelta(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
	                                      const aux_state::InvalidationPlan &plan, SecEdgarQ6IncrementalTxnBuffer &buffer,
	                                      SecEdgarQ6ComputedIncrementalDelta &out);
	friend struct SecEdgarQ6MutationBatchFeatures BuildSecEdgarQ6MutationBatchFeatures(const aux_state::InvalidationPlan &plan,
	                                                                   const SecEdgarQ6IncrementalTxnBuffer &buffer,
	                                                                   uint64_t rows_touched);

	struct Impl;
	std::unique_ptr<Impl> impl;
};

void SecEdgarQ6AccumulatePreMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                  const std::vector<aux_state::TableDependency> &dependencies,
                                  SecEdgarQ6IncrementalTxnBuffer &buffer);

bool SecEdgarQ6FinalizeIncrementalCapture(duckdb::ClientContext &context,
                                  const std::vector<aux_state::TableDependency> &dependencies,
                                  SecEdgarQ6IncrementalTxnBuffer &buffer);

void MaybeMergePendingSecEdgarQ6LightOps(duckdb::ClientContext &context,
                                 const std::vector<aux_state::TableDependency> &dependencies,
                                 SecEdgarQ6IncrementalTxnBuffer &buffer);

void SecEdgarQ6AccumulatePreMutation(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                             const std::vector<aux_state::TableDependency> &dependencies,
                             SecEdgarQ6IncrementalTxnBuffer &buffer);

bool ComputeSecEdgarQ6IncrementalDelta(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                               const aux_state::InvalidationPlan &plan, SecEdgarQ6IncrementalTxnBuffer &buffer,
                               SecEdgarQ6ComputedIncrementalDelta &out);

void PublishSecEdgarQ6IncrementalDelta(aux_state::PartitionedStateStore &store, const SecEdgarQ6ComputedIncrementalDelta &delta,
                               uint64_t valid_through_mutation_epoch);

SecEdgarQ6IncrementalTxnBuffer &GetSecEdgarQ6IncrementalTxnBuffer(aux_state::TxnInvalidationBuffer &txn_buffer);

aux_state::InvalidationPlan AccumulateSecEdgarQ6Mutation(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                                 const std::vector<aux_state::TableDependency> &dependencies,
                                                 aux_state::TxnInvalidationBuffer &txn_buffer);

} // namespace accelerator_codegen
