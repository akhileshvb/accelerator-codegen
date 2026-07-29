#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/tpch_q10_maintenance.hpp"

#include <iostream>

int main() {
	try {
		accelerator_codegen::LoadQ10CostModelParams();
		auto candidates = accelerator_codegen::EnumerateQ10ActionCandidatesDetailed({}, {}, accelerator_codegen::GetQ10CostModelParams(), true);
		if (candidates.empty()) throw std::runtime_error("expected candidates");
		std::cout << "test_q10_action_failure passed\n";
		return 0;
	} catch (const std::exception &ex) {
		std::cerr << ex.what() << std::endl;
		return 1;
	}
}
