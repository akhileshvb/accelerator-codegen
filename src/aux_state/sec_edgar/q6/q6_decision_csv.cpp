#include "accelerator_codegen/sec_edgar_q6_diagnostics.hpp"

#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "accelerator_codegen/aux_state/maintenance/maintenance_action.hpp"

namespace accelerator_codegen {
namespace {

std::string EscapeCsvField(const std::string &value) {
	std::string escaped;
	escaped.reserve(value.size());
	for (const auto ch : value) {
		if (ch == '"') {
			escaped += "\"\"";
		} else {
			escaped.push_back(ch);
		}
	}
	return escaped;
}

} // namespace

void MaybeAppendSecEdgarQ6DecisionCsv(const SecEdgarQ6MaintenanceDecisionRecord &record) {
	const char *path = std::getenv("ACCEL_SecEdgarQ6_DECISION_CSV");
	if (!path || !*path) {
		return;
	}
	std::ofstream out(path, std::ios::app);
	if (!out) {
		return;
	}

	static bool wrote_header = false;
	if (!wrote_header) {
		out << "selected,low_confidence,mutation_class,rows_touched,affected_partitions,expected_queries,"
		       "selected_predicted_total_us,selected_predicted_immediate_us,actual_immediate_us,"
		       "prediction_error_immediate_us,heavy_capture_ran,apply_delta_eligible,apply_delta_exclusion,"
		       "row_ops,pending_light_ops,training_samples_sufficient,apply_delta_predicted_immediate_us,candidates\n";
		wrote_header = true;
	}

	std::ostringstream candidates;
	bool first = true;
	for (const auto &candidate : record.candidate_details) {
		if (!first) {
			candidates << ';';
		}
		first = false;
		candidates << aux_state::maintenance::MaintenanceActionName(candidate.action) << ':'
		             << (candidate.supported ? "supported" : "excluded") << ':'
		             << std::fixed << std::setprecision(1) << candidate.estimate.immediate_us << '+'
		             << candidate.estimate.differential_future_query_us << '='
		             << candidate.total_overhead_us;
		if (!candidate.exclusion_reason.empty()) {
			candidates << ':' << candidate.exclusion_reason;
		}
	}

	out << aux_state::maintenance::MaintenanceActionName(record.selected) << ',' << (record.low_confidence ? 1 : 0)
	    << ',' << SecEdgarQ6MutationClassName(record.mutation_class) << ',' << record.rows_touched << ','
	    << record.affected_partition_count << ',' << std::fixed << std::setprecision(3)
	    << record.expected_queries_before_next_write << ',' << record.selected_predicted_cost_us << ','
	    << record.selected_predicted_immediate_us << ',' << record.actual_cost_us << ',' << record.prediction_error_us
	    << ',' << (record.heavy_capture_ran ? 1 : 0) << ',' << (record.apply_delta_eligibility.eligible ? 1 : 0)
	    << ','
	    << (record.apply_delta_eligibility.primary_exclusion_reason
	            ? record.apply_delta_eligibility.primary_exclusion_reason
	            : "")
	    << ',' << record.apply_delta_eligibility.row_ops << ','
	    << record.apply_delta_eligibility.pending_light_ops << ','
	    << (record.apply_delta_eligibility.training_samples_sufficient ? 1 : 0) << ','
	    << record.apply_delta_eligibility.apply_delta_predicted_immediate_us << ",\"" << EscapeCsvField(candidates.str()) << "\"\n";
}

} // namespace accelerator_codegen
