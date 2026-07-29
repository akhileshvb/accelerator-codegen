#include "accelerator_codegen/tpch_q12_maintenance.hpp"
#include "accelerator_codegen/aux_state/maintenance/maintenance_action.hpp"

#include <iostream>

int main() {
	accelerator_codegen::LoadQ12CostModelParams();
	auto selection = accelerator_codegen::SelectQ12MaintenanceAction(
	    {}, {}, accelerator_codegen::aux_state::maintenance::MaintenanceAction::NoOp);
	std::cout << "selected=" << static_cast<int>(selection.action) << '\n';
	return 0;
}
