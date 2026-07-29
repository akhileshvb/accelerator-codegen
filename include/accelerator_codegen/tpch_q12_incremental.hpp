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
#include "accelerator_codegen/tpch_q12_aux.hpp"
#include "accelerator_codegen/tpch_q12_finalize.hpp"
#include "duckdb/accelerator/mutation_recorder.hpp"

namespace duckdb {
class ClientContext;
}

namespace accelerator_codegen::aux_state {
struct TableDependency;
}

namespace accelerator_codegen {

enum class Q12MutationClass : uint8_t {
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

const char *Q12MutationClassName(Q12MutationClass class_id);

struct Q12MutationBatchFeatures {
	Q12MutationClass class_id = Q12MutationClass::Unknown;
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

struct Q12CandidateDecisionDetail {
	aux_state::maintenance::MaintenanceAction action = aux_state::maintenance::MaintenanceAction::NoOp;
	bool supported = false;
	aux_state::maintenance::ActionCostEstimate estimate;
	double total_overhead_us = 0.0;
	std::string exclusion_reason;
};

struct Q12WorkloadFeatures {
	double q12_query_rate_eps = 0.0;
	double relevant_write_rate_eps = 0.0;
	double ewma_inter_write_interval_us = 0.0;
	double expected_queries_before_next_write = 0.0;
	uint64_t invalid_partition_count = 0;
	double ewma_fallback_latency_us = 50000.0;
	double ewma_aux_hit_latency_us = 500.0;
	uint32_t active_query_count = 0;
};

struct Q12ApplyDeltaEligibility {
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

struct Q12MaintenanceDecisionRecord {
	aux_state::maintenance::MaintenanceAction selected = aux_state::maintenance::MaintenanceAction::NoOp;
	bool low_confidence = true;
	Q12MutationClass mutation_class = Q12MutationClass::Unknown;
	double selected_predicted_cost_us = 0.0;
	double selected_predicted_immediate_us = 0.0;
	double actual_cost_us = 0.0;
	double prediction_error_us = 0.0;
	double expected_queries_before_next_write = 0.0;
	uint64_t rows_touched = 0;
	uint64_t affected_partition_count = 0;
	bool stage1_selected_apply_delta = false;
	bool heavy_capture_ran = false;
	Q12ApplyDeltaEligibility apply_delta_eligibility;
	std::vector<std::pair<aux_state::maintenance::MaintenanceAction, double>> candidate_costs;
	std::vector<Q12CandidateDecisionDetail> candidate_details;
};

Q12ApplyDeltaEligibility BuildQ12ApplyDeltaEligibility(const Q12MutationBatchFeatures &features,
                                                       const aux_state::maintenance::CostModelParams &params,
                                                       bool low_confidence, uint64_t pending_light_ops,
                                                       uint64_t row_ops, uint64_t cross_partition_ops);

const aux_state::maintenance::CostModelParams &GetQ12CostModelParams();
void LoadQ12CostModelParams();
void RecordQ12WorkloadQuery();
void RecordQ12WorkloadRelevantWrite();
void ResetQ12QueriesSinceLastWrite();
void RecordQ12LazyRebuildObservation(uint64_t rebuild_scan_us, uint64_t total_stall_us);

const std::deque<Q12MaintenanceDecisionRecord> &GetQ12MaintenanceDecisionLog();
void AppendQ12MaintenanceDecisionRecord(const Q12MaintenanceDecisionRecord &record);

Q12WorkloadFeatures BuildQ12WorkloadFeatures(aux_state::PartitionedStateStore &store);

struct Q12IncrementalTxnBuffer;
Q12MutationBatchFeatures BuildQ12MutationBatchFeatures(const aux_state::InvalidationPlan &plan,
                                                     const Q12IncrementalTxnBuffer &buffer, uint64_t rows_touched);

double Q12PartitionRebuildUs();
double Q12FullRebuildUs();
double Q12FirstQueryBlockingUs();
double Q12ApplyDeltaOverheadUs();
double Q12RecentIncrementalSuccessRate();
void RecordQ12IncrementalSuccessObservation(bool success);
bool Q12HasSufficientTrainingSamples();

std::vector<aux_state::maintenance::ActionCandidate>
EnumerateQ12ActionCandidates(const Q12MutationBatchFeatures &features, const Q12WorkloadFeatures &workload,
                            const aux_state::maintenance::CostModelParams &params, bool low_confidence);

std::vector<Q12CandidateDecisionDetail>
EnumerateQ12ActionCandidatesDetailed(const Q12MutationBatchFeatures &features, const Q12WorkloadFeatures &workload,
                                    const aux_state::maintenance::CostModelParams &params, bool low_confidence);

bool Q12ApplyDeltaPlausible(const Q12MutationBatchFeatures &features, const Q12WorkloadFeatures &workload,
                           const aux_state::maintenance::CostModelParams &params, bool low_confidence);

bool Q12ShouldRunHeavyCapture(const Q12MutationBatchFeatures &features, const Q12WorkloadFeatures &workload,
                             const aux_state::maintenance::CostModelParams &params, bool low_confidence);

aux_state::maintenance::SelectionResult
SelectQ12MaintenanceAction(const Q12MutationBatchFeatures &features, const Q12WorkloadFeatures &workload,
                          aux_state::maintenance::MaintenanceAction last_action);

struct Q12CounterDelta {
	std::string shipmode;
	Q12PriorityClass priority = Q12PriorityClass::Absent;
	int8_t sign = 0;
};

struct Q12PartitionDelta {
	std::vector<Q12CounterDelta> counter_deltas;
};

struct Q12ComputedIncrementalDelta {
	bool success = false;
	std::vector<aux_state::maintenance::StagedPartitionUpdate> partition_updates;
	aux_state::InvalidationPlan affected_partitions;
};

class Q12IncrementalTxnBuffer : public aux_state::IncrementalTxnBuffer {
public:
	Q12IncrementalTxnBuffer();
	~Q12IncrementalTxnBuffer() override;
	Q12IncrementalTxnBuffer(Q12IncrementalTxnBuffer &&) noexcept;
	Q12IncrementalTxnBuffer &operator=(Q12IncrementalTxnBuffer &&) noexcept;
	Q12IncrementalTxnBuffer(const Q12IncrementalTxnBuffer &) = delete;
	Q12IncrementalTxnBuffer &operator=(const Q12IncrementalTxnBuffer &) = delete;

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
	friend void Q12AccumulateLineitemMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
	                                              const std::vector<aux_state::TableDependency> &dependencies,
	                                              Q12IncrementalTxnBuffer &buffer);
	friend void Q12AccumulateOrdersMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
	                                            const std::vector<aux_state::TableDependency> &dependencies,
	                                            Q12IncrementalTxnBuffer &buffer);
	friend void MaybeMergePendingQ12LightOps(duckdb::ClientContext &context,
	                                        const std::vector<aux_state::TableDependency> &dependencies,
	                                        Q12IncrementalTxnBuffer &buffer);
	friend bool Q12FinalizeIncrementalCapture(duckdb::ClientContext &context,
	                                         const std::vector<aux_state::TableDependency> &dependencies,
	                                         Q12IncrementalTxnBuffer &buffer);
	friend bool ComputeQ12IncrementalDelta(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
	                                      const aux_state::InvalidationPlan &plan, Q12IncrementalTxnBuffer &buffer,
	                                      Q12ComputedIncrementalDelta &out);
	friend struct Q12MutationBatchFeatures BuildQ12MutationBatchFeatures(const aux_state::InvalidationPlan &plan,
	                                                                   const Q12IncrementalTxnBuffer &buffer,
	                                                                   uint64_t rows_touched);

