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

enum class SecEdgarQ24MutationClass : uint8_t {
	Unknown = 0,
	Irrelevant,
	PointLineitemUpdate,
	LineitemInsertDelete,
	ShipdateMove,
	BulkLineitem,
};

const char *SecEdgarQ24MutationClassName(SecEdgarQ24MutationClass class_id);

struct SecEdgarQ24MutationBatchFeatures {
	SecEdgarQ24MutationClass class_id = SecEdgarQ24MutationClass::Unknown;
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

struct SecEdgarQ24CandidateDecisionDetail {
	aux_state::maintenance::MaintenanceAction action = aux_state::maintenance::MaintenanceAction::NoOp;
	bool supported = false;
	aux_state::maintenance::ActionCostEstimate estimate;
	double total_overhead_us = 0.0;
	std::string exclusion_reason;
};

struct SecEdgarQ24WorkloadFeatures {
	double q24_query_rate_eps = 0.0;
	double relevant_write_rate_eps = 0.0;
	double ewma_inter_write_interval_us = 0.0;
	double expected_queries_before_next_write = 0.0;
	uint64_t invalid_partition_count = 0;
	double ewma_fallback_latency_us = 50000.0;
	double ewma_aux_hit_latency_us = 500.0;
	uint32_t active_query_count = 0;
};

struct SecEdgarQ24ApplyDeltaEligibility {
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

struct SecEdgarQ24MaintenanceDecisionRecord {
	aux_state::maintenance::MaintenanceAction selected = aux_state::maintenance::MaintenanceAction::NoOp;
	bool low_confidence = true;
	SecEdgarQ24MutationClass mutation_class = SecEdgarQ24MutationClass::Unknown;
	double selected_predicted_cost_us = 0.0;
	double selected_predicted_immediate_us = 0.0;
	double actual_cost_us = 0.0;
	double prediction_error_us = 0.0;
	double expected_queries_before_next_write = 0.0;
	uint64_t rows_touched = 0;
	uint64_t affected_partition_count = 0;
	bool stage1_selected_apply_delta = false;
	bool heavy_capture_ran = false;
	SecEdgarQ24ApplyDeltaEligibility apply_delta_eligibility;
	std::vector<std::pair<aux_state::maintenance::MaintenanceAction, double>> candidate_costs;
	std::vector<SecEdgarQ24CandidateDecisionDetail> candidate_details;
};

SecEdgarQ24ApplyDeltaEligibility BuildSecEdgarQ24ApplyDeltaEligibility(const SecEdgarQ24MutationBatchFeatures &features,
                                                     const aux_state::maintenance::CostModelParams &params,
                                                     bool low_confidence, uint64_t pending_light_ops, uint64_t row_ops);

const aux_state::maintenance::CostModelParams &GetSecEdgarQ24CostModelParams();
void LoadSecEdgarQ24CostModelParams();
void RecordSecEdgarQ24WorkloadQuery();
void RecordSecEdgarQ24WorkloadRelevantWrite();
void ResetSecEdgarQ24QueriesSinceLastWrite();
void RecordSecEdgarQ24LazyRebuildObservation(uint64_t rebuild_scan_us, uint64_t total_stall_us);

const std::deque<SecEdgarQ24MaintenanceDecisionRecord> &GetSecEdgarQ24MaintenanceDecisionLog();
void AppendSecEdgarQ24MaintenanceDecisionRecord(const SecEdgarQ24MaintenanceDecisionRecord &record);

namespace aux_state {
class PartitionedStateStore;
}
SecEdgarQ24WorkloadFeatures BuildSecEdgarQ24WorkloadFeatures(aux_state::PartitionedStateStore &store);

struct SecEdgarQ24IncrementalTxnBuffer;
SecEdgarQ24MutationBatchFeatures BuildSecEdgarQ24MutationBatchFeatures(const aux_state::InvalidationPlan &plan,
                                                     const SecEdgarQ24IncrementalTxnBuffer &buffer, uint64_t rows_touched);

double SecEdgarQ24PartitionRebuildUs();
double SecEdgarQ24FullRebuildUs();
double SecEdgarQ24FirstQueryBlockingUs();
double SecEdgarQ24ApplyDeltaOverheadUs();
double SecEdgarQ24PartitionMergeUsPerDay();
double SecEdgarQ24RecentIncrementalSuccessRate();
void RecordSecEdgarQ24IncrementalSuccessObservation(bool success);
bool SecEdgarQ24HasSufficientTrainingSamples();

std::vector<aux_state::maintenance::ActionCandidate>
EnumerateSecEdgarQ24ActionCandidates(const SecEdgarQ24MutationBatchFeatures &features, const SecEdgarQ24WorkloadFeatures &workload,
                            const aux_state::maintenance::CostModelParams &params, bool low_confidence);

std::vector<SecEdgarQ24CandidateDecisionDetail>
EnumerateSecEdgarQ24ActionCandidatesDetailed(const SecEdgarQ24MutationBatchFeatures &features, const SecEdgarQ24WorkloadFeatures &workload,
                                    const aux_state::maintenance::CostModelParams &params, bool low_confidence);

bool SecEdgarQ24ApplyDeltaPlausible(const SecEdgarQ24MutationBatchFeatures &features, const SecEdgarQ24WorkloadFeatures &workload,
                           const aux_state::maintenance::CostModelParams &params, bool low_confidence);

aux_state::maintenance::SelectionResult
SelectSecEdgarQ24MaintenanceAction(const SecEdgarQ24MutationBatchFeatures &features, const SecEdgarQ24WorkloadFeatures &workload,
                          aux_state::maintenance::MaintenanceAction last_action);

} // namespace accelerator_codegen
