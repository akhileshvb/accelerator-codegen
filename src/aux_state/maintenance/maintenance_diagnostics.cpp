#include "accelerator_codegen/aux_state/maintenance/maintenance_diagnostics.hpp"

#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <mutex>

namespace accelerator_codegen::aux_state::maintenance {
namespace {

std::mutex g_diag_mutex;
MaintenanceCounters g_counters;
std::deque<MaintenanceDecisionRecord> g_decision_log;
constexpr size_t kMaxDecisionLog = 256;

void AppendCsvRow(const MaintenanceDecisionRecord &record) {
	const char *path = std::getenv("ACCEL_Q14_DECISION_CSV");
	if (!path || !*path) {
		return;
	}
	std::ofstream out(path, std::ios::app);
	if (!out) {
		return;
	}
	out << MaintenanceActionName(record.selected) << ',' << (record.low_confidence ? 1 : 0) << ','
	    << std::fixed << std::setprecision(3) << record.selected_predicted_cost_us << ',' << record.actual_cost_us
	    << ',' << record.prediction_error_us << '\n';
}

} // namespace

void ResetMaintenanceCounters() {
	std::lock_guard<std::mutex> lock(g_diag_mutex);
	g_counters = MaintenanceCounters {};
	g_decision_log.clear();
}

const MaintenanceCounters &GetMaintenanceCounters() {
	return g_counters;
}

void RecordMaintenanceActionSelected(MaintenanceAction action, bool low_confidence) {
	std::lock_guard<std::mutex> lock(g_diag_mutex);
	const auto idx = static_cast<size_t>(action);
	if (idx < 5) {
		g_counters.action_selected[idx]++;
	}
	if (low_confidence) {
		g_counters.low_confidence_decisions++;
	}
}

void RecordMaintenancePredictionError(double error_us) {
	std::lock_guard<std::mutex> lock(g_diag_mutex);
	g_counters.prediction_error_samples++;
	g_counters.prediction_error_sum_us += error_us;
}

void AppendMaintenanceDecisionRecord(const MaintenanceDecisionRecord &record) {
	std::lock_guard<std::mutex> lock(g_diag_mutex);
	g_decision_log.push_back(record);
	while (g_decision_log.size() > kMaxDecisionLog) {
		g_decision_log.pop_front();
	}
}

const std::deque<MaintenanceDecisionRecord> &GetMaintenanceDecisionLog() {
	return g_decision_log;
}

void MaybeAppendDecisionCsv(const MaintenanceDecisionRecord &record) {
	AppendCsvRow(record);
}

} // namespace accelerator_codegen::aux_state::maintenance
