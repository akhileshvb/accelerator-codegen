#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "accelerator_codegen/aux_state/incremental_txn_buffer.hpp"
#include "accelerator_codegen/aux_state/invalidation_plan.hpp"
#include "accelerator_codegen/aux_state/maintenance/action_selector.hpp"
#include "accelerator_codegen/aux_state/maintenance/cost_model.hpp"
#include "accelerator_codegen/aux_state/maintenance/maintenance_action.hpp"
#include "accelerator_codegen/aux_state/maintenance/staged_publication.hpp"
#include "accelerator_codegen/aux_state/partitioned_state_store.hpp"
#include "accelerator_codegen/aux_state/txn_invalidation_buffer.hpp"
#include "accelerator_codegen/tpch_q6_aux.hpp"
#include "accelerator_codegen/tpch_q6_finalize.hpp"
#include "duckdb/accelerator/mutation_recorder.hpp"

namespace duckdb {
class ClientContext;
}

namespace accelerator_codegen::aux_state {
struct TableDependency;
}

namespace accelerator_codegen {

enum class Q6MutationClass : uint8_t {
	Unknown = 0,
	Irrelevant,
	PointLineitemUpdate,
	LineitemInsertDelete,
	ShipdateMove,
	BulkLineitem,
	CrossPartitionMove,
};

const char *Q6MutationClassName(Q6MutationClass class_id);

struct Q6MutationBatchFeatures {
	Q6MutationClass class_id = Q6MutationClass::Unknown;
	std::string table_name;
	duckdb::TableMutationKind kind = duckdb::TableMutationKind::INSERT;
	uint64_t rows_touched = 0;
	bool relevant_columns = false;
	bool incremental_supported = false;
	bool old_new_values_available = false;
	bool incremental_buffer_complete = false;
	bool incremental_buffer_poisoned = false;
	uint64_t affected_partition_count = 0;
	std::vector<aux_state::PartitionKey> affected_partitions;
	uint64_t estimated_lineitem_scan_rows = 0;
	uint64_t cross_partition_moves = 0;
	uint64_t captured_row_op_count = 0;
	bool whole_state_required = false;
	bool bulk_mutation = false;
	bool heavy_capture_available = false;
	double recent_incremental_success_rate = 1.0;
};

struct Q6CandidateDecisionDetail {
	aux_state::maintenance::MaintenanceAction action = aux_state::maintenance::MaintenanceAction::NoOp;
	bool supported = false;
	aux_state::maintenance::ActionCostEstimate estimate;
	double total_overhead_us = 0.0;
	std::string exclusion_reason;
};

struct Q6WorkloadFeatures {
	double q6_query_rate_eps = 0.0;
	double relevant_write_rate_eps = 0.0;
	double ewma_inter_write_interval_us = 0.0;
	double expected_queries_before_next_write = 0.0;
	uint64_t invalid_partition_count = 0;
	double ewma_fallback_latency_us = 50000.0;
	double ewma_aux_hit_latency_us = 500.0;
	uint32_t active_query_count = 0;
};

struct Q6ApplyDeltaEligibility {
	bool eligible = false;
	const char *primary_exclusion_reason = nullptr;
	bool low_confidence = false;
	bool bulk_mutation = false;
	bool whole_state_required = false;
	bool incremental_buffer_poisoned = false;
	bool incremental_buffer_complete = false;
	bool old_new_values_available = false;
	bool heavy_capture_available = false;
	bool incremental_supported = false;
	bool rows_exceed_max = false;
	bool training_samples_sufficient = false;
	uint64_t pending_light_ops = 0;
	uint64_t row_ops = 0;
	uint64_t cross_partition_ops = 0;
	uint64_t estimated_lineitem_scan_rows = 0;
	double apply_delta_predicted_immediate_us = 0.0;
};

struct Q6MaintenanceDecisionRecord {
	aux_state::maintenance::MaintenanceAction selected = aux_state::maintenance::MaintenanceAction::NoOp;
	bool low_confidence = true;
	Q6MutationClass mutation_class = Q6MutationClass::Unknown;
	double selected_predicted_cost_us = 0.0;
	double selected_predicted_immediate_us = 0.0;
	double actual_cost_us = 0.0;
	double prediction_error_us = 0.0;
	double expected_queries_before_next_write = 0.0;
	uint64_t rows_touched = 0;
	uint64_t affected_partition_count = 0;
	bool stage1_selected_apply_delta = false;
	bool heavy_capture_ran = false;
	Q6ApplyDeltaEligibility apply_delta_eligibility;
	std::vector<std::pair<aux_state::maintenance::MaintenanceAction, double>> candidate_costs;
	std::vector<Q6CandidateDecisionDetail> candidate_details;
};

Q6ApplyDeltaEligibility BuildQ6ApplyDeltaEligibility(const Q6MutationBatchFeatures &features,
                                                       const aux_state::maintenance::CostModelParams &params,
                                                       bool low_confidence, uint64_t pending_light_ops,
                                                       uint64_t row_ops, uint64_t cross_partition_ops);

const aux_state::maintenance::CostModelParams &GetQ6CostModelParams();
void LoadQ6CostModelParams();
void RecordQ6WorkloadQuery();
void RecordQ6WorkloadRelevantWrite();
void ResetQ6QueriesSinceLastWrite();
void RecordQ6LazyRebuildObservation(uint64_t rebuild_scan_us, uint64_t total_stall_us);

const std::deque<Q6MaintenanceDecisionRecord> &GetQ6MaintenanceDecisionLog();
void AppendQ6MaintenanceDecisionRecord(const Q6MaintenanceDecisionRecord &record);

Q6WorkloadFeatures BuildQ6WorkloadFeatures(aux_state::PartitionedStateStore &store);

struct Q6IncrementalTxnBuffer;
Q6MutationBatchFeatures BuildQ6MutationBatchFeatures(const aux_state::InvalidationPlan &plan,
                                                     const Q6IncrementalTxnBuffer &buffer, uint64_t rows_touched);

double Q6PartitionRebuildUs();
double Q6FullRebuildUs();
double Q6FirstQueryBlockingUs();
double Q6ApplyDeltaOverheadUs();
double Q6RecentIncrementalSuccessRate();
void RecordQ6IncrementalSuccessObservation(bool success);
bool Q6HasSufficientTrainingSamples();

std::vector<aux_state::maintenance::ActionCandidate>
EnumerateQ6ActionCandidates(const Q6MutationBatchFeatures &features, const Q6WorkloadFeatures &workload,
                            const aux_state::maintenance::CostModelParams &params, bool low_confidence);

std::vector<Q6CandidateDecisionDetail>
EnumerateQ6ActionCandidatesDetailed(const Q6MutationBatchFeatures &features, const Q6WorkloadFeatures &workload,
                                    const aux_state::maintenance::CostModelParams &params, bool low_confidence);

bool Q6ApplyDeltaPlausible(const Q6MutationBatchFeatures &features, const Q6WorkloadFeatures &workload,
                           const aux_state::maintenance::CostModelParams &params, bool low_confidence);

bool Q6ShouldRunHeavyCapture(const Q6MutationBatchFeatures &features, const Q6WorkloadFeatures &workload,
                             const aux_state::maintenance::CostModelParams &params, bool low_confidence);

aux_state::maintenance::SelectionResult
SelectQ6MaintenanceAction(const Q6MutationBatchFeatures &features, const Q6WorkloadFeatures &workload,
                          aux_state::maintenance::MaintenanceAction last_action);

struct Q6RowDelta {
	int8_t discount_cent = 0;
	int64_t quantity_scaled = 0;
	Q6AccT contribution = static_cast<Q6AccT>(0);
	int8_t sign = 0;
};

struct Q6PartitionDelta {
	std::vector<Q6RowDelta> row_deltas;
};

struct Q6ComputedIncrementalDelta {
	bool success = false;
	std::vector<aux_state::maintenance::StagedPartitionUpdate> partition_updates;
	aux_state::InvalidationPlan affected_partitions;
};

class Q6IncrementalTxnBuffer : public aux_state::IncrementalTxnBuffer {
public:
	Q6IncrementalTxnBuffer();
	~Q6IncrementalTxnBuffer() override;
	Q6IncrementalTxnBuffer(Q6IncrementalTxnBuffer &&) noexcept;
	Q6IncrementalTxnBuffer &operator=(Q6IncrementalTxnBuffer &&) noexcept;
	Q6IncrementalTxnBuffer(const Q6IncrementalTxnBuffer &) = delete;
	Q6IncrementalTxnBuffer &operator=(const Q6IncrementalTxnBuffer &) = delete;

