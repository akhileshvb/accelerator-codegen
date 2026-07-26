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
#include "accelerator_codegen/aux_state/maintenance/maintenance_action.hpp"
#include "accelerator_codegen/aux_state/partition_key.hpp"
#include "duckdb/accelerator/mutation_recorder.hpp"

namespace accelerator_codegen {

enum class Q14MutationClass : uint8_t {
	Unknown = 0,
	Irrelevant,
	PointLineitemUpdate,
	LineitemInsertDelete,
	ShipdateMove,
	BulkLineitem,
	PointPartTypeChange,
	BulkPartMutation,
	PartInsertDelete,
};

const char *Q14MutationClassName(Q14MutationClass class_id);

struct Q14MutationBatchFeatures {
	Q14MutationClass class_id = Q14MutationClass::Unknown;
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
	uint64_t estimated_part_fanout = 0;
	uint64_t estimated_lineitem_scan_rows = 0;
	uint64_t promo_op_count = 0;
	uint64_t captured_row_op_count = 0;
	bool whole_state_required = false;
	bool bulk_mutation = false;
	bool heavy_capture_available = false;
	double recent_incremental_success_rate = 1.0;
};

struct Q14CandidateDecisionDetail {
	aux_state::maintenance::MaintenanceAction action = aux_state::maintenance::MaintenanceAction::NoOp;
	bool supported = false;
	aux_state::maintenance::ActionCostEstimate estimate;
	double total_overhead_us = 0.0;
	std::string exclusion_reason;
};

struct Q14WorkloadFeatures {
	double q14_query_rate_eps = 0.0;
	double relevant_write_rate_eps = 0.0;
	double ewma_inter_write_interval_us = 0.0;
	double expected_queries_before_next_write = 0.0;
	uint64_t invalid_partition_count = 0;
	double ewma_fallback_latency_us = 50000.0;
	double ewma_aux_hit_latency_us = 500.0;
	uint32_t active_query_count = 0;
};

struct Q14ApplyDeltaEligibility {
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
	bool part_fanout_exceeds_max = false;
	bool training_samples_sufficient = false;
	uint64_t pending_light_ops = 0;
	uint64_t row_ops = 0;
	uint64_t promo_ops = 0;
	uint64_t bounded_part_key_fanout = 0;
	uint64_t estimated_lineitem_scan_rows = 0;
	double apply_delta_predicted_immediate_us = 0.0;
};

struct Q14MaintenanceDecisionRecord {
	aux_state::maintenance::MaintenanceAction selected = aux_state::maintenance::MaintenanceAction::NoOp;
	bool low_confidence = true;
	Q14MutationClass mutation_class = Q14MutationClass::Unknown;
	double selected_predicted_cost_us = 0.0;
	double selected_predicted_immediate_us = 0.0;
	double actual_cost_us = 0.0;
	double prediction_error_us = 0.0;
	double expected_queries_before_next_write = 0.0;
	uint64_t rows_touched = 0;
	uint64_t affected_partition_count = 0;
	bool stage1_selected_apply_delta = false;
	bool heavy_capture_ran = false;
	Q14ApplyDeltaEligibility apply_delta_eligibility;
	std::vector<std::pair<aux_state::maintenance::MaintenanceAction, double>> candidate_costs;
	std::vector<Q14CandidateDecisionDetail> candidate_details;
};

Q14ApplyDeltaEligibility BuildQ14ApplyDeltaEligibility(const Q14MutationBatchFeatures &features,
                                                       const aux_state::maintenance::CostModelParams &params,
                                                       bool low_confidence, uint64_t pending_light_ops,
                                                       uint64_t row_ops, uint64_t promo_ops);

const aux_state::maintenance::CostModelParams &GetQ14CostModelParams();
void LoadQ14CostModelParams();
void RecordQ14WorkloadQuery();
void RecordQ14WorkloadRelevantWrite();
void ResetQ14QueriesSinceLastWrite();
void RecordQ14LazyRebuildObservation(uint64_t rebuild_scan_us, uint64_t total_stall_us);

const std::deque<Q14MaintenanceDecisionRecord> &GetQ14MaintenanceDecisionLog();
void AppendQ14MaintenanceDecisionRecord(const Q14MaintenanceDecisionRecord &record);

namespace aux_state {
class PartitionedStateStore;
}
Q14WorkloadFeatures BuildQ14WorkloadFeatures(aux_state::PartitionedStateStore &store);

struct Q14IncrementalTxnBuffer;
Q14MutationBatchFeatures BuildQ14MutationBatchFeatures(const aux_state::InvalidationPlan &plan,
                                                     const Q14IncrementalTxnBuffer &buffer, uint64_t rows_touched);

double Q14PartitionRebuildUs();
double Q14FullRebuildUs();
double Q14FirstQueryBlockingUs();
double Q14ApplyDeltaOverheadUs();
double Q14RecentIncrementalSuccessRate();
void RecordQ14IncrementalSuccessObservation(bool success);
bool Q14HasSufficientTrainingSamples();

std::vector<aux_state::maintenance::ActionCandidate>
EnumerateQ14ActionCandidates(const Q14MutationBatchFeatures &features, const Q14WorkloadFeatures &workload,
                             const aux_state::maintenance::CostModelParams &params, bool low_confidence);

std::vector<Q14CandidateDecisionDetail>
EnumerateQ14ActionCandidatesDetailed(const Q14MutationBatchFeatures &features, const Q14WorkloadFeatures &workload,
                                     const aux_state::maintenance::CostModelParams &params, bool low_confidence);

bool Q14ApplyDeltaPlausible(const Q14MutationBatchFeatures &features, const Q14WorkloadFeatures &workload,
                            const aux_state::maintenance::CostModelParams &params, bool low_confidence);

bool Q14ShouldRunHeavyCapture(const Q14MutationBatchFeatures &features, const Q14WorkloadFeatures &workload,
                              const aux_state::maintenance::CostModelParams &params, bool low_confidence);

aux_state::maintenance::SelectionResult
SelectQ14MaintenanceAction(const Q14MutationBatchFeatures &features, const Q14WorkloadFeatures &workload,
                           aux_state::maintenance::MaintenanceAction last_action);

} // namespace accelerator_codegen
