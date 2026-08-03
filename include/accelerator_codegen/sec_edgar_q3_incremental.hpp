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
#include "accelerator_codegen/sec_edgar_q3_aux.hpp"
#include "accelerator_codegen/sec_edgar_q3_finalize.hpp"
#include "duckdb/accelerator/mutation_recorder.hpp"

namespace duckdb {
class ClientContext;
}

namespace accelerator_codegen::aux_state {
struct TableDependency;
}

namespace accelerator_codegen {

struct SecEdgarQ3GroupDelta {
	int64_t cnt_delta = 0;
	int64_t line_sum_delta = 0;
	std::unordered_map<std::string, int64_t> adsh_refcount_delta;
};

struct SecEdgarQ3PartitionDelta {
	SecEdgarQ3GroupDelta delta;
};

struct SecEdgarQ3ComputedIncrementalDelta {
	bool success = false;
	std::vector<aux_state::maintenance::StagedPartitionUpdate> partition_updates;
	aux_state::InvalidationPlan affected_partitions;
};

class SecEdgarQ3IncrementalTxnBuffer : public aux_state::IncrementalTxnBuffer {
public:
	SecEdgarQ3IncrementalTxnBuffer();
	~SecEdgarQ3IncrementalTxnBuffer() override;
	SecEdgarQ3IncrementalTxnBuffer(SecEdgarQ3IncrementalTxnBuffer &&) noexcept;
	SecEdgarQ3IncrementalTxnBuffer &operator=(SecEdgarQ3IncrementalTxnBuffer &&) noexcept;
	SecEdgarQ3IncrementalTxnBuffer(const SecEdgarQ3IncrementalTxnBuffer &) = delete;
	SecEdgarQ3IncrementalTxnBuffer &operator=(const SecEdgarQ3IncrementalTxnBuffer &) = delete;

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
	friend void SecEdgarQ3AccumulatePreMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
	                                         const std::vector<aux_state::TableDependency> &dependencies,
	                                         SecEdgarQ3IncrementalTxnBuffer &buffer);
	friend void MaybeMergePendingSecEdgarQ3LightOps(duckdb::ClientContext &context,
	                                        const std::vector<aux_state::TableDependency> &dependencies,
	                                        SecEdgarQ3IncrementalTxnBuffer &buffer);
	friend bool SecEdgarQ3FinalizeIncrementalCapture(duckdb::ClientContext &context,
	                                           const std::vector<aux_state::TableDependency> &dependencies,
	                                           SecEdgarQ3IncrementalTxnBuffer &buffer);
	friend bool ComputeSecEdgarQ3IncrementalDelta(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
	                                      const aux_state::InvalidationPlan &plan, SecEdgarQ3IncrementalTxnBuffer &buffer,
	                                      SecEdgarQ3ComputedIncrementalDelta &out);
	friend struct SecEdgarQ3MutationBatchFeatures BuildSecEdgarQ3MutationBatchFeatures(const aux_state::InvalidationPlan &plan,
	                                                                   const SecEdgarQ3IncrementalTxnBuffer &buffer,
	                                                                   uint64_t rows_touched);

	struct Impl;
	std::unique_ptr<Impl> impl;
};

void SecEdgarQ3AccumulatePreMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                  const std::vector<aux_state::TableDependency> &dependencies,
                                  SecEdgarQ3IncrementalTxnBuffer &buffer);

bool SecEdgarQ3FinalizeIncrementalCapture(duckdb::ClientContext &context,
                                  const std::vector<aux_state::TableDependency> &dependencies,
                                  SecEdgarQ3IncrementalTxnBuffer &buffer);

void MaybeMergePendingSecEdgarQ3LightOps(duckdb::ClientContext &context,
                                 const std::vector<aux_state::TableDependency> &dependencies,
                                 SecEdgarQ3IncrementalTxnBuffer &buffer);

void SecEdgarQ3AccumulatePreMutation(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                             const std::vector<aux_state::TableDependency> &dependencies,
                             SecEdgarQ3IncrementalTxnBuffer &buffer);

bool ComputeSecEdgarQ3IncrementalDelta(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                               const aux_state::InvalidationPlan &plan, SecEdgarQ3IncrementalTxnBuffer &buffer,
                               SecEdgarQ3ComputedIncrementalDelta &out);

void PublishSecEdgarQ3IncrementalDelta(aux_state::PartitionedStateStore &store, const SecEdgarQ3ComputedIncrementalDelta &delta,
                               uint64_t valid_through_mutation_epoch);

SecEdgarQ3IncrementalTxnBuffer &GetSecEdgarQ3IncrementalTxnBuffer(aux_state::TxnInvalidationBuffer &txn_buffer);

aux_state::InvalidationPlan AccumulateSecEdgarQ3Mutation(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                                 const std::vector<aux_state::TableDependency> &dependencies,
                                                 aux_state::TxnInvalidationBuffer &txn_buffer);

} // namespace accelerator_codegen