	void Clear() override;
	bool HasPendingChanges() const override;
	aux_state::InvalidationPlan ExtraInvalidationPlan() const override;
	bool IsPoisoned() const;
	uint64_t RowOpCount() const;
	uint64_t CrossPartitionOpCount() const;
	uint64_t PendingLightOpCount() const;
	uint64_t CheapRowsTouched() const;
	uint64_t EstimatedLineitemScanRows() const;
	bool HeavyCaptureDone() const;
	bool BulkMutation() const;

private:
	friend void Q6AccumulateLineitemMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
	                                              const std::vector<aux_state::TableDependency> &dependencies,
	                                              Q6IncrementalTxnBuffer &buffer);
	friend void MaybeMergePendingQ6LightOps(duckdb::ClientContext &context,
	                                        const std::vector<aux_state::TableDependency> &dependencies,
	                                        Q6IncrementalTxnBuffer &buffer);
	friend bool Q6FinalizeIncrementalCapture(duckdb::ClientContext &context,
	                                         const std::vector<aux_state::TableDependency> &dependencies,
	                                         Q6IncrementalTxnBuffer &buffer);
	friend bool ComputeQ6IncrementalDelta(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
	                                      const aux_state::InvalidationPlan &plan, Q6IncrementalTxnBuffer &buffer,
	                                      Q6ComputedIncrementalDelta &out);
	friend struct Q6MutationBatchFeatures BuildQ6MutationBatchFeatures(const aux_state::InvalidationPlan &plan,
	                                                                   const Q6IncrementalTxnBuffer &buffer,
	                                                                   uint64_t rows_touched);

