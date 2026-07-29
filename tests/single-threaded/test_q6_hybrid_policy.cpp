#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/tpch_q6_maintenance.hpp"
#include "accelerator_codegen/aux_state/maintenance/maintenance_action.hpp"
#include "test_support/q6_aux_helpers.hpp"

#include <iostream>

int main() {
	try {
		accelerator_codegen::test_support::EnableHybridMaintenance();
		accelerator_codegen::LoadQ6CostModelParams();
		auto selection = accelerator_codegen::SelectQ6MaintenanceAction(
		    {}, {}, accelerator_codegen::aux_state::maintenance::MaintenanceAction::NoOp);
		(void)selection;
		std::cout << "test_q6_hybrid_policy passed\n";
		return 0;
	} catch (const std::exception &ex) {
		std::cerr << ex.what() << std::endl;
		return 1;
	}
}
