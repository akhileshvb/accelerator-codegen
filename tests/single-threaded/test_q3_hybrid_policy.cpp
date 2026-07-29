#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/tpch_q3_maintenance.hpp"
#include "accelerator_codegen/aux_state/maintenance/maintenance_action.hpp"
#include "test_support/q3_aux_helpers.hpp"

#include <iostream>

#include "duckdb.hpp"

int main() {
	try {
		accelerator_codegen::test_support::EnableHybridMaintenance();
		accelerator_codegen::LoadQ3CostModelParams();
		auto selection = accelerator_codegen::SelectQ3MaintenanceAction(
		    {}, {}, accelerator_codegen::aux_state::maintenance::MaintenanceAction::NoOp);
		(void)selection;
		std::cout << "test_q3_hybrid_policy passed\n";
		return 0;
	} catch (const std::exception &ex) {
		std::cerr << ex.what() << std::endl;
		return 1;
	}
}
