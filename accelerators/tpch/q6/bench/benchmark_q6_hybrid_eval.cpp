#include "accelerator_codegen/tpch_q6_maintenance.hpp"
#include "substrate/aux/maintenance/maintenance_action.hpp"

#include <iostream>

int main() {
	accelerator_codegen::LoadQ6CostModelParams();
	auto selection = accelerator_codegen::SelectQ6MaintenanceAction(
	    {}, {}, accelerator_codegen::aux_state::maintenance::MaintenanceAction::NoOp);
	std::cout << "selected=" << static_cast<int>(selection.action) << '\n';
	return 0;
}
