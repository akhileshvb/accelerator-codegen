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
#include "accelerator_codegen/sec_edgar_q2_aux.hpp"
#include "accelerator_codegen/sec_edgar_q2_finalize.hpp"
#include "duckdb/accelerator/mutation_recorder.hpp"

namespace duckdb {
class ClientContext;
}

namespace accelerator_codegen::aux_state {
struct TableDependency;
}

namespace accelerator_codegen {

struct SecEdgarQ2GroupDelta {
	int64_t cnt_delta = 0;
	int64_t line_sum_delta = 0;
	std::unordered_map<std::string, int64_t> adsh_refcount_delta;
};

struct SecEdgarQ2PartitionDelta {
	SecEdgarQ2GroupDelta delta;
};

struct SecEdgarQ2ComputedIncrementalDelta {
	bool success = false;
	std::vector<aux_state::maintenance::StagedPartitionUpdate> partition_updates;
	aux_state::InvalidationPlan affected_partitions;
};

class SecEdgarQ2IncrementalTxnBuffer : public aux_state::IncrementalTxnBuffer {
public:
	SecEdgarQ2IncrementalTxnBuffer();
	~SecEdgarQ2IncrementalTxnBuffer() override;
	SecEdgarQ2IncrementalTxnBuffer(SecEdgarQ2IncrementalTxnBuffer &&) noexcept;
	SecEdgarQ2IncrementalTxnBuffer &operator=(SecEdgarQ2IncrementalTxnBuffer &&) noexcept;
	SecEdgarQ2IncrementalTxnBuffer(const SecEdgarQ2IncrementalTxnBuffer &) = delete;
	SecEdgarQ2IncrementalTxnBuffer &operator=(const SecEdgarQ2IncrementalTxnBuffer &) = delete;

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
	friend void SecEdgarQ2AccumulatePreMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
	                                         const std::vector<aux_state::TableDependency> &dependencies,
	                                         SecEdgarQ2IncrementalTxnBuffer &buffer);
	friend void MaybeMergePendingSecEdgarQ2LightOps(duckdb::ClientContext &context,
	                                        const std::vector<aux_state::TableDependency> &dependencies,
	                                        SecEdgarQ2IncrementalTxnBuffer &buffer);
	friend bool SecEdgarQ2FinalizeIncrementalCapture(duckdb::ClientContext &context,
	                                           const std::vector<aux_state::TableDependency> &dependencies,
	                                           SecEdgarQ2IncrementalTxnBuffer &buffer);
	friend bool ComputeSecEdgarQ2IncrementalDelta(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
	                                      const aux_state::InvalidationPlan &plan, SecEdgarQ2IncrementalTxnBuffer &buffer,
	                                      SecEdgarQ2ComputedIncrementalDelta &out);
	friend struct SecEdgarQ2MutationBatchFeatures BuildSecEdgarQ2MutationBatchFeatures(const aux_state::InvalidationPlan &plan,
	                                                                   const SecEdgarQ2IncrementalTxnBuffer &buffer,
	                                                                   uint64_t rows_touched);

	struct Impl;
	std::unique_ptr<Impl> impl;
};

void SecEdgarQ2AccumulatePreMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                  const std::vector<aux_state::TableDependency> &dependencies,
                                  SecEdgarQ2IncrementalTxnBuffer &buffer);

bool SecEdgarQ2FinalizeIncrementalCapture(duckdb::ClientContext &context,
                                  const std::vector<aux_state::TableDependency> &dependencies,
                                  SecEdgarQ2IncrementalTxnBuffer &buffer);

void MaybeMergePendingSecEdgarQ2LightOps(duckdb::ClientContext &context,
                                 const std::vector<aux_state::TableDependency> &dependencies,
                                 SecEdgarQ2IncrementalTxnBuffer &buffer);

void SecEdgarQ2AccumulatePreMutation(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                             const std::vector<aux_state::TableDependency> &dependencies,
                             SecEdgarQ2IncrementalTxnBuffer &buffer);

bool ComputeSecEdgarQ2IncrementalDelta(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                               const aux_state::InvalidationPlan &plan, SecEdgarQ2IncrementalTxnBuffer &buffer,
                               SecEdgarQ2ComputedIncrementalDelta &out);

void PublishSecEdgarQ2IncrementalDelta(aux_state::PartitionedStateStore &store, const SecEdgarQ2ComputedIncrementalDelta &delta,
                               uint64_t valid_through_mutation_epoch);

SecEdgarQ2IncrementalTxnBuffer &GetSecEdgarQ2IncrementalTxnBuffer(aux_state::TxnInvalidationBuffer &txn_buffer);

aux_state::InvalidationPlan AccumulateSecEdgarQ2Mutation(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                                 const std::vector<aux_state::TableDependency> &dependencies,
                                                 aux_state::TxnInvalidationBuffer &txn_buffer);

} // namespace accelerator_codegen
