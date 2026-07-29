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
#include "accelerator_codegen/tpch_q3_aux.hpp"
#include "accelerator_codegen/tpch_q3_finalize.hpp"
#include "duckdb/accelerator/mutation_recorder.hpp"

namespace duckdb {
class ClientContext;
}

namespace accelerator_codegen::aux_state {
struct TableDependency;
}

namespace accelerator_codegen {

enum class Q3MutationClass : uint8_t {
	Unknown = 0,
	Irrelevant,
	PointLineitemUpdate,
	LineitemInsertDelete,
	ShipdateMove,
	BulkLineitem,
	OrderInsertDelete,
	OrderMetadataUpdate,
	CustomerSegmentChange,
	BulkCustomerMutation,
	CrossPartitionMove,
};

const char *Q3MutationClassName(Q3MutationClass class_id);

struct Q3MutationBatchFeatures {
	Q3MutationClass class_id = Q3MutationClass::Unknown;
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
	uint64_t estimated_order_fanout = 0;
	uint64_t estimated_lineitem_scan_rows = 0;
	uint64_t cross_partition_moves = 0;
	uint64_t captured_row_op_count = 0;
	bool whole_state_required = false;
	bool bulk_mutation = false;
	bool heavy_capture_available = false;
	double recent_incremental_success_rate = 1.0;
};

struct Q3CandidateDecisionDetail {
	aux_state::maintenance::MaintenanceAction action = aux_state::maintenance::MaintenanceAction::NoOp;
	bool supported = false;
	aux_state::maintenance::ActionCostEstimate estimate;
	double total_overhead_us = 0.0;
	std::string exclusion_reason;
};

struct Q3WorkloadFeatures {
	double q3_query_rate_eps = 0.0;
	double relevant_write_rate_eps = 0.0;
	double ewma_inter_write_interval_us = 0.0;
	double expected_queries_before_next_write = 0.0;
	uint64_t invalid_partition_count = 0;
	double ewma_fallback_latency_us = 50000.0;
	double ewma_aux_hit_latency_us = 500.0;
	uint32_t active_query_count = 0;
};

struct Q3ApplyDeltaEligibility {
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
	bool order_fanout_exceeds_max = false;
	bool training_samples_sufficient = false;
	uint64_t pending_light_ops = 0;
	uint64_t row_ops = 0;
	uint64_t cross_partition_ops = 0;
	uint64_t bounded_order_fanout = 0;
	uint64_t estimated_lineitem_scan_rows = 0;
	double apply_delta_predicted_immediate_us = 0.0;
};

struct Q3MaintenanceDecisionRecord {
	aux_state::maintenance::MaintenanceAction selected = aux_state::maintenance::MaintenanceAction::NoOp;
	bool low_confidence = true;
	Q3MutationClass mutation_class = Q3MutationClass::Unknown;
	double selected_predicted_cost_us = 0.0;
	double selected_predicted_immediate_us = 0.0;
	double actual_cost_us = 0.0;
	double prediction_error_us = 0.0;
	double expected_queries_before_next_write = 0.0;
	uint64_t rows_touched = 0;
	uint64_t affected_partition_count = 0;
	bool stage1_selected_apply_delta = false;
	bool heavy_capture_ran = false;
	Q3ApplyDeltaEligibility apply_delta_eligibility;
	std::vector<std::pair<aux_state::maintenance::MaintenanceAction, double>> candidate_costs;
	std::vector<Q3CandidateDecisionDetail> candidate_details;
};

Q3ApplyDeltaEligibility BuildQ3ApplyDeltaEligibility(const Q3MutationBatchFeatures &features,
                                                       const aux_state::maintenance::CostModelParams &params,
                                                       bool low_confidence, uint64_t pending_light_ops,
                                                       uint64_t row_ops, uint64_t cross_partition_ops);

const aux_state::maintenance::CostModelParams &GetQ3CostModelParams();
void LoadQ3CostModelParams();
void RecordQ3WorkloadQuery();
void RecordQ3WorkloadRelevantWrite();
void ResetQ3QueriesSinceLastWrite();
void RecordQ3LazyRebuildObservation(uint64_t rebuild_scan_us, uint64_t total_stall_us);

const std::deque<Q3MaintenanceDecisionRecord> &GetQ3MaintenanceDecisionLog();
void AppendQ3MaintenanceDecisionRecord(const Q3MaintenanceDecisionRecord &record);

Q3WorkloadFeatures BuildQ3WorkloadFeatures(aux_state::PartitionedStateStore &store);

struct Q3IncrementalTxnBuffer;
Q3MutationBatchFeatures BuildQ3MutationBatchFeatures(const aux_state::InvalidationPlan &plan,
                                                     const Q3IncrementalTxnBuffer &buffer, uint64_t rows_touched);

double Q3PartitionRebuildUs();
double Q3FullRebuildUs();
double Q3FirstQueryBlockingUs();
double Q3ApplyDeltaOverheadUs();
double Q3RecentIncrementalSuccessRate();
void RecordQ3IncrementalSuccessObservation(bool success);
bool Q3HasSufficientTrainingSamples();

std::vector<aux_state::maintenance::ActionCandidate>
EnumerateQ3ActionCandidates(const Q3MutationBatchFeatures &features, const Q3WorkloadFeatures &workload,
                            const aux_state::maintenance::CostModelParams &params, bool low_confidence);

std::vector<Q3CandidateDecisionDetail>
EnumerateQ3ActionCandidatesDetailed(const Q3MutationBatchFeatures &features, const Q3WorkloadFeatures &workload,
                                    const aux_state::maintenance::CostModelParams &params, bool low_confidence);

bool Q3ApplyDeltaPlausible(const Q3MutationBatchFeatures &features, const Q3WorkloadFeatures &workload,
                           const aux_state::maintenance::CostModelParams &params, bool low_confidence);

bool Q3ShouldRunHeavyCapture(const Q3MutationBatchFeatures &features, const Q3WorkloadFeatures &workload,
                             const aux_state::maintenance::CostModelParams &params, bool low_confidence);

aux_state::maintenance::SelectionResult
SelectQ3MaintenanceAction(const Q3MutationBatchFeatures &features, const Q3WorkloadFeatures &workload,
                          aux_state::maintenance::MaintenanceAction last_action);

struct Q3PartitionDelta {
	std::unordered_map<int64_t, Q3OrderState> upserts;
	std::unordered_set<int64_t> removals;
	std::vector<std::tuple<int64_t, int32_t, Q3AccT>> bucket_deltas;
};

struct Q3ComputedIncrementalDelta {
	bool success = false;
	std::vector<aux_state::maintenance::StagedPartitionUpdate> partition_updates;
	aux_state::InvalidationPlan affected_partitions;
};

class Q3IncrementalTxnBuffer : public aux_state::IncrementalTxnBuffer {
public:
	Q3IncrementalTxnBuffer();
	~Q3IncrementalTxnBuffer() override;
	Q3IncrementalTxnBuffer(Q3IncrementalTxnBuffer &&) noexcept;
	Q3IncrementalTxnBuffer &operator=(Q3IncrementalTxnBuffer &&) noexcept;
	Q3IncrementalTxnBuffer(const Q3IncrementalTxnBuffer &) = delete;
	Q3IncrementalTxnBuffer &operator=(const Q3IncrementalTxnBuffer &) = delete;

