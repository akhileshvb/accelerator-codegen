#include "accelerator_codegen/tpch_q10_maintenance.hpp"
#include "substrate/aux/maintenance/maintenance_action.hpp"

#include <iostream>

int main() {
	accelerator_codegen::LoadQ10CostModelParams();
	auto selection = accelerator_codegen::SelectQ10MaintenanceAction(
	    {}, {}, accelerator_codegen::aux_state::maintenance::MaintenanceAction::NoOp);
	std::cout << "selected=" << static_cast<int>(selection.action) << '\n';
	return 0;
}
