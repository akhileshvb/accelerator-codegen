// SCAFFOLDING
#pragma once

#include <cstdint>
#include <vector>

#include "substrate/aux/maintenance/cost_model.hpp"
#include "substrate/aux/maintenance/maintenance_action.hpp"

namespace accelerator_codegen::aux_state::maintenance {

enum class ConfidenceTier : uint8_t { Low, Trained };

struct ActionCandidate {
	MaintenanceAction action = MaintenanceAction::NoOp;
	ActionCostEstimate estimate;
	bool supported = false;
};

struct SelectionInput {
	std::vector<ActionCandidate> candidates;
	ConfidenceTier confidence = ConfidenceTier::Low;
	MaintenanceAction last_action = MaintenanceAction::NoOp;
	double decision_margin_us = 1000.0;
};

struct SelectionResult {
	MaintenanceAction action = MaintenanceAction::NoOp;
	ConfidenceTier confidence = ConfidenceTier::Low;
	double selected_cost_us = 0.0;
};

SelectionResult SelectMinCostAction(const SelectionInput &input);

} // namespace accelerator_codegen::aux_state::maintenance