	void Clear() override;
	bool HasPendingChanges() const override;
	aux_state::InvalidationPlan ExtraInvalidationPlan() const override;
	bool IsPoisoned() const;
	uint64_t RowOpCount() const;
	uint64_t CrossPartitionOpCount() const;
	uint64_t PendingLightOpCount() const;
	uint64_t CheapRowsTouched() const;
	uint64_t CheapOrderFanout() const;
	uint64_t EstimatedLineitemScanRows() const;
	bool HeavyCaptureDone() const;
	bool BulkMutation() const;

private:
	friend void Q3AccumulateLineitemMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
	                                              const std::vector<aux_state::TableDependency> &dependencies,
	                                              Q3IncrementalTxnBuffer &buffer);
	friend void Q3AccumulateOrdersMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
	                                            const std::vector<aux_state::TableDependency> &dependencies,
	                                            Q3IncrementalTxnBuffer &buffer);
	friend void Q3AccumulateCustomerMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
	                                              const std::vector<aux_state::TableDependency> &dependencies,
	                                              Q3IncrementalTxnBuffer &buffer);
	friend void MaybeMergePendingQ3LightOps(duckdb::ClientContext &context,
	                                        const std::vector<aux_state::TableDependency> &dependencies,
	                                        Q3IncrementalTxnBuffer &buffer);
	friend bool Q3FinalizeIncrementalCapture(duckdb::ClientContext &context,
	                                         const std::vector<aux_state::TableDependency> &dependencies,
	                                         Q3IncrementalTxnBuffer &buffer);
	friend bool ComputeQ3IncrementalDelta(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
	                                      const aux_state::InvalidationPlan &plan, Q3IncrementalTxnBuffer &buffer,
	                                      Q3ComputedIncrementalDelta &out);
	friend struct Q3MutationBatchFeatures BuildQ3MutationBatchFeatures(const aux_state::InvalidationPlan &plan,
	                                                                   const Q3IncrementalTxnBuffer &buffer,
	                                                                   uint64_t rows_touched);

	struct Impl;
	std::unique_ptr<Impl> impl;
};

void Q3AccumulateLineitemMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                       const std::vector<aux_state::TableDependency> &dependencies,
                                       Q3IncrementalTxnBuffer &buffer);

void Q3AccumulateOrdersMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                     const std::vector<aux_state::TableDependency> &dependencies,
                                     Q3IncrementalTxnBuffer &buffer);

void Q3AccumulateCustomerMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                       const std::vector<aux_state::TableDependency> &dependencies,
                                       Q3IncrementalTxnBuffer &buffer);

bool Q3FinalizeIncrementalCapture(duckdb::ClientContext &context,
                                  const std::vector<aux_state::TableDependency> &dependencies,
                                  Q3IncrementalTxnBuffer &buffer);

void MaybeMergePendingQ3LightOps(duckdb::ClientContext &context,
                                 const std::vector<aux_state::TableDependency> &dependencies,
                                 Q3IncrementalTxnBuffer &buffer);

bool ComputeQ3IncrementalDelta(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                               const aux_state::InvalidationPlan &plan, Q3IncrementalTxnBuffer &buffer,
                               Q3ComputedIncrementalDelta &out);

void PublishQ3IncrementalDelta(aux_state::PartitionedStateStore &store, const Q3ComputedIncrementalDelta &delta,
                                uint64_t valid_through_mutation_epoch);

Q3IncrementalTxnBuffer &GetQ3IncrementalTxnBuffer(aux_state::TxnInvalidationBuffer &txn_buffer);

aux_state::InvalidationPlan AccumulateQ3Mutation(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                                 const std::vector<aux_state::TableDependency> &dependencies,
                                                 aux_state::TxnInvalidationBuffer &txn_buffer);

} // namespace accelerator_codegen
