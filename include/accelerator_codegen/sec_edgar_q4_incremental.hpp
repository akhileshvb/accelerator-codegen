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
#include "accelerator_codegen/sec_edgar_q4_aux.hpp"
#include "accelerator_codegen/sec_edgar_q4_finalize.hpp"
#include "duckdb/accelerator/mutation_recorder.hpp"

namespace duckdb {
class ClientContext;
}

namespace accelerator_codegen::aux_state {
struct TableDependency;
}

namespace accelerator_codegen {

struct SecEdgarQ4GroupDelta {
	int64_t cnt_delta = 0;
	int64_t line_sum_delta = 0;
	std::unordered_map<std::string, int64_t> adsh_refcount_delta;
};

struct SecEdgarQ4PartitionDelta {
	SecEdgarQ4GroupDelta delta;
};

struct SecEdgarQ4ComputedIncrementalDelta {
	bool success = false;
	std::vector<aux_state::maintenance::StagedPartitionUpdate> partition_updates;
	aux_state::InvalidationPlan affected_partitions;
};

class SecEdgarQ4IncrementalTxnBuffer : public aux_state::IncrementalTxnBuffer {
public:
	SecEdgarQ4IncrementalTxnBuffer();
	~SecEdgarQ4IncrementalTxnBuffer() override;
	SecEdgarQ4IncrementalTxnBuffer(SecEdgarQ4IncrementalTxnBuffer &&) noexcept;
	SecEdgarQ4IncrementalTxnBuffer &operator=(SecEdgarQ4IncrementalTxnBuffer &&) noexcept;
	SecEdgarQ4IncrementalTxnBuffer(const SecEdgarQ4IncrementalTxnBuffer &) = delete;
	SecEdgarQ4IncrementalTxnBuffer &operator=(const SecEdgarQ4IncrementalTxnBuffer &) = delete;

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
	friend void SecEdgarQ4AccumulatePreMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
	                                         const std::vector<aux_state::TableDependency> &dependencies,
	                                         SecEdgarQ4IncrementalTxnBuffer &buffer);
	friend void MaybeMergePendingSecEdgarQ4LightOps(duckdb::ClientContext &context,
	                                        const std::vector<aux_state::TableDependency> &dependencies,
	                                        SecEdgarQ4IncrementalTxnBuffer &buffer);
	friend bool SecEdgarQ4FinalizeIncrementalCapture(duckdb::ClientContext &context,
	                                           const std::vector<aux_state::TableDependency> &dependencies,
	                                           SecEdgarQ4IncrementalTxnBuffer &buffer);
	friend bool ComputeSecEdgarQ4IncrementalDelta(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
	                                      const aux_state::InvalidationPlan &plan, SecEdgarQ4IncrementalTxnBuffer &buffer,
	                                      SecEdgarQ4ComputedIncrementalDelta &out);
	friend struct SecEdgarQ4MutationBatchFeatures BuildSecEdgarQ4MutationBatchFeatures(const aux_state::InvalidationPlan &plan,
	                                                                   const SecEdgarQ4IncrementalTxnBuffer &buffer,
	                                                                   uint64_t rows_touched);

	struct Impl;
	std::unique_ptr<Impl> impl;
};

void SecEdgarQ4AccumulatePreMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                  const std::vector<aux_state::TableDependency> &dependencies,
                                  SecEdgarQ4IncrementalTxnBuffer &buffer);

bool SecEdgarQ4FinalizeIncrementalCapture(duckdb::ClientContext &context,
                                  const std::vector<aux_state::TableDependency> &dependencies,
                                  SecEdgarQ4IncrementalTxnBuffer &buffer);

void MaybeMergePendingSecEdgarQ4LightOps(duckdb::ClientContext &context,
                                 const std::vector<aux_state::TableDependency> &dependencies,
                                 SecEdgarQ4IncrementalTxnBuffer &buffer);

void SecEdgarQ4AccumulatePreMutation(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                             const std::vector<aux_state::TableDependency> &dependencies,
                             SecEdgarQ4IncrementalTxnBuffer &buffer);

bool ComputeSecEdgarQ4IncrementalDelta(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                               const aux_state::InvalidationPlan &plan, SecEdgarQ4IncrementalTxnBuffer &buffer,
                               SecEdgarQ4ComputedIncrementalDelta &out);

void PublishSecEdgarQ4IncrementalDelta(aux_state::PartitionedStateStore &store, const SecEdgarQ4ComputedIncrementalDelta &delta,
                               uint64_t valid_through_mutation_epoch);

SecEdgarQ4IncrementalTxnBuffer &GetSecEdgarQ4IncrementalTxnBuffer(aux_state::TxnInvalidationBuffer &txn_buffer);

aux_state::InvalidationPlan AccumulateSecEdgarQ4Mutation(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                                 const std::vector<aux_state::TableDependency> &dependencies,
                                                 aux_state::TxnInvalidationBuffer &txn_buffer);

} // namespace accelerator_codegen
