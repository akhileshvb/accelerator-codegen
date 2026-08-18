// SCAFFOLDING
#include "accelerator_codegen/stateful/maintenance/action_selector.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace accelerator_codegen::aux_state::maintenance {
namespace {

int ActionPriority(MaintenanceAction action) {
	switch (action) {
	case MaintenanceAction::ApplyDelta:
		return 0;
	case MaintenanceAction::RebuildPartition:
		return 1;
	case MaintenanceAction::RebuildAll:
		return 2;
	case MaintenanceAction::InvalidateAndFallback:
		return 3;
	case MaintenanceAction::NoOp:
		return 4;
	}
	return 5;
}

bool IsConservativeAction(MaintenanceAction action) {
	return action == MaintenanceAction::InvalidateAndFallback || action == MaintenanceAction::RebuildPartition ||
	       action == MaintenanceAction::RebuildAll;
}

} // namespace

SelectionResult SelectMinCostAction(const SelectionInput &input) {
	SelectionResult result;
	result.confidence = input.confidence;

	std::vector<const ActionCandidate *> supported;
	for (const auto &candidate : input.candidates) {
		if (candidate.supported) {
			supported.push_back(&candidate);
		}
	}
	if (supported.empty()) {
		result.action = MaintenanceAction::InvalidateAndFallback;
		return result;
	}

	if (input.confidence == ConfidenceTier::Low) {
		const ActionCandidate *best = nullptr;
		for (const auto *candidate : supported) {
			if (!IsConservativeAction(candidate->action)) {
				continue;
			}
			if (!best || candidate->estimate.TotalOverheadUs() < best->estimate.TotalOverheadUs() ||
			    (std::fabs(candidate->estimate.TotalOverheadUs() - best->estimate.TotalOverheadUs()) <=
			         input.decision_margin_us &&
			     ActionPriority(candidate->action) > ActionPriority(best->action))) {
				best = candidate;
			}
		}
		if (best) {
			result.action = best->action;
			result.selected_cost_us = best->estimate.TotalOverheadUs();
			return result;
		}
	}

	const ActionCandidate *best = nullptr;
	const ActionCandidate *second_best = nullptr;
	for (const auto *candidate : supported) {
		const auto cost = candidate->estimate.TotalOverheadUs();
		if (!best || cost < best->estimate.TotalOverheadUs() ||
		    (std::fabs(cost - best->estimate.TotalOverheadUs()) <= input.decision_margin_us &&
		     ActionPriority(candidate->action) < ActionPriority(best->action))) {
			second_best = best;
			best = candidate;
			continue;
		}
		if (!second_best || cost < second_best->estimate.TotalOverheadUs()) {
			second_best = candidate;
		}
	}

	if (best && input.last_action != MaintenanceAction::NoOp && second_best &&
	    input.last_action != best->action &&
	    best->estimate.TotalOverheadUs() + input.decision_margin_us >= second_best->estimate.TotalOverheadUs()) {
		for (const auto *candidate : supported) {
			if (candidate->action == input.last_action) {
				best = candidate;
				break;
			}
		}
	}

	result.action = best->action;
	result.selected_cost_us = best->estimate.TotalOverheadUs();
	return result;
}

} // namespace accelerator_codegen::aux_state::maintenance
