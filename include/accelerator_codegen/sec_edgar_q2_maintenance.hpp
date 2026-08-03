#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "accelerator_codegen/aux_state/invalidation_plan.hpp"
#include "accelerator_codegen/aux_state/maintenance/action_selector.hpp"
#include "accelerator_codegen/aux_state/maintenance/cost_model.hpp"
#include "accelerator_codegen/aux_state/maintenance/maintenance_action.hpp"
#include "accelerator_codegen/aux_state/partition_key.hpp"
#include "duckdb/accelerator/mutation_recorder.hpp"

namespace accelerator_codegen {

enum class SecEdgarQ2MutationClass : uint8_t {
	Unknown = 0,
	Irrelevant,
	PointLineitemUpdate,
	LineitemInsertDelete,
	ShipdateMove,
	BulkLineitem,
};

const char *SecEdgarQ2MutationClassName(SecEdgarQ2MutationClass class_id);

struct SecEdgarQ2MutationBatchFeatures {
	SecEdgarQ2MutationClass class_id = SecEdgarQ2MutationClass::Unknown;
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
	uint64_t captured_row_op_count = 0;
	bool whole_state_required = false;
	bool bulk_mutation = false;
	bool heavy_capture_available = false;
	double recent_incremental_success_rate = 1.0;
};

struct SecEdgarQ2CandidateDecisionDetail {
	aux_state::maintenance::MaintenanceAction action = aux_state::maintenance::MaintenanceAction::NoOp;
	bool supported = false;
	aux_state::maintenance::ActionCostEstimate estimate;
	double total_overhead_us = 0.0;
	std::string exclusion_reason;
};

struct SecEdgarQ2WorkloadFeatures {
	double q2_query_rate_eps = 0.0;
	double relevant_write_rate_eps = 0.0;
	double ewma_inter_write_interval_us = 0.0;
	double expected_queries_before_next_write = 0.0;
	uint64_t invalid_partition_count = 0;
	double ewma_fallback_latency_us = 50000.0;
	double ewma_aux_hit_latency_us = 500.0;
	uint32_t active_query_count = 0;
};

struct SecEdgarQ2ApplyDeltaEligibility {
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
	double apply_delta_predicted_immediate_us = 0.0;
};

struct SecEdgarQ2MaintenanceDecisionRecord {
	aux_state::maintenance::MaintenanceAction selected = aux_state::maintenance::MaintenanceAction::NoOp;
	bool low_confidence = true;
	SecEdgarQ2MutationClass mutation_class = SecEdgarQ2MutationClass::Unknown;
	double selected_predicted_cost_us = 0.0;
	double selected_predicted_immediate_us = 0.0;
	double actual_cost_us = 0.0;
	double prediction_error_us = 0.0;
	double expected_queries_before_next_write = 0.0;
	uint64_t rows_touched = 0;
	uint64_t affected_partition_count = 0;
	bool stage1_selected_apply_delta = false;
	bool heavy_capture_ran = false;
	SecEdgarQ2ApplyDeltaEligibility apply_delta_eligibility;
	std::vector<std::pair<aux_state::maintenance::MaintenanceAction, double>> candidate_costs;
	std::vector<SecEdgarQ2CandidateDecisionDetail> candidate_details;
};

SecEdgarQ2ApplyDeltaEligibility BuildSecEdgarQ2ApplyDeltaEligibility(const SecEdgarQ2MutationBatchFeatures &features,
                                                     const aux_state::maintenance::CostModelParams &params,
                                                     bool low_confidence, uint64_t pending_light_ops, uint64_t row_ops);

const aux_state::maintenance::CostModelParams &GetSecEdgarQ2CostModelParams();
void LoadSecEdgarQ2CostModelParams();
void RecordSecEdgarQ2WorkloadQuery();
void RecordSecEdgarQ2WorkloadRelevantWrite();
void ResetSecEdgarQ2QueriesSinceLastWrite();
void RecordSecEdgarQ2LazyRebuildObservation(uint64_t rebuild_scan_us, uint64_t total_stall_us);

const std::deque<SecEdgarQ2MaintenanceDecisionRecord> &GetSecEdgarQ2MaintenanceDecisionLog();
void AppendSecEdgarQ2MaintenanceDecisionRecord(const SecEdgarQ2MaintenanceDecisionRecord &record);

namespace aux_state {
class PartitionedStateStore;
}
SecEdgarQ2WorkloadFeatures BuildSecEdgarQ2WorkloadFeatures(aux_state::PartitionedStateStore &store);

struct SecEdgarQ2IncrementalTxnBuffer;
SecEdgarQ2MutationBatchFeatures BuildSecEdgarQ2MutationBatchFeatures(const aux_state::InvalidationPlan &plan,
                                                     const SecEdgarQ2IncrementalTxnBuffer &buffer, uint64_t rows_touched);

double SecEdgarQ2PartitionRebuildUs();
double SecEdgarQ2FullRebuildUs();
double SecEdgarQ2FirstQueryBlockingUs();
double SecEdgarQ2ApplyDeltaOverheadUs();
double SecEdgarQ2PartitionMergeUsPerDay();
double SecEdgarQ2RecentIncrementalSuccessRate();
void RecordSecEdgarQ2IncrementalSuccessObservation(bool success);
bool SecEdgarQ2HasSufficientTrainingSamples();

std::vector<aux_state::maintenance::ActionCandidate>
EnumerateSecEdgarQ2ActionCandidates(const SecEdgarQ2MutationBatchFeatures &features, const SecEdgarQ2WorkloadFeatures &workload,
                            const aux_state::maintenance::CostModelParams &params, bool low_confidence);

std::vector<SecEdgarQ2CandidateDecisionDetail>
EnumerateSecEdgarQ2ActionCandidatesDetailed(const SecEdgarQ2MutationBatchFeatures &features, const SecEdgarQ2WorkloadFeatures &workload,
                                    const aux_state::maintenance::CostModelParams &params, bool low_confidence);

bool SecEdgarQ2ApplyDeltaPlausible(const SecEdgarQ2MutationBatchFeatures &features, const SecEdgarQ2WorkloadFeatures &workload,
                           const aux_state::maintenance::CostModelParams &params, bool low_confidence);

aux_state::maintenance::SelectionResult
SelectSecEdgarQ2MaintenanceAction(const SecEdgarQ2MutationBatchFeatures &features, const SecEdgarQ2WorkloadFeatures &workload,
                          aux_state::maintenance::MaintenanceAction last_action);

} // namespace accelerator_codegen
