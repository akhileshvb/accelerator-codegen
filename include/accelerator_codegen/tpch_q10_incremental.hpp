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
#include "accelerator_codegen/tpch_q10_aux.hpp"
#include "accelerator_codegen/tpch_q10_finalize.hpp"
#include "duckdb/accelerator/mutation_recorder.hpp"

namespace duckdb {
class ClientContext;
}

namespace accelerator_codegen::aux_state {
struct TableDependency;
}

namespace accelerator_codegen {

enum class Q10MutationClass : uint8_t {
	Unknown = 0,
	Irrelevant,
	PointLineitemUpdate,
	LineitemInsertDelete,
	ReturnflagFlip,
	BulkLineitem,
	OrderInsertDelete,
	OrderMetadataUpdate,
	CustomerAttributeUpdate,
	BulkCustomerMutation,
	CrossPartitionMove,
};

const char *Q10MutationClassName(Q10MutationClass class_id);

struct Q10MutationBatchFeatures {
	Q10MutationClass class_id = Q10MutationClass::Unknown;
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

struct Q10CandidateDecisionDetail {
	aux_state::maintenance::MaintenanceAction action = aux_state::maintenance::MaintenanceAction::NoOp;
	bool supported = false;
	aux_state::maintenance::ActionCostEstimate estimate;
	double total_overhead_us = 0.0;
	std::string exclusion_reason;
};

struct Q10WorkloadFeatures {
	double q10_query_rate_eps = 0.0;
	double relevant_write_rate_eps = 0.0;
	double ewma_inter_write_interval_us = 0.0;
	double expected_queries_before_next_write = 0.0;
	uint64_t invalid_partition_count = 0;
	double ewma_fallback_latency_us = 50000.0;
	double ewma_aux_hit_latency_us = 500.0;
	uint32_t active_query_count = 0;
};

struct Q10ApplyDeltaEligibility {
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

struct Q10MaintenanceDecisionRecord {
	aux_state::maintenance::MaintenanceAction selected = aux_state::maintenance::MaintenanceAction::NoOp;
	bool low_confidence = true;
	Q10MutationClass mutation_class = Q10MutationClass::Unknown;
	double selected_predicted_cost_us = 0.0;
	double selected_predicted_immediate_us = 0.0;
	double actual_cost_us = 0.0;
	double prediction_error_us = 0.0;
	double expected_queries_before_next_write = 0.0;
	uint64_t rows_touched = 0;
	uint64_t affected_partition_count = 0;
	bool stage1_selected_apply_delta = false;
	bool heavy_capture_ran = false;
	Q10ApplyDeltaEligibility apply_delta_eligibility;
	std::vector<std::pair<aux_state::maintenance::MaintenanceAction, double>> candidate_costs;
	std::vector<Q10CandidateDecisionDetail> candidate_details;
};

Q10ApplyDeltaEligibility BuildQ10ApplyDeltaEligibility(const Q10MutationBatchFeatures &features,
                                                       const aux_state::maintenance::CostModelParams &params,
                                                       bool low_confidence, uint64_t pending_light_ops,
                                                       uint64_t row_ops, uint64_t cross_partition_ops);

const aux_state::maintenance::CostModelParams &GetQ10CostModelParams();
void LoadQ10CostModelParams();
void RecordQ10WorkloadQuery();
void RecordQ10WorkloadRelevantWrite();
void ResetQ10QueriesSinceLastWrite();
void RecordQ10LazyRebuildObservation(uint64_t rebuild_scan_us, uint64_t total_stall_us);

const std::deque<Q10MaintenanceDecisionRecord> &GetQ10MaintenanceDecisionLog();
void AppendQ10MaintenanceDecisionRecord(const Q10MaintenanceDecisionRecord &record);

Q10WorkloadFeatures BuildQ10WorkloadFeatures(aux_state::PartitionedStateStore &store);

struct Q10IncrementalTxnBuffer;
Q10MutationBatchFeatures BuildQ10MutationBatchFeatures(const aux_state::InvalidationPlan &plan,
                                                     const Q10IncrementalTxnBuffer &buffer, uint64_t rows_touched);

double Q10PartitionRebuildUs();
double Q10FullRebuildUs();
double Q10FirstQueryBlockingUs();
double Q10ApplyDeltaOverheadUs();
double Q10RecentIncrementalSuccessRate();
void RecordQ10IncrementalSuccessObservation(bool success);
bool Q10HasSufficientTrainingSamples();

std::vector<aux_state::maintenance::ActionCandidate>
EnumerateQ10ActionCandidates(const Q10MutationBatchFeatures &features, const Q10WorkloadFeatures &workload,
                            const aux_state::maintenance::CostModelParams &params, bool low_confidence);

std::vector<Q10CandidateDecisionDetail>
EnumerateQ10ActionCandidatesDetailed(const Q10MutationBatchFeatures &features, const Q10WorkloadFeatures &workload,
                                    const aux_state::maintenance::CostModelParams &params, bool low_confidence);

bool Q10ApplyDeltaPlausible(const Q10MutationBatchFeatures &features, const Q10WorkloadFeatures &workload,
                           const aux_state::maintenance::CostModelParams &params, bool low_confidence);

bool Q10ShouldRunHeavyCapture(const Q10MutationBatchFeatures &features, const Q10WorkloadFeatures &workload,
                             const aux_state::maintenance::CostModelParams &params, bool low_confidence);

aux_state::maintenance::SelectionResult
SelectQ10MaintenanceAction(const Q10MutationBatchFeatures &features, const Q10WorkloadFeatures &workload,
                          aux_state::maintenance::MaintenanceAction last_action);

struct Q10PartitionDelta {
	std::unordered_map<int64_t, Q10AccT> customer_revenue_deltas;
};

struct Q10ComputedIncrementalDelta {
	bool success = false;
	std::vector<aux_state::maintenance::StagedPartitionUpdate> partition_updates;
	aux_state::InvalidationPlan affected_partitions;
};

class Q10IncrementalTxnBuffer : public aux_state::IncrementalTxnBuffer {
public:
	Q10IncrementalTxnBuffer();
	~Q10IncrementalTxnBuffer() override;
	Q10IncrementalTxnBuffer(Q10IncrementalTxnBuffer &&) noexcept;
	Q10IncrementalTxnBuffer &operator=(Q10IncrementalTxnBuffer &&) noexcept;
	Q10IncrementalTxnBuffer(const Q10IncrementalTxnBuffer &) = delete;
	Q10IncrementalTxnBuffer &operator=(const Q10IncrementalTxnBuffer &) = delete;

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
	friend void Q10AccumulateLineitemMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
	                                              const std::vector<aux_state::TableDependency> &dependencies,
	                                              Q10IncrementalTxnBuffer &buffer);
	friend void Q10AccumulateOrdersMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
	                                            const std::vector<aux_state::TableDependency> &dependencies,
	                                            Q10IncrementalTxnBuffer &buffer);
	friend void MaybeMergePendingQ10LightOps(duckdb::ClientContext &context,
	                                        const std::vector<aux_state::TableDependency> &dependencies,
	                                        Q10IncrementalTxnBuffer &buffer);
	friend bool Q10FinalizeIncrementalCapture(duckdb::ClientContext &context,
	                                         const std::vector<aux_state::TableDependency> &dependencies,
	                                         Q10IncrementalTxnBuffer &buffer);
	friend bool ComputeQ10IncrementalDelta(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
	                                      const aux_state::InvalidationPlan &plan, Q10IncrementalTxnBuffer &buffer,
	                                      Q10ComputedIncrementalDelta &out);
	friend struct Q10MutationBatchFeatures BuildQ10MutationBatchFeatures(const aux_state::InvalidationPlan &plan,
	                                                                   const Q10IncrementalTxnBuffer &buffer,
	                                                                   uint64_t rows_touched);