	struct Impl;
	std::unique_ptr<Impl> impl;
};

void Q6AccumulateLineitemMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                       const std::vector<aux_state::TableDependency> &dependencies,
                                       Q6IncrementalTxnBuffer &buffer);

bool Q6FinalizeIncrementalCapture(duckdb::ClientContext &context,
                                  const std::vector<aux_state::TableDependency> &dependencies,
                                  Q6IncrementalTxnBuffer &buffer);

void MaybeMergePendingQ6LightOps(duckdb::ClientContext &context,
                                 const std::vector<aux_state::TableDependency> &dependencies,
                                 Q6IncrementalTxnBuffer &buffer);

bool ComputeQ6IncrementalDelta(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                               const aux_state::InvalidationPlan &plan, Q6IncrementalTxnBuffer &buffer,
                               Q6ComputedIncrementalDelta &out);

void PublishQ6IncrementalDelta(aux_state::PartitionedStateStore &store, const Q6ComputedIncrementalDelta &delta,
                                uint64_t valid_through_mutation_epoch);

Q6IncrementalTxnBuffer &GetQ6IncrementalTxnBuffer(aux_state::TxnInvalidationBuffer &txn_buffer);

aux_state::InvalidationPlan AccumulateQ6Mutation(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                                 const std::vector<aux_state::TableDependency> &dependencies,
                                                 aux_state::TxnInvalidationBuffer &txn_buffer);

} // namespace accelerator_codegen