	struct Impl;
	std::unique_ptr<Impl> impl;
};

void Q12AccumulateLineitemMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                       const std::vector<aux_state::TableDependency> &dependencies,
                                       Q12IncrementalTxnBuffer &buffer);

void Q12AccumulateOrdersMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                     const std::vector<aux_state::TableDependency> &dependencies,
                                     Q12IncrementalTxnBuffer &buffer);

bool Q12FinalizeIncrementalCapture(duckdb::ClientContext &context,
                                  const std::vector<aux_state::TableDependency> &dependencies,
                                  Q12IncrementalTxnBuffer &buffer);

void MaybeMergePendingQ12LightOps(duckdb::ClientContext &context,
                                 const std::vector<aux_state::TableDependency> &dependencies,
                                 Q12IncrementalTxnBuffer &buffer);

bool ComputeQ12IncrementalDelta(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                               const aux_state::InvalidationPlan &plan, Q12IncrementalTxnBuffer &buffer,
                               Q12ComputedIncrementalDelta &out);

void PublishQ12IncrementalDelta(aux_state::PartitionedStateStore &store, const Q12ComputedIncrementalDelta &delta,
                                uint64_t valid_through_mutation_epoch);

Q12IncrementalTxnBuffer &GetQ12IncrementalTxnBuffer(aux_state::TxnInvalidationBuffer &txn_buffer);

aux_state::InvalidationPlan AccumulateQ12Mutation(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                                 const std::vector<aux_state::TableDependency> &dependencies,
                                                 aux_state::TxnInvalidationBuffer &txn_buffer);

} // namespace accelerator_codegen