	struct Impl;
	std::unique_ptr<Impl> impl;
};

void Q10AccumulateLineitemMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                       const std::vector<aux_state::TableDependency> &dependencies,
                                       Q10IncrementalTxnBuffer &buffer);

void Q10AccumulateOrdersMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                     const std::vector<aux_state::TableDependency> &dependencies,
                                     Q10IncrementalTxnBuffer &buffer);

bool Q10FinalizeIncrementalCapture(duckdb::ClientContext &context,
                                  const std::vector<aux_state::TableDependency> &dependencies,
                                  Q10IncrementalTxnBuffer &buffer);

void MaybeMergePendingQ10LightOps(duckdb::ClientContext &context,
                                 const std::vector<aux_state::TableDependency> &dependencies,
                                 Q10IncrementalTxnBuffer &buffer);

bool ComputeQ10IncrementalDelta(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                               const aux_state::InvalidationPlan &plan, Q10IncrementalTxnBuffer &buffer,
                               Q10ComputedIncrementalDelta &out);

void PublishQ10IncrementalDelta(aux_state::PartitionedStateStore &store, const Q10ComputedIncrementalDelta &delta,
                                uint64_t valid_through_mutation_epoch);

Q10IncrementalTxnBuffer &GetQ10IncrementalTxnBuffer(aux_state::TxnInvalidationBuffer &txn_buffer);

aux_state::InvalidationPlan AccumulateQ10Mutation(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                                 const std::vector<aux_state::TableDependency> &dependencies,
                                                 aux_state::TxnInvalidationBuffer &txn_buffer);

} // namespace accelerator_codegen
