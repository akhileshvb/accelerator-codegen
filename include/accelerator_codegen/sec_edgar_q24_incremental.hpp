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
#include "accelerator_codegen/sec_edgar_q24_aux.hpp"
#include "accelerator_codegen/sec_edgar_q24_finalize.hpp"
#include "duckdb/accelerator/mutation_recorder.hpp"

namespace duckdb {
class ClientContext;
}

namespace accelerator_codegen::aux_state {
struct TableDependency;
}

namespace accelerator_codegen {

struct SecEdgarQ24GroupDelta {
	int64_t cnt_delta = 0;
	int64_t line_sum_delta = 0;
	std::unordered_map<std::string, int64_t> adsh_refcount_delta;
};

struct SecEdgarQ24PartitionDelta {
	SecEdgarQ24GroupDelta delta;
};

struct SecEdgarQ24ComputedIncrementalDelta {
	bool success = false;
	std::vector<aux_state::maintenance::StagedPartitionUpdate> partition_updates;
	aux_state::InvalidationPlan affected_partitions;
};

class SecEdgarQ24IncrementalTxnBuffer : public aux_state::IncrementalTxnBuffer {
public:
	SecEdgarQ24IncrementalTxnBuffer();
	~SecEdgarQ24IncrementalTxnBuffer() override;
	SecEdgarQ24IncrementalTxnBuffer(SecEdgarQ24IncrementalTxnBuffer &&) noexcept;
	SecEdgarQ24IncrementalTxnBuffer &operator=(SecEdgarQ24IncrementalTxnBuffer &&) noexcept;
	SecEdgarQ24IncrementalTxnBuffer(const SecEdgarQ24IncrementalTxnBuffer &) = delete;
	SecEdgarQ24IncrementalTxnBuffer &operator=(const SecEdgarQ24IncrementalTxnBuffer &) = delete;

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
	friend void SecEdgarQ24AccumulatePreMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
	                                         const std::vector<aux_state::TableDependency> &dependencies,
	                                         SecEdgarQ24IncrementalTxnBuffer &buffer);
	friend void MaybeMergePendingSecEdgarQ24LightOps(duckdb::ClientContext &context,
	                                        const std::vector<aux_state::TableDependency> &dependencies,
	                                        SecEdgarQ24IncrementalTxnBuffer &buffer);
	friend bool SecEdgarQ24FinalizeIncrementalCapture(duckdb::ClientContext &context,
	                                           const std::vector<aux_state::TableDependency> &dependencies,
	                                           SecEdgarQ24IncrementalTxnBuffer &buffer);
	friend bool ComputeSecEdgarQ24IncrementalDelta(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
	                                      const aux_state::InvalidationPlan &plan, SecEdgarQ24IncrementalTxnBuffer &buffer,
	                                      SecEdgarQ24ComputedIncrementalDelta &out);
	friend struct SecEdgarQ24MutationBatchFeatures BuildSecEdgarQ24MutationBatchFeatures(const aux_state::InvalidationPlan &plan,
	                                                                   const SecEdgarQ24IncrementalTxnBuffer &buffer,
	                                                                   uint64_t rows_touched);

	struct Impl;
	std::unique_ptr<Impl> impl;
};

void SecEdgarQ24AccumulatePreMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                  const std::vector<aux_state::TableDependency> &dependencies,
                                  SecEdgarQ24IncrementalTxnBuffer &buffer);

bool SecEdgarQ24FinalizeIncrementalCapture(duckdb::ClientContext &context,
                                  const std::vector<aux_state::TableDependency> &dependencies,
                                  SecEdgarQ24IncrementalTxnBuffer &buffer);

void MaybeMergePendingSecEdgarQ24LightOps(duckdb::ClientContext &context,
                                 const std::vector<aux_state::TableDependency> &dependencies,
                                 SecEdgarQ24IncrementalTxnBuffer &buffer);

void SecEdgarQ24AccumulatePreMutation(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                             const std::vector<aux_state::TableDependency> &dependencies,
                             SecEdgarQ24IncrementalTxnBuffer &buffer);

bool ComputeSecEdgarQ24IncrementalDelta(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                               const aux_state::InvalidationPlan &plan, SecEdgarQ24IncrementalTxnBuffer &buffer,
                               SecEdgarQ24ComputedIncrementalDelta &out);

void PublishSecEdgarQ24IncrementalDelta(aux_state::PartitionedStateStore &store, const SecEdgarQ24ComputedIncrementalDelta &delta,
                               uint64_t valid_through_mutation_epoch);

SecEdgarQ24IncrementalTxnBuffer &GetSecEdgarQ24IncrementalTxnBuffer(aux_state::TxnInvalidationBuffer &txn_buffer);

aux_state::InvalidationPlan AccumulateSecEdgarQ24Mutation(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                                 const std::vector<aux_state::TableDependency> &dependencies,
                                                 aux_state::TxnInvalidationBuffer &txn_buffer);

} // namespace accelerator_codegen
