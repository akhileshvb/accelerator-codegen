// SCAFFOLDING
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace accelerator_codegen {

//! Returns true when ACCEL_PARALLEL_PROFILE=1.
inline bool ParallelProfileEnabled() {
	const char *env = std::getenv("ACCEL_PARALLEL_PROFILE");
	return env && env[0] == '1';
}

using ProfileClock = std::chrono::steady_clock;

inline int64_t ProfileElapsedNs(const ProfileClock::time_point &start) {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(ProfileClock::now() - start).count();
}

//! Scan metrics — comparable across all parallel kernels.
struct ParallelScanMetrics {
	size_t rows_scanned = 0;
	size_t rows_after_storage_filter = 0;
	size_t rows_after_local_predicate = 0;
	size_t rows_decoded = 0;
	size_t payload_decoded_rejected = 0;
	size_t row_groups_considered = 0;
	size_t row_groups_skipped = 0;
	size_t varlen_bytes_inspected = 0;
};

//! State metrics — logical and allocated footprint.
struct ParallelStateMetrics {
	size_t logical_elements = 0;
	size_t element_width_bytes = 0;
	size_t logical_bytes = 0;
	size_t allocated_bytes = 0;
	size_t vector_capacity_bytes = 0;
	int64_t max_raw_key = 0;
	double domain_density = 0.0;
	size_t compact_cardinality = 0;
	size_t bitmap_bytes = 0;
	size_t hash_bucket_bytes = 0;
	double hash_load_factor = 0.0;
	size_t generation_bytes = 0;
	size_t touched_index_bytes = 0;
};

//! Per-worker replicated state.
struct ParallelWorkerReplicationMetrics {
	size_t per_worker_bytes = 0;
	size_t worker_count = 0;
	size_t total_worker_replicated_bytes = 0;
	//! fixed | scale_dependent | max_raw_key | qualifying_rows
	std::string replication_kind = "fixed";
};

//! Intermediate materialization.
struct ParallelIntermediateMetrics {
	size_t records_materialized = 0;
	size_t bytes_per_record = 0;
	size_t logical_intermediate_bytes = 0;
	size_t allocated_capacity_bytes = 0;
	size_t copies_per_record = 0;
	size_t concatenation_passes = 0;
	size_t sort_inputs = 0;
	size_t distinct_records = 0;
	size_t duplicate_records = 0;
};

//! Merge and finalization.
struct ParallelMergeMetrics {
	size_t entries_inspected = 0;
	size_t entries_merged = 0;
	size_t bytes_read = 0;
	size_t bytes_written = 0;
	size_t hash_entries_merged = 0;
	size_t sort_input_cardinality = 0;
	size_t comparison_count = 0;
	int64_t serial_merge_ns = 0;
	int64_t parallel_reduction_ns = 0;
	size_t output_candidates_before_limit = 0;
	size_t final_output_cardinality = 0;
};

//! Worker balance.
struct ParallelWorkerBalanceMetrics {
	std::vector<size_t> rows_per_worker;
	std::vector<size_t> qualifying_rows_per_worker;
	std::vector<size_t> state_entries_per_worker;
	std::vector<int64_t> elapsed_ns_per_worker;
	double max_to_avg_time = 0.0;
	double max_to_min_time = 0.0;
};

//! Unified scale-sensitive profile emitted by every parallel kernel.
struct ParallelScaleProfile {
	std::string query;
	size_t worker_count = 0;
	int64_t total_ns = 0;

	ParallelScanMetrics scan;
	ParallelStateMetrics state;
	ParallelWorkerReplicationMetrics worker_replication;
	ParallelIntermediateMetrics intermediate;
	ParallelMergeMetrics merge;
	ParallelWorkerBalanceMetrics balance;

	//! Phase timings (kernel-specific keys allowed as extensions).
	std::vector<std::pair<std::string, int64_t>> phase_ns;
};

inline void UpdateWorkerReplicationTotals(ParallelWorkerReplicationMetrics &m) {
	m.total_worker_replicated_bytes = m.per_worker_bytes * m.worker_count;
}

inline void ComputeWorkerBalanceRatios(ParallelWorkerBalanceMetrics &b) {
	if (b.elapsed_ns_per_worker.empty()) {
		return;
	}
	int64_t sum = 0;
	int64_t max_ns = 0;
	int64_t min_ns = b.elapsed_ns_per_worker[0];
	for (const auto ns : b.elapsed_ns_per_worker) {
		sum += ns;
		max_ns = std::max(max_ns, ns);
		min_ns = std::min(min_ns, ns);
	}
	const auto avg = static_cast<double>(sum) / static_cast<double>(b.elapsed_ns_per_worker.size());
	b.max_to_avg_time = avg > 0.0 ? static_cast<double>(max_ns) / avg : 0.0;
	b.max_to_min_time = min_ns > 0 ? static_cast<double>(max_ns) / static_cast<double>(min_ns) : 0.0;
}

inline void EmitParallelScaleProfile(const ParallelScaleProfile &p) {
	std::ostringstream out;
	out << "accel_scale_profile query=" << p.query << " worker_count=" << p.worker_count << " total_ns=" << p.total_ns;

	// Scan
	out << " scan_rows=" << p.scan.rows_scanned << " scan_rows_after_filter=" << p.scan.rows_after_storage_filter
	    << " scan_rows_after_predicate=" << p.scan.rows_after_local_predicate << " scan_rows_decoded=" << p.scan.rows_decoded
	    << " scan_payload_rejected=" << p.scan.payload_decoded_rejected
	    << " scan_row_groups=" << p.scan.row_groups_considered << " scan_row_groups_skipped=" << p.scan.row_groups_skipped
	    << " scan_varlen_bytes=" << p.scan.varlen_bytes_inspected;

	// State
	out << " state_logical_elements=" << p.state.logical_elements
	    << " state_element_width=" << p.state.element_width_bytes << " state_logical_bytes=" << p.state.logical_bytes
	    << " state_allocated_bytes=" << p.state.allocated_bytes
	    << " state_vector_capacity_bytes=" << p.state.vector_capacity_bytes << " state_max_raw_key=" << p.state.max_raw_key
	    << " state_domain_density=" << p.state.domain_density << " state_compact_cardinality=" << p.state.compact_cardinality
	    << " state_bitmap_bytes=" << p.state.bitmap_bytes << " state_hash_bucket_bytes=" << p.state.hash_bucket_bytes
	    << " state_hash_load_factor=" << p.state.hash_load_factor << " state_generation_bytes=" << p.state.generation_bytes
	    << " state_touched_index_bytes=" << p.state.touched_index_bytes;

	// Worker replication
	out << " worker_per_worker_bytes=" << p.worker_replication.per_worker_bytes
	    << " worker_count=" << p.worker_replication.worker_count
	    << " worker_total_replicated_bytes=" << p.worker_replication.total_worker_replicated_bytes
	    << " worker_replication_kind=" << p.worker_replication.replication_kind;

	// Intermediate
	out << " intermediate_records=" << p.intermediate.records_materialized
	    << " intermediate_bytes_per_record=" << p.intermediate.bytes_per_record
	    << " intermediate_logical_bytes=" << p.intermediate.logical_intermediate_bytes
	    << " intermediate_capacity_bytes=" << p.intermediate.allocated_capacity_bytes
	    << " intermediate_copies_per_record=" << p.intermediate.copies_per_record
	    << " intermediate_concat_passes=" << p.intermediate.concatenation_passes
	    << " intermediate_sort_inputs=" << p.intermediate.sort_inputs
	    << " intermediate_distinct=" << p.intermediate.distinct_records
	    << " intermediate_duplicates=" << p.intermediate.duplicate_records;

	// Merge
	out << " merge_entries_inspected=" << p.merge.entries_inspected << " merge_entries_merged=" << p.merge.entries_merged
	    << " merge_bytes_read=" << p.merge.bytes_read << " merge_bytes_written=" << p.merge.bytes_written
	    << " merge_hash_entries=" << p.merge.hash_entries_merged
	    << " merge_sort_input_cardinality=" << p.merge.sort_input_cardinality
	    << " merge_comparisons=" << p.merge.comparison_count << " merge_serial_ns=" << p.merge.serial_merge_ns
	    << " merge_parallel_reduction_ns=" << p.merge.parallel_reduction_ns
	    << " merge_output_candidates=" << p.merge.output_candidates_before_limit
	    << " merge_final_output=" << p.merge.final_output_cardinality;

	// Balance
	out << " balance_max_to_avg=" << p.balance.max_to_avg_time << " balance_max_to_min=" << p.balance.max_to_min_time;

	for (const auto &[phase, ns] : p.phase_ns) {
		out << " phase_" << phase << "_ns=" << ns;
	}

	std::cerr << out.str() << '\n';
}

} // namespace accelerator_codegen
