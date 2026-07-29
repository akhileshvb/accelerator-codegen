#include "accelerator_codegen/tpch_q3_maintenance.hpp"
#include "accelerator_codegen/aux_state/maintenance/maintenance_action.hpp"

#include <iostream>

int main() {
	accelerator_codegen::LoadQ3CostModelParams();
	auto selection = accelerator_codegen::SelectQ3MaintenanceAction(
	    {}, {}, accelerator_codegen::aux_state::maintenance::MaintenanceAction::NoOp);
	std::cout << "selected=" << static_cast<int>(selection.action) << '\n';
	return 0;
}
