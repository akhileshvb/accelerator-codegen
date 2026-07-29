#include "accelerator_codegen/tpch_q6_maintenance.hpp"

#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "accelerator_codegen/aux_state/maintenance/maintenance_action.hpp"

namespace accelerator_codegen {

void MaybeAppendQ6DecisionCsv(const Q6MaintenanceDecisionRecord &record) {
	const char *path = std::getenv("ACCEL_Q6_DECISION_CSV");
	if (!path || !*path) return;
	std::ofstream out(path, std::ios::app);
	if (!out) return;
	static bool wrote_header = false;
	if (!wrote_header) {
		out << "selected,low_confidence,mutation_class,rows_touched,affected_partitions,expected_queries,"
		       "selected_predicted_total_us,selected_predicted_immediate_us,actual_immediate_us,prediction_error_immediate_us\n";
		wrote_header = true;
	}
	out << aux_state::maintenance::MaintenanceActionName(record.selected) << ',' << (record.low_confidence ? 1 : 0)
	    << ',' << Q6MutationClassName(record.mutation_class) << ',' << record.rows_touched << ','
	    << record.affected_partition_count << ',' << std::fixed << std::setprecision(3)
	    << record.expected_queries_before_next_write << ',' << record.selected_predicted_cost_us << ','
	    << record.selected_predicted_immediate_us << ',' << record.actual_cost_us << ',' << record.prediction_error_us
	    << '\n';
}

} // namespace accelerator_codegen
